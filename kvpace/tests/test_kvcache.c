/*
 * test_kvcache.c - KV-cache workload tests with an in-memory LRU cache as driver.
 *
 * Verifies the request structure (hits up to the first miss, PUTs of the missing tail), hit rates with
 * ample and with tiny capacity, determinism for equal seeds, key sharing between conversations of one
 * group, key separation between groups and seeds, and detection of corrupted content. A run of the
 * scenario through pace_run() counts block operations as the operations of the result. Through the
 * asynchronous interface (several connections and pipeline slots per thread), request machines produce
 * the same requests as synchronous threads of the same identities: with no template blocks (so that no
 * two identities share a key) the counters of both runners are equal; with templates, hits are verified
 * byte for byte and open-loop runs complete.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

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
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER; /* runner threads share the cache */

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
    pthread_mutex_lock(&cache_lock);
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
    pthread_mutex_unlock(&cache_lock);
    return PACE_OK;
}

static int fake_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    (void)c;
    (void)cap;
    pthread_mutex_lock(&cache_lock);
    struct entry *e = find(key, klen);
    if (e == NULL) {
        pthread_mutex_unlock(&cache_lock);
        return PACE_NOTFOUND;
    }
    e->used = ++clock_ticks;
    memcpy(buf, e->val, BLOCK);
    pthread_mutex_unlock(&cache_lock);
    if (corrupt_reads) {
        ((unsigned char *)buf)[3] ^= 0x40;
    }
    *vlen = BLOCK;
    return PACE_OK;
}

static void fake_close(pace_conn *c) {
    free(c);
}

/*
 * Asynchronous interface over the same cache: submit() executes the operation at once and queues its
 * reply (with a copy of a GET value); next() returns the queued replies in order. The descriptor is an
 * eventfd that never becomes readable; arm() reports progress whenever replies are queued.
 */
#define AQ 256
struct afake {
    int fd;
    unsigned head;
    unsigned count;
    pace_reply rep[AQ];
    unsigned char val[AQ][BLOCK];
    unsigned char cur[BLOCK]; /* value of the reply last returned by next() */
};

static pace_aconn *afake_connect(const char *host, const char *port) {
    (void)host;
    (void)port;
    struct afake *a = calloc(1, sizeof(*a));
    if (a != NULL) {
        a->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }
    return (pace_aconn *)a;
}

static int afake_fd(pace_aconn *c) {
    return ((struct afake *)c)->fd;
}

static unsigned afake_window(pace_aconn *c) {
    (void)c;
    return AQ;
}

static int afake_submit(pace_aconn *c, const pace_req *r) {
    struct afake *a = (struct afake *)c;
    if (a->count == AQ) {
        return PACE_ERR;
    }
    unsigned i = (a->head + a->count) % AQ;
    pace_reply *p = &a->rep[i];
    *p = (pace_reply){0};
    if (r->type == PACE_OP_GET) {
        size_t n = 0;
        p->status = fake_get(NULL, r->key, r->klen, a->val[i], BLOCK, &n);
        p->vlen = p->status == PACE_OK ? n : 0;
    } else {
        p->status = fake_put(NULL, r->key, r->klen, r->val, r->vlen);
    }
    a->count++;
    return PACE_OK;
}

static int afake_io(pace_aconn *c) {
    (void)c;
    return PACE_OK;
}

static int afake_next(pace_aconn *c, pace_reply *out) {
    struct afake *a = (struct afake *)c;
    if (a->count == 0) {
        return 0;
    }
    *out = a->rep[a->head];
    if (out->status == PACE_OK && out->vlen > 0) {
        memcpy(a->cur, a->val[a->head], out->vlen);
        out->val = a->cur;
    }
    a->head = (a->head + 1) % AQ;
    a->count--;
    return 1;
}

static unsigned afake_events(pace_aconn *c) {
    (void)c;
    return EPOLLIN;
}

static int afake_arm(pace_aconn *c) {
    return ((struct afake *)c)->count > 0;
}

static void afake_close(pace_aconn *c) {
    struct afake *a = (struct afake *)c;
    close(a->fd);
    free(a);
}

static const pace_adriver afake = {.connect = afake_connect,
                                   .fd = afake_fd,
                                   .window = afake_window,
                                   .submit = afake_submit,
                                   .io = afake_io,
                                   .next = afake_next,
                                   .events = afake_events,
                                   .arm = afake_arm,
                                   .close = afake_close};

static const pace_driver fake = {.name = "fake",
                                 .connect = fake_connect,
                                 .put = fake_put,
                                 .get = fake_get,
                                 .close = fake_close,
                                 .async = &afake};

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

/* Runs the scenario through pace_run() in count mode and returns the counters. */
static pace_kvcache_stats run_count(unsigned threads, unsigned conns, unsigned pipeline, uint32_t shared,
                                    uint64_t requests) {
    cache_reset(4096);
    corrupt_reads = 0;
    pace_config cfg;
    pace_config_default(&cfg);
    cfg.driver = &fake;
    cfg.threads = threads;
    cfg.conns_per_thread = conns;
    cfg.pipeline = pipeline;
    cfg.scenario = PACE_SCENARIO_KVCACHE;
    cfg.kvcache = test_cfg();
    cfg.kvcache.shared_blocks = shared;
    cfg.preload = 0;
    cfg.requests = requests;
    static pace_result res;
    EXPECT(pace_run(&cfg, &res) == 0, "kvcache count run %u/%u/%u", threads, conns, pipeline);
    EXPECT(res.errors == 0, "%llu errors", (unsigned long long)res.errors);
    return res.kvcache;
}

/*
 * The asynchronous runner with 2 connections of 3 pipeline slots per thread runs 6 request machines with
 * identities 0..5, which make the same requests as 6 synchronous threads. Without template blocks no key
 * is shared between identities, so the counters must be equal.
 */
static void test_async_equivalence(void) {
    pace_kvcache_stats sync = run_count(6, 1, 1, 0, 150);
    pace_kvcache_stats async = run_count(1, 2, 3, 0, 150);
    EXPECT(sync.requests == 900 && async.requests == sync.requests && async.block_gets == sync.block_gets &&
               async.block_hits == sync.block_hits && async.block_puts == sync.block_puts &&
               async.bytes_get == sync.bytes_get && async.corrupt == 0 && sync.corrupt == 0,
           "async %llu/%llu/%llu/%llu, sync %llu/%llu/%llu/%llu (requests, gets, hits, puts)",
           (unsigned long long)async.requests, (unsigned long long)async.block_gets,
           (unsigned long long)async.block_hits, (unsigned long long)async.block_puts,
           (unsigned long long)sync.requests, (unsigned long long)sync.block_gets,
           (unsigned long long)sync.block_hits, (unsigned long long)sync.block_puts);
    /* with shared templates: verified hits, reuse from other machines' templates */
    pace_kvcache_stats shared = run_count(2, 2, 2, 6, 100);
    EXPECT(shared.requests == 800 && shared.block_hits > 0 && shared.corrupt == 0,
           "shared templates: %llu requests, %llu hits, %llu corrupt", (unsigned long long)shared.requests,
           (unsigned long long)shared.block_hits, (unsigned long long)shared.corrupt);
    /* corrupted values are detected through the asynchronous replies as well */
    cache_reset(4096);
    corrupt_reads = 1;
    pace_config cfg;
    pace_config_default(&cfg);
    cfg.driver = &fake;
    cfg.conns_per_thread = 2;
    cfg.pipeline = 2;
    cfg.scenario = PACE_SCENARIO_KVCACHE;
    cfg.kvcache = test_cfg();
    cfg.preload = 0;
    cfg.requests = 50;
    static pace_result res;
    EXPECT(pace_run(&cfg, &res) == 0 && res.kvcache.corrupt > 0 &&
               res.kvcache.corrupt == res.kvcache.block_hits,
           "corrupt %llu of %llu hits", (unsigned long long)res.kvcache.corrupt,
           (unsigned long long)res.kvcache.block_hits);
    corrupt_reads = 0;
}

/* Open loop through the asynchronous runner: requests at a fixed rate complete without errors. */
static void test_async_open_loop(void) {
    cache_reset(4096);
    pace_config cfg;
    pace_config_default(&cfg);
    cfg.driver = &fake;
    cfg.threads = 2;
    cfg.conns_per_thread = 2;
    cfg.pipeline = 4;
    cfg.rate = 2000;
    cfg.scenario = PACE_SCENARIO_KVCACHE;
    cfg.kvcache = test_cfg();
    cfg.preload = 0;
    cfg.warmup_s = 0.05;
    cfg.duration_s = 0.3;
    static pace_result res;
    EXPECT(pace_run(&cfg, &res) == 0, "open-loop kvcache run");
    double rate = (double)res.kvcache.requests / res.seconds;
    EXPECT(res.errors == 0 && res.kvcache.corrupt == 0 && rate > 1500 && rate < 2500,
           "open loop: %.0f requests/s at 2000/s, %llu errors", rate, (unsigned long long)res.errors);
}

/* The runner counts block operations as the operations of the result. */
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
    test_async_equivalence();
    test_async_open_loop();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_kvcache: ok");
    return 0;
}
