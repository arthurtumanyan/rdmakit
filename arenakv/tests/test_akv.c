/*
 * test_akv.c - engine tests.
 *
 * - basic operations and argument validation
 * - model check against a reference map: exact results when memory is ample; under eviction a hit must
 *   always return the latest stored version (never a stale or foreign value)
 * - arena wrap-around with small gaps (tiny shard arena, entry sizes that leave 8 and 16 byte gaps)
 * - second chance: a frequently read entry survives while unread neighbors are evicted
 * - multi-threaded stress with self-describing values (run under ThreadSanitizer as well)
 * - index growth from the minimal size without losing entries; the same model with a growing index;
 *   lookups of every key while a doubling is still moving slots, and release of the old table
 * - store modes (add, replace, append, prepend, CAS), client flags and CAS versions
 * - expiry: time to live, touch, persist, keep-ttl, get-and-touch, reclamation counters
 * - increments in memcached (unsigned, wrapping, floor at 0) and RESP (signed, overflow) semantics
 * - flush, enumeration with remaining time to live, and stores without eviction
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Index sizing of make(): 128 preallocates the index, 0 lets it grow from the minimal size. */
static uint32_t make_avg_entry = 128;

static akv *make(uint64_t capacity, uint32_t shards) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = capacity;
    cfg.shards = shards;
    cfg.avg_entry = make_avg_entry;
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

/* ---- index growth --------------------------------------------------------------------------------- */

static void test_growth(void) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = 64 << 20;
    cfg.shards = 4;
    cfg.hash_seed = 3;
    akv *s = akv_create(&cfg);
    akv_stats st0;
    akv_stats_get(s, &st0);
    enum { N = 200000 };
    char key[32];
    size_t klen;
    unsigned char val[64];
    unsigned char out[64];
    size_t n;
    for (unsigned i = 0; i < N; i++) {
        key_of(i, key, &klen);
        memcpy(val, &i, sizeof(i));
        EXPECT(akv_put(s, key, klen, val, sizeof(val)) == 0, "growth put %u", i);
    }
    unsigned missing = 0;
    for (unsigned i = 0; i < N; i++) {
        key_of(i, key, &klen);
        unsigned got = 0;
        if (akv_get(s, key, klen, out, sizeof(out), &n) != 0 || (memcpy(&got, out, sizeof(got)), got != i)) {
            missing++;
        }
    }
    akv_stats st;
    akv_stats_get(s, &st);
    EXPECT(missing == 0, "%u of %u entries lost while the index grew", missing, N);
    EXPECT(st.index_evictions == 0 && st.evictions == 0, "evictions %llu/%llu with ample memory",
           (unsigned long long)st.evictions, (unsigned long long)st.index_evictions);
    EXPECT(st.index_grows > 0, "the index never grew");
    EXPECT(st0.index_bytes == 4 * 16 * 128, "initial index %llu bytes", (unsigned long long)st0.index_bytes);
    /* At most 2 / 0.75 slots of 8 bytes per entry plus rounding to a power of two: 32 bytes per entry. */
    EXPECT(st.index_bytes <= (uint64_t)N * 32, "index %llu bytes for %u entries",
           (unsigned long long)st.index_bytes, N);
    printf("growth: %u entries, index %llu KiB after %llu doublings\n", N,
           (unsigned long long)(st.index_bytes >> 10), (unsigned long long)st.index_grows);
    akv_destroy(s);
}

/*
 * Incremental growth: right after a doubling the old table is still held (index memory is three times the
 * size before the doubling) and every key stored so far is found while its slot may be in either table;
 * lookups then finish the move and release the old table.
 */
static void test_incremental_growth(void) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = 64 << 20;
    cfg.shards = 1;
    cfg.hash_seed = 7;
    akv *s = akv_create(&cfg);
    enum { N = 100000 };
    char key[32];
    size_t klen;
    unsigned char val[32] = {0};
    unsigned char out[32];
    size_t n;
    akv_stats prev;
    akv_stats_get(s, &prev);
    unsigned checks = 0;
    for (unsigned i = 0; i < N; i++) {
        key_of(i, key, &klen);
        EXPECT(akv_put(s, key, klen, val, sizeof(val)) == 0, "incremental put %u", i);
        akv_stats st;
        akv_stats_get(s, &st);
        if (st.index_grows != prev.index_grows) {
            EXPECT(st.index_grows == prev.index_grows + 1, "one doubling per put");
            EXPECT(st.index_bytes == 3 * prev.index_bytes, "index %llu bytes after doubling %llu bytes",
                   (unsigned long long)st.index_bytes, (unsigned long long)prev.index_bytes);
            unsigned missing = 0;
            for (unsigned j = 0; j <= i; j++) {
                key_of(j, key, &klen);
                missing += !akv_exists(s, key, klen); /* akv_exists does not advance the move */
            }
            EXPECT(missing == 0, "%u of %u keys missing during the move after doubling %llu", missing, i + 1,
                   (unsigned long long)st.index_grows);
            checks++;
        }
        prev = st;
    }
    for (unsigned i = 0; i < N; i++) {
        key_of(i, key, &klen);
        EXPECT(akv_get(s, key, klen, out, sizeof(out), &n) == 0, "incremental get %u", i);
    }
    akv_stats st;
    akv_stats_get(s, &st);
    uint64_t buckets = st.index_bytes / 128;
    EXPECT((buckets & (buckets - 1)) == 0, "old table still held after %u lookups (%llu buckets)", N,
           (unsigned long long)buckets);
    EXPECT(st.entries == N && st.index_evictions == 0, "entries %llu, index evictions %llu",
           (unsigned long long)st.entries, (unsigned long long)st.index_evictions);
    EXPECT(checks > 5, "only %u doublings observed", checks);
    printf("incremental growth: %u doublings checked mid-move, index %llu KiB\n", checks,
           (unsigned long long)(st.index_bytes >> 10));
    akv_destroy(s);
}

/* akv_prefetch() runs without the lock while another thread grows the index (checked under TSan). */
struct prefetch_arg {
    akv *s;
    atomic_int stop;
    unsigned long calls;
};

static void *prefetch_main(void *arg) {
    struct prefetch_arg *a = arg;
    char key[32];
    size_t klen;
    unsigned i = 0;
    while (!atomic_load(&a->stop)) {
        key_of(i++ % 300000, key, &klen);
        akv_prefetch(a->s, key, klen);
        a->calls++;
    }
    return NULL;
}

static void test_prefetch(void) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = 64 << 20;
    cfg.shards = 4;
    cfg.hash_seed = 5;
    akv *s = akv_create(&cfg);
    struct prefetch_arg a = {.s = s};
    pthread_t th;
    pthread_create(&th, NULL, prefetch_main, &a);
    char key[32];
    size_t klen;
    unsigned char val[16] = {0};
    unsigned char out[16];
    size_t n;
    enum { N = 100000 };
    for (unsigned i = 0; i < N; i++) {
        key_of(i, key, &klen);
        EXPECT(akv_put(s, key, klen, val, sizeof(val)) == 0, "prefetch put %u", i);
    }
    atomic_store(&a.stop, 1);
    pthread_join(th, NULL);
    unsigned missing = 0;
    for (unsigned i = 0; i < N; i++) {
        key_of(i, key, &klen);
        akv_prefetch(s, key, klen);
        missing += akv_get(s, key, klen, out, sizeof(out), &n) != 0;
    }
    akv_stats st;
    akv_stats_get(s, &st);
    EXPECT(missing == 0, "%u entries missing after prefetching during growth", missing);
    EXPECT(st.index_grows > 0, "the index never grew during the prefetch test");
    printf("prefetch: %lu hints during %llu index doublings\n", a.calls, (unsigned long long)st.index_grows);
    akv_destroy(s);
}

/* ---- store modes, flags and CAS ------------------------------------------------------------------- */

static void test_modes(void) {
    akv *s = make(1 << 20, 2);
    unsigned char out[256];
    size_t n;
    akv_meta m;
    uint64_t cas1;
    uint64_t cas2;
    akv_store_opts add = {.mode = AKV_ADD, .flags = 7};
    EXPECT(akv_store(s, "k", 1, "one", 3, &add, &cas1) == 0, "add new");
    EXPECT(akv_store(s, "k", 1, "two", 3, &add, NULL) == -EEXIST, "add existing");
    EXPECT(akv_get_meta(s, "k", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && n == 3 && m.flags == 7 &&
               m.cas == cas1 && m.ttl_ms == -1,
           "flags and cas of the added entry");
    akv_store_opts rep = {.mode = AKV_REPLACE, .flags = 9};
    EXPECT(akv_store(s, "absent", 6, "x", 1, &rep, NULL) == -ENOENT, "replace absent");
    EXPECT(akv_store(s, "k", 1, "three", 5, &rep, &cas2) == 0 && cas2 != cas1, "replace present changes cas");
    akv_store_opts app = {.mode = AKV_APPEND, .flags = 99};
    EXPECT(akv_store(s, "k", 1, "-tail", 5, &app, NULL) == 0, "append");
    akv_store_opts pre = {.mode = AKV_PREPEND};
    EXPECT(akv_store(s, "k", 1, "head-", 5, &pre, NULL) == 0, "prepend");
    EXPECT(akv_get_meta(s, "k", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && n == 15 &&
               memcmp(out, "head-three-tail", 15) == 0 && m.flags == 9,
           "append and prepend keep the flags: %.*s flags %u", (int)n, out, m.flags);
    EXPECT(akv_store(s, "absent", 6, "x", 1, &app, NULL) == -ENOENT, "append absent");
    /* same-size append (in place) */
    EXPECT(akv_put(s, "p", 1, "ab", 2) == 0 && akv_store(s, "p", 1, "c", 1, &app, NULL) == 0 &&
               akv_get(s, "p", 1, out, sizeof(out), &n) == 0 && n == 3 && memcmp(out, "abc", 3) == 0,
           "in-place append");
    EXPECT(akv_store(s, "p", 1, "z", 1, &pre, NULL) == 0 && akv_get(s, "p", 1, out, sizeof(out), &n) == 0 &&
               n == 4 && memcmp(out, "zabc", 4) == 0,
           "prepend");
    akv_get_meta(s, "k", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP);
    akv_store_opts cas = {.mode = AKV_CAS, .cas = m.cas + 1};
    EXPECT(akv_store(s, "k", 1, "new", 3, &cas, NULL) == -EEXIST, "cas mismatch");
    cas.cas = m.cas;
    EXPECT(akv_store(s, "k", 1, "new", 3, &cas, &cas1) == 0 && cas1 != m.cas, "cas match");
    EXPECT(akv_store(s, "k", 1, "again", 5, &cas, NULL) == -EEXIST, "stale cas after a store");
    EXPECT(akv_store(s, "absent", 6, "x", 1, &cas, NULL) == -ENOENT, "cas on an absent key");
    EXPECT(akv_exists(s, "k", 1) == 1 && akv_exists(s, "absent", 6) == 0, "exists");
    akv_destroy(s);
}

/* ---- expiry ---------------------------------------------------------------------------------------- */

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void test_ttl(void) {
    akv *s = make(1 << 20, 1);
    unsigned char out[64];
    size_t n;
    akv_meta m;
    akv_store_opts t = {.ttl_ms = 60};
    EXPECT(akv_store(s, "a", 1, "v", 1, &t, NULL) == 0, "store with ttl");
    EXPECT(akv_get_meta(s, "a", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && m.ttl_ms > 0 &&
               m.ttl_ms <= 60,
           "remaining ttl %lld", (long long)m.ttl_ms);
    sleep_ms(90);
    EXPECT(akv_get(s, "a", 1, out, sizeof(out), &n) == -ENOENT, "expired entry is not returned");
    EXPECT(akv_exists(s, "a", 1) == 0 && akv_del(s, "a", 1) == -ENOENT, "expired entry does not exist");
    akv_store_opts add = {.mode = AKV_ADD};
    EXPECT(akv_store(s, "a", 1, "w", 1, &add, NULL) == 0, "add over an expired entry");

    /* the largest times to live are clamped to AKV_TTL_MAX instead of overflowing into the past */
    akv_store_opts huge = {.ttl_ms = INT64_MAX};
    EXPECT(akv_store(s, "h", 1, "v", 1, &huge, NULL) == 0 &&
               akv_get_meta(s, "h", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 &&
               m.ttl_ms > AKV_TTL_MAX - 1000 && m.ttl_ms <= AKV_TTL_MAX,
           "huge ttl clamped (%lld)", (long long)m.ttl_ms);
    EXPECT(akv_touch(s, "h", 1, INT64_MAX) == 0 && akv_exists(s, "h", 1), "huge touch clamped");

    t.ttl_ms = 60;
    akv_store(s, "b", 1, "v", 1, &t, NULL);
    EXPECT(akv_touch(s, "b", 1, 0) == 0, "persist");
    sleep_ms(90);
    EXPECT(akv_get_meta(s, "b", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && m.ttl_ms == -1,
           "persisted entry survives");
    EXPECT(akv_touch(s, "b", 1, 50000) == 0 &&
               akv_get_meta(s, "b", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && m.ttl_ms > 49000,
           "touch sets a ttl");
    akv_store_opts keep = {.keep_ttl = 1};
    EXPECT(akv_store(s, "b", 1, "xyz", 3, &keep, NULL) == 0 &&
               akv_get_meta(s, "b", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && m.ttl_ms > 49000 &&
               n == 3,
           "keep_ttl keeps the expiry");
    EXPECT(akv_store(s, "b", 1, "xyz", 3, NULL, NULL) == 0 &&
               akv_get_meta(s, "b", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && m.ttl_ms == -1,
           "a plain store clears the expiry");
    EXPECT(akv_get_meta(s, "b", 1, out, sizeof(out), &n, &m, 30) == 0 &&
               akv_get_meta(s, "b", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && m.ttl_ms > 0 &&
               m.ttl_ms <= 30,
           "get-and-touch");
    EXPECT(akv_touch(s, "b", 1, -1) == 0 && akv_exists(s, "b", 1) == 0, "negative touch deletes");
    t.ttl_ms = -1;
    EXPECT(akv_put(s, "c", 1, "v", 1) == 0 && akv_store(s, "c", 1, "v", 1, &t, NULL) == 0 &&
               akv_exists(s, "c", 1) == 0,
           "store with a negative ttl removes the key");
    EXPECT(akv_touch(s, "zz", 2, 10) == -ENOENT, "touch absent");

    /* Expired entries are reclaimed by eviction without counting as evictions. */
    t.ttl_ms = 20;
    unsigned char big[900];
    memset(big, 1, sizeof(big));
    for (unsigned i = 0; i < 400; i++) {
        char key[16];
        size_t klen = (size_t)snprintf(key, sizeof(key), "t%u", i);
        akv_store(s, key, klen, big, sizeof(big), &t, NULL);
    }
    sleep_ms(40);
    akv_stats st0;
    akv_stats_get(s, &st0);
    /* 1100 entries of about 950 bytes exceed the 1 MiB arena, so the tail passes the expired ones. */
    for (unsigned i = 0; i < 1100; i++) {
        char key[16];
        size_t klen = (size_t)snprintf(key, sizeof(key), "u%u", i);
        akv_put(s, key, klen, big, sizeof(big));
    }
    akv_stats st;
    akv_stats_get(s, &st);
    EXPECT(st.expired > st0.expired, "expired entries reclaimed: %llu", (unsigned long long)st.expired);
    akv_destroy(s);
}

/* ---- increments ------------------------------------------------------------------------------------ */

static void test_incr(void) {
    akv *s = make(1 << 20, 1);
    unsigned char out[64];
    size_t n;
    int64_t r;
    EXPECT(akv_incr(s, "n", 1, 1, AKV_INCR_UNSIGNED, &r, NULL) == -ENOENT, "unsigned incr of an absent key");
    akv_store_opts f = {.flags = 5, .ttl_ms = 100000};
    akv_store(s, "n", 1, "41", 2, &f, NULL);
    EXPECT(akv_incr(s, "n", 1, 1, AKV_INCR_UNSIGNED, &r, NULL) == 0 && r == 42, "incr");
    akv_meta m;
    EXPECT(akv_get_meta(s, "n", 1, out, sizeof(out), &n, &m, AKV_TTL_KEEP) == 0 && n == 2 &&
               memcmp(out, "42", 2) == 0 && m.flags == 5 && m.ttl_ms > 0,
           "result stored as text with flags and expiry kept");
    EXPECT(akv_incr(s, "n", 1, -100, AKV_INCR_UNSIGNED, &r, NULL) == 0 && r == 0, "decrement stops at 0");
    akv_put(s, "w", 1, "18446744073709551615", 20);
    EXPECT(akv_incr(s, "w", 1, 2, AKV_INCR_UNSIGNED, &r, NULL) == 0 && (uint64_t)r == 1, "unsigned wrap");
    akv_put(s, "x", 1, "12a", 3);
    EXPECT(akv_incr(s, "x", 1, 1, AKV_INCR_UNSIGNED, &r, NULL) == -EINVAL, "non-numeric value");
    akv_put(s, "neg", 3, "-5", 2);
    EXPECT(akv_incr(s, "neg", 3, 1, AKV_INCR_UNSIGNED, &r, NULL) == -EINVAL,
           "negative value in unsigned mode");
    EXPECT(akv_incr(s, "neg", 3, 1, AKV_INCR_SIGNED, &r, NULL) == 0 && r == -4, "signed incr");
    EXPECT(akv_incr(s, "fresh", 5, -3, AKV_INCR_SIGNED, &r, NULL) == 0 && r == -3,
           "signed incr creates the key");
    akv_put(s, "max", 3, "9223372036854775807", 19);
    EXPECT(akv_incr(s, "max", 3, 1, AKV_INCR_SIGNED, &r, NULL) == -ERANGE, "signed overflow");
    EXPECT(akv_get(s, "max", 3, out, sizeof(out), &n) == 0 && n == 19, "value unchanged after overflow");
    akv_put(s, "big", 3, "99999999999999999999", 20);
    EXPECT(akv_incr(s, "big", 3, 1, AKV_INCR_SIGNED, &r, NULL) == -EINVAL, "out-of-range text");
    akv_destroy(s);
}

/* ---- flush, enumeration, no eviction --------------------------------------------------------------- */

struct seen {
    unsigned count;
    unsigned with_ttl;
    int bad;
};

static int count_record(void *arg, const akv_record *r) {
    struct seen *c = arg;
    c->count++;
    if (r->ttl_ms >= 0) {
        c->with_ttl++;
        if (r->ttl_ms > 100000) {
            c->bad++;
        }
    }
    if (r->vlen != 8 || r->klen == 0) {
        c->bad++;
    }
    return 0;
}

static int stop_at_3(void *arg, const akv_record *r) {
    (void)r;
    return ++*(unsigned *)arg == 3 ? 42 : 0;
}

static void test_flush_foreach(void) {
    akv *s = make(4 << 20, 4);
    char key[32];
    size_t klen;
    for (unsigned i = 0; i < 1000; i++) {
        key_of(i, key, &klen);
        akv_store_opts o = {.ttl_ms = i % 2 ? 100000 : 0};
        akv_store(s, key, klen, "12345678", 8, &o, NULL);
    }
    for (unsigned i = 0; i < 100; i++) {
        key_of(i, key, &klen);
        akv_del(s, key, klen);
    }
    akv_store_opts gone = {.ttl_ms = 1};
    akv_store(s, "short", 5, "12345678", 8, &gone, NULL);
    sleep_ms(5);
    struct seen c = {0};
    EXPECT(akv_foreach(s, count_record, &c) == 0, "foreach");
    EXPECT(c.count == 900 && c.with_ttl == 450 && c.bad == 0, "foreach saw %u entries, %u with ttl, %d bad",
           c.count, c.with_ttl, c.bad);
    unsigned k = 0;
    EXPECT(akv_foreach(s, stop_at_3, &k) == 42 && k == 3, "foreach stops at the first non-zero result");
    akv_flush(s);
    akv_stats st;
    akv_stats_get(s, &st);
    unsigned char out[16];
    size_t n;
    key_of(500, key, &klen);
    EXPECT(st.entries == 0 && st.log_bytes == 0 && akv_get(s, key, klen, out, sizeof(out), &n) == -ENOENT,
           "flush removes everything");
    EXPECT(akv_put(s, key, klen, "x", 1) == 0 && akv_get(s, key, klen, out, sizeof(out), &n) == 0,
           "store after flush");
    akv_destroy(s);

    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = 64 << 10;
    cfg.shards = 1;
    cfg.no_evict = 1;
    s = akv_create(&cfg);
    unsigned char val[1000];
    memset(val, 3, sizeof(val));
    unsigned stored = 0;
    int rc = 0;
    for (unsigned i = 0; i < 200 && rc == 0; i++) {
        key_of(i, key, &klen);
        rc = akv_put(s, key, klen, val, sizeof(val));
        stored += rc == 0;
    }
    EXPECT(rc == -ENOMEM && stored > 30 && stored < 64, "no-evict store fails when full (stored %u, rc %d)",
           stored, rc);
    unsigned present = 0;
    unsigned char big[1100];
    for (unsigned i = 0; i < stored; i++) {
        key_of(i, key, &klen);
        present += akv_get(s, key, klen, big, sizeof(big), &n) == 0;
    }
    EXPECT(present == stored, "no entry was evicted (%u of %u present)", present, stored);
    for (unsigned i = 0; i < 5; i++) {
        key_of(i, key, &klen);
        akv_del(s, key, klen); /* the oldest entries become reclaimable dead space */
    }
    key_of(9999, key, &klen);
    EXPECT(akv_put(s, key, klen, val, sizeof(val)) == 0, "dead space is reused without evicting");
    akv_destroy(s);
}

int main(void) {
    printf("vector implementation: %s\n", akv_simd_name());
    test_basic();
    run_model(64 << 20, 8, 2000, 1500, 300000, 1);  /* ample memory: exact */
    run_model(512 << 10, 4, 5000, 2000, 600000, 0); /* heavy eviction */
    run_model(16 << 10, 1, 800, 1500, 300000, 0);   /* tiny arena: wrap-around gaps of 8 and 16 bytes */
    run_model(64 << 10, 2, 100000, 100, 400000, 0); /* more keys than index slots: index evictions */
    /* the same models with an index that grows from the minimal size */
    make_avg_entry = 0;
    run_model(64 << 20, 8, 2000, 1500, 300000, 1);
    run_model(512 << 10, 4, 5000, 2000, 600000, 0);
    run_model(64 << 10, 2, 100000, 100, 400000, 0);
    make_avg_entry = 128;
    test_second_chance();
    test_inplace();
    test_threads();
    test_growth();
    test_incremental_growth();
    test_prefetch();
    test_modes();
    test_ttl();
    test_incr();
    test_flush_foreach();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_akv: ok");
    return 0;
}
