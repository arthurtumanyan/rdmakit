/*
 * test_aof.c - append-only log (src/aof.c).
 *
 * - round trip: random operations (all store modes, increments, deletes, touches, expiry, eviction,
 *   flush) on an engine whose change feed goes to the log; a new engine loaded from the log holds exactly
 *   the same keys, values, flags and expiry times
 * - rewrites while other threads keep changing data, and automatic rewrites: the reloaded state is exact,
 *   and old files are removed
 * - a torn last block is cut off (records before it survive); a damaged block before the end refuses the
 *   start, or with load_damaged loads the records before it
 * - transactions: records with a transaction id apply only with their end record; an incomplete
 *   transaction at the end is dropped
 * - durable sequence numbers with the "always" policy; log files without a manifest are refused
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aof.h"
#include "srv.h"

static int failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static char base_dir[256];

/* The log that receives the change feed of the engine under test. */
static aof *_Atomic target;

static void feed(void *arg, const akv_change *c) {
    (void)arg;
    aof *a = target;
    if (a != NULL) {
        aof_append(a, c);
    }
}

static akv *engine(uint64_t capacity, int with_feed) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = capacity;
    cfg.shards = 4;
    cfg.hash_seed = 99;
    if (with_feed) {
        cfg.on_change = feed;
    }
    return akv_create(&cfg);
}

static aof_config config(const char *dir) {
    aof_config c = {.dir = dir, .fsync = AOF_FSYNC_EVERYSEC};
    return c;
}

static size_t key_of(unsigned id, char *key) {
    return (size_t)snprintf(key, 32, "%s-%u", id % 5 == 0 ? "n" : "k", id);
}

static void random_op(akv *s, unsigned id) {
    char key[32];
    size_t klen = key_of(id, key);
    unsigned char val[400];
    size_t vlen = 1 + rnd() % sizeof(val);
    for (size_t i = 0; i < vlen; i++) {
        val[i] = (unsigned char)rnd();
    }
    unsigned pick = (unsigned)(rnd() % 100);
    akv_store_opts o = {.ttl_ms = pick < 70 ? 0 : pick < 95 ? 60000 : -1, .flags = (uint32_t)rnd()};
    unsigned op = (unsigned)(rnd() % 100);
    if (key[0] == 'n' && op < 50) {
        int64_t r;
        if (akv_incr(s, key, klen, (int64_t)(rnd() % 50), AKV_INCR_SIGNED, &r, NULL) == -EINVAL) {
            akv_store(s, key, klen, "1", 1, &o, NULL);
        }
    } else if (op < 40) {
        akv_store(s, key, klen, val, vlen, &o, NULL);
    } else if (op < 50) {
        o.mode = op < 45 ? AKV_APPEND : AKV_ADD;
        akv_store(s, key, klen, val, vlen % 30 + 1, &o, NULL);
    } else if (op < 62) {
        akv_del(s, key, klen);
    } else if (op < 68) {
        akv_touch(s, key, klen, o.ttl_ms);
    } else {
        unsigned char out[1024];
        size_t n;
        akv_get(s, key, klen, out, sizeof(out), &n);
    }
}

/* Number of keys of [0, keys) whose visible state differs between a and b. */
static unsigned differences(akv *a, akv *b, unsigned keys) {
    unsigned bad = 0;
    for (unsigned id = 0; id < keys; id++) {
        char key[32];
        size_t klen = key_of(id, key);
        static unsigned char va[4096];
        static unsigned char vb[4096];
        size_t na = 0;
        size_t nb = 0;
        akv_meta ma;
        akv_meta mb;
        int ra = akv_get_meta(a, key, klen, va, sizeof(va), &na, &ma, AKV_TTL_KEEP);
        int rb = akv_get_meta(b, key, klen, vb, sizeof(vb), &nb, &mb, AKV_TTL_KEEP);
        int same = ra == rb;
        if (same && ra == 0) {
            int64_t dt = ma.ttl_ms - mb.ttl_ms;
            same = na == nb && memcmp(va, vb, na) == 0 && ma.flags == mb.flags &&
                   (ma.ttl_ms < 0) == (mb.ttl_ms < 0) && (ma.ttl_ms < 0 || (dt < 200 && dt > -200));
        }
        if (!same && bad++ < 3) {
            fprintf(stderr, "key %s differs (rc %d/%d, len %zu/%zu)\n", key, ra, rb, na, nb);
        }
    }
    return bad;
}

static void make_dir(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", base_dir, name);
}

/* Loads the log in dir into a new engine. */
static akv *reload(const char *dir, aof_load_info *li, int *rc_out, int load_damaged) {
    akv *s = engine(64 << 20, 0);
    aof_config c = config(dir);
    c.load_damaged = load_damaged;
    aof *a = NULL;
    int rc = aof_open(&c, s, 1, &a, li);
    if (rc_out != NULL) {
        *rc_out = rc;
    }
    if (rc == 0) {
        aof_close(a);
    }
    return s;
}

static unsigned count_files(const char *dir) {
    DIR *d = opendir(dir);
    unsigned n = 0;
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        n += strstr(e->d_name, ".aof") != NULL;
    }
    if (d != NULL) {
        closedir(d);
    }
    return n;
}

static void test_roundtrip(uint64_t capacity, unsigned keys, unsigned ops) {
    char dir[300];
    make_dir(dir, sizeof(dir), "roundtrip");
    akv *s = engine(capacity, 1);
    aof_config c = config(dir);
    aof_load_info li;
    aof *a = NULL;
    EXPECT(aof_open(&c, s, 1, &a, &li) == 0 && li.created, "new log");
    target = a;
    for (unsigned i = 0; i < ops; i++) {
        random_op(s, (unsigned)(rnd() % keys));
        if (i == ops / 3) {
            akv_flush(s);
        }
    }
    target = NULL;
    EXPECT(aof_close(a) == 0, "close");
    int rc;
    akv *r = reload(dir, &li, &rc, 0);
    EXPECT(rc == 0 && li.files == 2, "reload: rc %d, %llu files", rc, (unsigned long long)li.files);
    unsigned bad = differences(s, r, keys);
    EXPECT(bad == 0, "round trip at %llu KiB: %u keys differ", (unsigned long long)(capacity >> 10), bad);
    printf("round trip: capacity %llu KiB, %llu records\n", (unsigned long long)(capacity >> 10),
           (unsigned long long)li.records);
    akv_destroy(s);
    akv_destroy(r);
}

struct churn {
    akv *s;
    unsigned keys;
    unsigned ops;
    uint64_t seed;
};

static void *churn_main(void *p) {
    struct churn *c = p;
    for (unsigned i = 0; i < c->ops; i++) {
        c->seed ^= c->seed << 13;
        c->seed ^= c->seed >> 7;
        c->seed ^= c->seed << 17;
        char key[32];
        size_t klen = key_of((unsigned)(c->seed % c->keys), key);
        char val[64];
        int n = snprintf(val, sizeof(val), "v%llu", (unsigned long long)c->seed);
        if (c->seed % 10 == 0) {
            akv_del(c->s, key, klen);
        } else {
            akv_store(c->s, key, klen, val, (size_t)n, NULL, NULL);
        }
    }
    return NULL;
}

static void test_rewrite_under_load(void) {
    char dir[300];
    make_dir(dir, sizeof(dir), "rewrite");
    akv *s = engine(64 << 20, 1);
    aof_config c = config(dir);
    c.rewrite_percentage = 50;
    c.rewrite_min_size = 256 << 10;
    aof_load_info li;
    aof *a = NULL;
    EXPECT(aof_open(&c, s, 1, &a, &li) == 0, "open");
    target = a;
    enum { THREADS = 4 };
    pthread_t th[THREADS];
    struct churn ch[THREADS];
    for (unsigned i = 0; i < THREADS; i++) {
        ch[i] = (struct churn){.s = s, .keys = 20000, .ops = 150000, .seed = 1234567u * (i + 1)};
        pthread_create(&th[i], NULL, churn_main, &ch[i]);
    }
    unsigned started = 0;
    for (int i = 0; i < 40; i++) {
        started += aof_rewrite(a) == 0;
        sleep_ms(5);
    }
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    aof_info ai;
    for (int i = 0; i < 500; i++) {
        aof_info_get(a, &ai);
        if (!ai.rewrite_running) {
            break;
        }
        sleep_ms(10);
    }
    target = NULL;
    EXPECT(aof_close(a) == 0, "close");
    EXPECT(ai.rewrites >= 1 && ai.last_rewrite_ok, "rewrites completed: %llu (%u requested)",
           (unsigned long long)ai.rewrites, started);
    EXPECT(count_files(dir) == 2, "old files removed: %u log files remain", count_files(dir));
    int rc;
    akv *r = reload(dir, &li, &rc, 0);
    unsigned bad = differences(s, r, 20000);
    EXPECT(rc == 0 && bad == 0, "after rewrites under load: rc %d, %u keys differ", rc, bad);
    printf("rewrite under load: %llu rewrites, %llu records reloaded\n", (unsigned long long)ai.rewrites,
           (unsigned long long)li.records);
    akv_destroy(s);
    akv_destroy(r);
}

/* Appends bytes to the last increment file of the log in dir. */
static void append_to_incr(const char *dir, const void *p, size_t n) {
    char man[400];
    snprintf(man, sizeof(man), "%s/manifest", dir);
    FILE *f = fopen(man, "r");
    char line[128];
    char last[128] = "";
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "incr ", 5) == 0) {
            snprintf(last, sizeof(last), "%.*s", (int)strcspn(line + 5, "\n"), line + 5);
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", dir, last);
    int fd = open(path, O_WRONLY | O_APPEND);
    EXPECT(fd >= 0 && write(fd, p, n) == (ssize_t)n, "append to %s", path);
    close(fd);
}

static void test_damage(void) {
    char dir[300];
    make_dir(dir, sizeof(dir), "damage");
    akv *s = engine(64 << 20, 1);
    aof_config c = config(dir);
    aof_load_info li;
    aof *a = NULL;
    aof_open(&c, s, 1, &a, &li);
    target = a;
    for (int i = 0; i < 1000; i++) {
        char key[32];
        size_t n = key_of((unsigned)i, key);
        akv_put(s, key, n, "x", 1);
    }
    target = NULL;
    aof_close(a);

    /* torn tail: a block header promising more bytes than follow */
    uint8_t torn[40] = {'A', 'L', 'G', '1', 3, 0, 0, 0, 0, 1, 0, 0};
    append_to_incr(dir, torn, sizeof(torn));
    int rc;
    akv *r = reload(dir, &li, &rc, 0);
    EXPECT(rc == 0 && li.truncated == sizeof(torn), "torn tail cut off (rc %d, %llu bytes)", rc,
           (unsigned long long)li.truncated);
    EXPECT(differences(s, r, 1000) == 0, "records before the torn tail");
    akv_destroy(r);
    r = reload(dir, &li, &rc, 0);
    EXPECT(rc == 0 && li.truncated == 0, "file usable after the cut");
    akv_destroy(r);

    /* zero-filled tail (a file extended by the file system but never written) */
    uint8_t zeros[4096] = {0};
    append_to_incr(dir, zeros, sizeof(zeros));
    r = reload(dir, &li, &rc, 0);
    EXPECT(rc == 0 && li.truncated == sizeof(zeros), "zero tail cut off (rc %d)", rc);
    akv_destroy(r);

    /* damage inside the base: refused, or loaded up to the damage with load_damaged */
    char path[400];
    snprintf(path, sizeof(path), "%s/base-1.aof", dir);
    struct stat st;
    stat(path, &st);
    EXPECT(st.st_size == 24, "empty base: %lld bytes", (long long)st.st_size);
    snprintf(path, sizeof(path), "%s/incr-2.aof", dir);
    int fd = open(path, O_RDWR);
    uint8_t byte;
    EXPECT(pread(fd, &byte, 1, 100) == 1, "read");
    byte ^= 0x5a;
    EXPECT(pwrite(fd, &byte, 1, 100) == 1, "corrupt one byte of the first block");
    close(fd);
    /* The damaged block is the only one; add a valid later file so that it is not the last. */
    char man[400];
    snprintf(man, sizeof(man), "%s/manifest", dir);
    FILE *f = fopen(man, "a");
    fputs("incr incr-3.aof\n", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/incr-3.aof", dir);
    fd = open(path, O_WRONLY | O_CREAT, 0600);
    uint8_t hdr[24] = {'A', 'K', 'V', 'L', 'O', 'G', '0', '1', 1, 0, 0, 0, 1, 0, 0, 0};
    EXPECT(write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr), "header");
    close(fd);
    r = reload(dir, &li, &rc, 0);
    EXPECT(rc == -EBADMSG, "damage before the end refuses the load (rc %d)", rc);
    akv_destroy(r);
    r = reload(dir, &li, &rc, 1);
    EXPECT(rc == 0 && li.damaged, "load_damaged starts anyway (rc %d)", rc);
    akv_destroy(r);
    akv_destroy(s);
}

static void test_transactions(void) {
    char dir[300];
    make_dir(dir, sizeof(dir), "txn");
    akv *s = engine(4 << 20, 0);
    aof_config c = config(dir);
    aof_load_info li;
    aof *a = NULL;
    aof_open(&c, s, 1, &a, &li);
    akv_change put = {.kind = AKV_CHG_PUT, .key = "a", .klen = 1, .val = "1", .vlen = 1, .txn = 7};
    aof_append(a, &put);
    put.key = "b";
    aof_append(a, &put);
    aof_append_txn_end(a, 7);
    put.key = "c";
    put.txn = 8; /* never ended */
    aof_append(a, &put);
    put.key = "d";
    put.txn = 0;
    aof_append(a, &put);
    aof_close(a);
    int rc;
    akv *r = reload(dir, &li, &rc, 0);
    EXPECT(rc == 0 && akv_exists(r, "a", 1) && akv_exists(r, "b", 1) && akv_exists(r, "d", 1),
           "complete transaction and plain record applied");
    EXPECT(!akv_exists(r, "c", 1) && li.dropped_txn == 1, "incomplete transaction dropped (%llu)",
           (unsigned long long)li.dropped_txn);
    akv_destroy(r);
    akv_destroy(s);
}

static void test_always(void) {
    char dir[300];
    make_dir(dir, sizeof(dir), "always");
    akv *s = engine(4 << 20, 1);
    aof_config c = config(dir);
    c.fsync = AOF_FSYNC_ALWAYS;
    aof_load_info li;
    aof *a = NULL;
    aof_open(&c, s, 1, &a, &li);
    target = a;
    uint64_t before = aof_thread_seq();
    akv_put(s, "x", 1, "1", 1);
    uint64_t seq = aof_thread_seq();
    EXPECT(seq != before && seq >= 1, "the thread's sequence number moves with its record");
    int waited = 0;
    while (aof_durable(a) < seq && waited < 2000) {
        sleep_ms(1);
        waited++;
    }
    aof_info ai;
    aof_info_get(a, &ai);
    EXPECT(aof_durable(a) >= seq && ai.fsyncs >= 1, "durable after a sync (%d ms, %llu fsyncs)", waited,
           (unsigned long long)ai.fsyncs);
    target = NULL;
    aof_close(a);
    akv_destroy(s);

    /* log files without a manifest */
    char man[400];
    snprintf(man, sizeof(man), "%s/manifest", dir);
    unlink(man);
    int rc;
    s = reload(dir, &li, &rc, 0);
    EXPECT(rc == -EBADMSG, "files without a manifest are refused (rc %d)", rc);
    akv_destroy(s);
}

static void remove_tree(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p);
        } else {
            unlink(p);
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    rmdir(dir);
}

int main(void) {
    snprintf(base_dir, sizeof(base_dir), "%s/akv-aof-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (mkdtemp(base_dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    char dir[300];
    test_roundtrip(64 << 20, 3000, 150000);
    make_dir(dir, sizeof(dir), "roundtrip");
    remove_tree(dir);
    test_roundtrip(256 << 10, 5000, 200000); /* eviction records */
    test_rewrite_under_load();
    test_damage();
    test_transactions();
    test_always();
    remove_tree(base_dir);
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_aof: ok");
    return 0;
}
