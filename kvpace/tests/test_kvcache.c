/*
 * test_kvcache.c - KV-cache workload tests with an in-memory LRU cache as driver.
 *
 * Verifies the request structure (hits up to the first miss, PUTs of the missing tail), hit rates with
 * ample and with tiny capacity, determinism for equal seeds, key sharing between conversations of one
 * group, key separation between groups and seeds, and detection of corrupted content. A run of the
 * scenario through pace_run() counts block operations as the operations of the result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvcache.h"
#include "runner.h"

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

/* ---- in-memory LRU cache ------------------------------------------------------------------------- */

#define BLOCK 64

struct entry {
    char key[PACE_KVCACHE_KEY_LEN];
    unsigned char val[BLOCK];
    uint64_t used; /* logical clock of the last access; 0 = free */
};

static struct entry *cache;
static size_t capacity;
static uint64_t clock_ticks;
static int corrupt_reads;

static struct entry *find(const void *key, size_t klen) {
    for (size_t i = 0; i < capacity; i++) {
        if (cache[i].used != 0 && klen == PACE_KVCACHE_KEY_LEN && memcmp(cache[i].key, key, klen) == 0) {
            return &cache[i];
        }
    }
    return NULL;
}

static void cache_reset(size_t cap) {
    free(cache);
    cache = calloc(cap, sizeof(*cache));
    capacity = cap;
    clock_ticks = 0;
}

static pace_conn *fake_connect(const char *host, const char *port) {
    (void)host;
    (void)port;
    return (pace_conn *)malloc(1);
}

static int fake_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    (void)c;
    struct entry *e = find(key, klen);
    if (e == NULL) {
        e = &cache[0];
        for (size_t i = 0; i < capacity; i++) {
            if (cache[i].used == 0) {
                e = &cache[i];
                break;
            }
            if (cache[i].used < e->used) {
                e = &cache[i];
            }
        }
    }
    memcpy(e->key, key, klen);
    memcpy(e->val, val, vlen < BLOCK ? vlen : BLOCK);
    e->used = ++clock_ticks;
    return PACE_OK;
}

static int fake_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    (void)c;
    (void)cap;
    struct entry *e = find(key, klen);
    if (e == NULL) {
        return PACE_NOTFOUND;
    }
    e->used = ++clock_ticks;
    memcpy(buf, e->val, BLOCK);
    if (corrupt_reads) {
        ((unsigned char *)buf)[3] ^= 0x40;
    }
    *vlen = BLOCK;
    return PACE_OK;
}

static void fake_close(pace_conn *c) {
    free(c);
}

static const pace_driver fake = {
    .name = "fake", .connect = fake_connect, .put = fake_put, .get = fake_get, .close = fake_close};

static pace_kvcache_cfg test_cfg(void) {
    pace_kvcache_cfg cfg;
    pace_kvcache_defaults(&cfg);
    cfg.block_size = BLOCK;
    cfg.groups = 4;
    cfg.shared_blocks = 6;
    cfg.turn_blocks = 3;
    cfg.active_conv = 8;
    cfg.max_turns = 5;
    cfg.new_conv_prob = 0.05;
    cfg.theta = 0.5;
    cfg.seed = 99;
    return cfg;
}

static pace_kvcache_stats run_requests(const pace_kvcache_cfg *cfg, unsigned n) {
    pace_kvcache_stats st = {0};
    pace_kvcache_state *s = pace_kvcache_create(cfg, 0, 7);
    pace_conn *c = fake.connect("", "");
    for (unsigned i = 0; i < n; i++) {
        EXPECT(pace_kvcache_request(s, &fake, c, &st) == PACE_OK, "request %u failed", i);
    }
    fake.close(c);
    pace_kvcache_destroy(s);
    return st;
}

static void test_large_cache(void) {
    pace_kvcache_cfg cfg = test_cfg();
    cache_reset(100000);
    corrupt_reads = 0;
    pace_kvcache_stats st = run_requests(&cfg, 6000);
    double hit = (double)st.block_hits / (double)st.block_gets;
    double puts_per_req = (double)st.block_puts / (double)st.requests;
    EXPECT(st.requests == 6000, "requests %llu", (unsigned long long)st.requests);
    EXPECT(hit > 0.75, "hit rate %.3f with unlimited capacity", hit);
    /* Steady state writes exactly the new turn's blocks (3) plus warm-up writes of the 4 templates (24 blocks). */
    EXPECT(puts_per_req > 2.9 && puts_per_req < 3.3, "puts per request %.3f", puts_per_req);
    EXPECT(st.bytes_put == st.block_puts * BLOCK, "bytes_put %llu", (unsigned long long)st.bytes_put);
    EXPECT(st.bytes_get == st.block_hits * BLOCK, "bytes_get %llu", (unsigned long long)st.bytes_get);
    EXPECT(st.corrupt == 0, "corrupt %llu", (unsigned long long)st.corrupt);
    EXPECT(st.block_gets >= st.block_hits && st.block_gets - st.block_hits <= st.requests,
           "at most one miss per request");
}

static void test_tiny_cache(void) {
    pace_kvcache_cfg cfg = test_cfg();
    cache_reset(4);
    corrupt_reads = 0;
    pace_kvcache_stats st = run_requests(&cfg, 3000);
    double hit = (double)st.block_hits / (double)st.block_gets;
    EXPECT(hit < 0.2, "hit rate %.3f with capacity below the chain length", hit);
    EXPECT(st.block_puts > st.requests * 6, "puts %llu: chains must be rewritten",
           (unsigned long long)st.block_puts);
}

static void test_determinism(void) {
    pace_kvcache_cfg cfg = test_cfg();
    cache_reset(100000);
    corrupt_reads = 0;
    pace_kvcache_stats a = run_requests(&cfg, 2000);
    cache_reset(100000);
    pace_kvcache_stats b = run_requests(&cfg, 2000);
    EXPECT(memcmp(&a, &b, sizeof(a)) == 0, "equal seeds must give equal counters");
}

static void test_corruption(void) {
    pace_kvcache_cfg cfg = test_cfg();
    cache_reset(100000);
    corrupt_reads = 1;
    pace_kvcache_stats st = run_requests(&cfg, 500);
    EXPECT(st.corrupt > 0 && st.corrupt == st.block_hits, "corrupt %llu of %llu hits",
           (unsigned long long)st.corrupt, (unsigned long long)st.block_hits);
    corrupt_reads = 0;
}

static void test_keys(void) {
    char a[PACE_KVCACHE_KEY_LEN];
    char b[PACE_KVCACHE_KEY_LEN];
    EXPECT(pace_kvcache_block_key(1, 2, 0, 5, a) == PACE_KVCACHE_KEY_LEN, "key length");
    pace_kvcache_block_key(1, 2, 0, 5, b);
    EXPECT(memcmp(a, b, sizeof(a)) == 0, "template key is deterministic");
    pace_kvcache_block_key(1, 3, 0, 5, b);
    EXPECT(memcmp(a, b, sizeof(a)) != 0, "groups have distinct template blocks");
    pace_kvcache_block_key(2, 2, 0, 5, b);
    EXPECT(memcmp(a, b, sizeof(a)) != 0, "seeds have distinct keys");
    pace_kvcache_block_key(1, 2, 7, 5, b);
    EXPECT(memcmp(a, b, sizeof(a)) != 0, "conversation blocks differ from template blocks");
    EXPECT(a[0] == 'b', "key prefix");
}

/* Two threads of the same workload must not share conversations (identities carry the thread id). */
static void test_thread_independence(void) {
    pace_kvcache_cfg cfg = test_cfg();
    cfg.new_conv_prob = 0.0;
    cfg.max_turns = 1000;
    cache_reset(100000);
    corrupt_reads = 0;
    pace_kvcache_state *s0 = pace_kvcache_create(&cfg, 0, 5);
    pace_kvcache_state *s1 = pace_kvcache_create(&cfg, 1, 5);
    pace_conn *c = fake.connect("", "");
    pace_kvcache_stats st0 = {0};
    pace_kvcache_stats st1 = {0};
    for (int i = 0; i < 200; i++) {
        pace_kvcache_request(s0, &fake, c, &st0);
    }
    uint64_t puts_before = st1.block_puts;
    pace_kvcache_request(s1, &fake, c, &st1);
    /* Thread 1 starts fresh conversations: at least one new turn's worth of blocks is written (the template may hit). */
    EXPECT(st1.block_puts - puts_before >= cfg.turn_blocks, "thread 1 wrote %llu blocks",
           (unsigned long long)st1.block_puts);
    fake.close(c);
    pace_kvcache_destroy(s0);
    pace_kvcache_destroy(s1);
}

/* One thread through the runner: the fake cache is not thread-safe. */
static void test_runner_counts(void) {
    cache_reset(4096);
    pace_config cfg;
    pace_config_default(&cfg);
    cfg.driver = &fake;
    cfg.threads = 1;
    cfg.scenario = PACE_SCENARIO_KVCACHE;
    cfg.kvcache = test_cfg();
    cfg.kvcache.block_size = BLOCK;
    cfg.preload = 0;
    cfg.warmup_s = 0.05;
    cfg.duration_s = 0.3;
    static pace_result res;
    EXPECT(pace_run(&cfg, &res) == 0, "kvcache run");
    uint64_t ops = pace_result_ops(&res);
    EXPECT(res.kvcache.requests > 0 && ops > 0 && ops == res.kvcache.block_gets + res.kvcache.block_puts &&
               res.gets == res.kvcache.block_gets && res.puts == res.kvcache.block_puts,
           "operations are block operations (%llu ops, %llu gets, %llu puts, %llu requests)",
           (unsigned long long)ops, (unsigned long long)res.kvcache.block_gets,
           (unsigned long long)res.kvcache.block_puts, (unsigned long long)res.kvcache.requests);
}

int main(void) {
    test_keys();
    test_large_cache();
    test_tiny_cache();
    test_determinism();
    test_corruption();
    test_thread_independence();
    test_runner_counts();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_kvcache: ok");
    return 0;
}
