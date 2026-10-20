/*
 * test_akv.c - engine tests.
 *
 * - basic operations and argument validation
 * - model check against a reference map: exact results when memory is ample; under eviction a hit must
 *   always return the latest stored version (never a stale or foreign value)
 * - arena wrap-around with small gaps (tiny shard arena, entry sizes that leave 8 and 16 byte gaps)
 * - second chance: a frequently read entry survives while unread neighbors are evicted
 * - multi-threaded stress with self-describing values (run under ThreadSanitizer as well)
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "akv.h"

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

static uint64_t rng_state = 88172645463325252ull;

static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Value layout: key id (8), version (8), length (4), then a pattern derived from all three. */
static size_t make_value(unsigned char *buf, uint64_t id, uint64_t version, uint32_t len) {
    if (len < 20) {
        len = 20;
    }
    memcpy(buf, &id, 8);
    memcpy(buf + 8, &version, 8);
    memcpy(buf + 16, &len, 4);
    uint8_t seed = (uint8_t)(id * 31 + version * 17 + len);
    for (uint32_t i = 20; i < len; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
    return len;
}

/* Verifies a received value; returns the version, or UINT64_MAX when the content is inconsistent. */
static uint64_t check_value(const unsigned char *buf, size_t n, uint64_t id) {
    if (n < 20) {
        return UINT64_MAX;
    }
    uint64_t gid;
    uint64_t version;
    uint32_t len;
    memcpy(&gid, buf, 8);
    memcpy(&version, buf + 8, 8);
    memcpy(&len, buf + 16, 4);
    if (gid != id || len != n) {
        return UINT64_MAX;
    }
    uint8_t seed = (uint8_t)(id * 31 + version * 17 + len);
    for (uint32_t i = 20; i < len; i++) {
        if (buf[i] != (uint8_t)(seed + i)) {
            return UINT64_MAX;
        }
    }
    return version;
}

static void key_of(uint64_t id, char *key, size_t *klen) {
    *klen = (size_t)snprintf(key, 32, "key-%llu", (unsigned long long)id);
}

static akv *make(uint64_t capacity, uint32_t shards) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = capacity;
    cfg.shards = shards;
    cfg.avg_entry = 128;
    cfg.hash_seed = 12345;
    akv *s = akv_create(&cfg);
    EXPECT(s != NULL, "create capacity=%llu shards=%u", (unsigned long long)capacity, shards);
    return s;
}

static void test_basic(void) {
    akv *s = make(1 << 20, 4);
    unsigned char buf[256];
    size_t n = 0;
    EXPECT(akv_get(s, "a", 1, buf, sizeof(buf), &n) == -ENOENT, "missing key");
    EXPECT(akv_put(s, "a", 1, "one", 3) == 0, "put");
    EXPECT(akv_get(s, "a", 1, buf, sizeof(buf), &n) == 0 && n == 3 && memcmp(buf, "one", 3) == 0, "get");
    EXPECT(akv_put(s, "a", 1, "second", 6) == 0, "overwrite");
    EXPECT(akv_get(s, "a", 1, buf, sizeof(buf), &n) == 0 && n == 6 && memcmp(buf, "second", 6) == 0,
           "overwritten value");
    EXPECT(akv_put(s, "e", 1, "", 0) == 0, "empty value");
    EXPECT(akv_get(s, "e", 1, buf, sizeof(buf), &n) == 0 && n == 0, "empty value read");
    EXPECT(akv_get(s, "a", 1, buf, 2, &n) == -EMSGSIZE && n == 6, "buffer too small reports the size");
    EXPECT(akv_del(s, "a", 1) == 0, "delete");
    EXPECT(akv_del(s, "a", 1) == -ENOENT, "delete twice");
    EXPECT(akv_get(s, "a", 1, buf, sizeof(buf), &n) == -ENOENT, "deleted key");
    EXPECT(akv_put(s, "", 0, "x", 1) == -EINVAL, "empty key rejected");
    EXPECT(akv_get(s, "", 0, buf, sizeof(buf), &n) == -EINVAL, "empty key get");
    char longkey[70000];
    memset(longkey, 'k', sizeof(longkey));
    EXPECT(akv_put(s, longkey, 65536, "x", 1) == -EINVAL, "key above 65535 bytes rejected");
    static unsigned char big[600000];
    EXPECT(akv_put(s, "big", 3, big, sizeof(big)) == -E2BIG, "value above half a shard rejected");
    akv_stats st;
    akv_stats_get(s, &st);
    EXPECT(st.entries == 1, "entries %llu", (unsigned long long)st.entries);
    EXPECT(st.rejected == 1, "rejected %llu", (unsigned long long)st.rejected);
    akv_destroy(s);

    akv_config bad;
    akv_config_default(&bad);
    bad.shards = 3;
    EXPECT(akv_create(&bad) == NULL, "shard count must be a power of two");
    bad.shards = 64;
    bad.capacity = 100;
    EXPECT(akv_create(&bad) == NULL, "capacity too small");
}

/* Random operations against a reference model. */
static void run_model(uint64_t capacity, uint32_t shards, unsigned keys, unsigned max_len, unsigned ops,
                      int strict) {
    akv *s = make(capacity, shards);
    uint64_t *version = calloc(keys, sizeof(*version)); /* 0 = absent */
    uint64_t next_version = 1;
    unsigned char *buf = malloc(max_len + 64);
    unsigned char *out = malloc(max_len + 64);
    uint64_t hits = 0;
    uint64_t gets = 0;
    for (unsigned i = 0; i < ops; i++) {
        uint64_t id = rnd() % keys;
        char key[32];
        size_t klen;
        key_of(id, key, &klen);
        unsigned op = (unsigned)(rnd() % 100);
        if (op < 40) {
            uint64_t v = next_version++;
            size_t n = make_value(buf, id, v, 20 + (uint32_t)(rnd() % (max_len - 20)));
            int rc = akv_put(s, key, klen, buf, n);
            EXPECT(rc == 0, "put rc %d", rc);
            version[id] = v;
        } else if (op < 95) {
            size_t n = 0;
            int rc = akv_get(s, key, klen, out, max_len + 64, &n);
            gets++;
            if (rc == 0) {
                hits++;
                uint64_t v = check_value(out, n, id);
                EXPECT(v != UINT64_MAX, "corrupt value for key %llu", (unsigned long long)id);
                EXPECT(v == version[id],
                       "stale or foreign value: key %llu returned version %llu, latest %llu",
                       (unsigned long long)id, (unsigned long long)v, (unsigned long long)version[id]);
            } else {
                EXPECT(rc == -ENOENT, "get rc %d", rc);
                EXPECT(!strict || version[id] == 0, "key %llu lost although memory is ample",
                       (unsigned long long)id);
            }
        } else {
            int rc = akv_del(s, key, klen);
            if (strict) {
                EXPECT((rc == 0) == (version[id] != 0), "delete result %d with model version %llu", rc,
                       (unsigned long long)version[id]);
            }
            version[id] = 0;
        }
    }
    akv_stats st;
    akv_stats_get(s, &st);
    uint64_t live = 0;
    for (unsigned i = 0; i < keys; i++) {
        live += version[i] != 0;
    }
    if (strict) {
        EXPECT(st.entries == live, "entries %llu, model %llu", (unsigned long long)st.entries,
               (unsigned long long)live);
        EXPECT(st.evictions == 0 && st.index_evictions == 0, "unexpected evictions %llu/%llu",
               (unsigned long long)st.evictions, (unsigned long long)st.index_evictions);
    } else {
        EXPECT(st.entries <= live + 0 || st.entries <= keys, "entries %llu above key count",
               (unsigned long long)st.entries);
        EXPECT(st.entries <= keys, "entry counter wrapped: %llu", (unsigned long long)st.entries);
        EXPECT(st.evictions > 0, "no eviction happened; the test does not exercise eviction");
    }
    EXPECT(st.log_bytes <= st.arena_bytes, "log %llu above arena %llu", (unsigned long long)st.log_bytes,
           (unsigned long long)st.arena_bytes);
    printf("model: capacity %llu KiB, %u keys: %llu of %llu gets hit, %llu evictions, %llu reinsertions, "
           "%llu index evictions\n",
           (unsigned long long)(capacity >> 10), keys, (unsigned long long)hits, (unsigned long long)gets,
           (unsigned long long)st.evictions, (unsigned long long)st.reinsertions,
           (unsigned long long)st.index_evictions);
    free(version);
    free(buf);
    free(out);
    akv_destroy(s);
}

static void test_second_chance(void) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = 64 << 10;
    cfg.shards = 1;
    cfg.avg_entry = 1024;
    cfg.hash_seed = 9;
    akv *s = akv_create(&cfg);
    unsigned char val[1000];
    unsigned char out[1100];
    size_t n;
    memset(val, 7, sizeof(val));
    for (unsigned i = 0; i < 40; i++) {
        char key[16];
        size_t klen = (size_t)snprintf(key, sizeof(key), "k%u", i);
        EXPECT(akv_put(s, key, klen, val, sizeof(val)) == 0, "fill");
    }
    EXPECT(akv_get(s, "k0", 2, out, sizeof(out), &n) == 0, "k0 present before pressure");
    for (unsigned i = 40; i < 400; i++) {
        char key[16];
        size_t klen = (size_t)snprintf(key, sizeof(key), "k%u", i);
        EXPECT(akv_put(s, key, klen, val, sizeof(val)) == 0, "pressure");
        if (i % 4 == 0) {
            akv_get(s, "k0", 2, out, sizeof(out), &n); /* keep k0 hot */
        }
    }
    EXPECT(akv_get(s, "k0", 2, out, sizeof(out), &n) == 0, "hot entry must survive eviction");
    EXPECT(akv_get(s, "k1", 2, out, sizeof(out), &n) == -ENOENT, "cold neighbor must be evicted");
    akv_stats st;
    akv_stats_get(s, &st);
    EXPECT(st.reinsertions > 0, "no reinsertion recorded");
    EXPECT(st.evictions > 0, "no eviction recorded");
    akv_destroy(s);
}

/* Same-size overwrites reuse the entry: the log does not grow, other sizes append. */
static void test_inplace(void) {
    akv *s = make(1 << 20, 1);
    unsigned char val[256];
    unsigned char out[512];
    size_t n;
    memset(val, 1, sizeof(val));
    EXPECT(akv_put(s, "key", 3, val, 200) == 0, "first put");
    akv_stats st0;
    akv_stats_get(s, &st0);
    for (unsigned i = 0; i < 1000; i++) {
        memset(val, (int)(i & 0xff), 200);
        EXPECT(akv_put(s, "key", 3, val, 200) == 0, "same-size overwrite");
    }
    akv_stats st1;
    akv_stats_get(s, &st1);
    EXPECT(st1.log_bytes == st0.log_bytes, "log grew from %llu to %llu bytes",
           (unsigned long long)st0.log_bytes, (unsigned long long)st1.log_bytes);
    EXPECT(st1.overwrites == 1000 && st1.entries == 1, "overwrites %llu entries %llu",
           (unsigned long long)st1.overwrites, (unsigned long long)st1.entries);
    EXPECT(akv_get(s, "key", 3, out, sizeof(out), &n) == 0 && n == 200 && out[0] == (999 & 0xff) &&
               out[199] == (999 & 0xff),
           "latest content after in-place overwrites");
    /* Sizes that round to the same total (padding) also match; a different size appends. */
    EXPECT(akv_put(s, "key", 3, val, 201) == 0, "size within the same 8-byte unit");
    EXPECT(akv_get(s, "key", 3, out, sizeof(out), &n) == 0 && n == 201, "length after same-total overwrite");
    EXPECT(akv_put(s, "key", 3, val, 100) == 0, "smaller value");
    akv_stats st2;
    akv_stats_get(s, &st2);
    EXPECT(st2.log_bytes > st1.log_bytes, "different total size must append");
    EXPECT(akv_get(s, "key", 3, out, sizeof(out), &n) == 0 && n == 100, "latest length after append");
    akv_destroy(s);
}

/* ---- concurrency ---------------------------------------------------------------------------------- */

struct worker {
    akv *s;
    unsigned id;
    unsigned ops;
    unsigned keys;
    int bad;
};

static void *worker_main(void *arg) {
    struct worker *w = arg;
    unsigned char buf[2048];
    unsigned char out[2200];
    uint64_t state = 0x9e3779b97f4a7c15ull * (w->id + 1);
    for (unsigned i = 0; i < w->ops; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint64_t id = state % w->keys;
        char key[32];
        size_t klen;
        key_of(id, key, &klen);
        unsigned op = (unsigned)((state >> 20) % 100);
        if (op < 30) {
            size_t n = make_value(buf, id, state >> 24, 20 + (uint32_t)((state >> 40) % 1800));
            if (akv_put(w->s, key, klen, buf, n) != 0) {
                w->bad++;
            }
        } else if (op < 97) {
            size_t n = 0;
            int rc = akv_get(w->s, key, klen, out, sizeof(out), &n);
            if (rc == 0 && check_value(out, n, id) == UINT64_MAX) {
                w->bad++;
            }
            if (rc != 0 && rc != -ENOENT) {
                w->bad++;
            }
        } else {
            akv_del(w->s, key, klen);
        }
    }
    return NULL;
}

static void test_threads(void) {
    akv *s = make(2 << 20, 16);
    enum { THREADS = 8 };
    struct worker ws[THREADS];
    pthread_t th[THREADS];
    for (unsigned i = 0; i < THREADS; i++) {
        ws[i] = (struct worker){.s = s, .id = i, .ops = 150000, .keys = 6000};
        pthread_create(&th[i], NULL, worker_main, &ws[i]);
    }
    int bad = 0;
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        bad += ws[i].bad;
    }
    EXPECT(bad == 0, "%d inconsistent results under concurrency", bad);
    akv_stats st;
    akv_stats_get(s, &st);
    EXPECT(st.entries <= 6000, "entry counter %llu", (unsigned long long)st.entries);
    EXPECT(st.log_bytes <= st.arena_bytes, "log above arena");
    akv_destroy(s);
}

int main(void) {
    printf("vector implementation: %s\n", akv_simd_name());
    test_basic();
    run_model(64 << 20, 8, 2000, 1500, 300000, 1);  /* ample memory: exact */
    run_model(512 << 10, 4, 5000, 2000, 600000, 0); /* heavy eviction */
    run_model(16 << 10, 1, 800, 1500, 300000, 0);   /* tiny arena: wrap-around gaps of 8 and 16 bytes */
    run_model(64 << 10, 2, 100000, 100, 400000, 0); /* more keys than index slots: index evictions */
    test_second_chance();
    test_inplace();
    test_threads();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_akv: ok");
    return 0;
}
