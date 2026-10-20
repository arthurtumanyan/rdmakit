/*
 * test_workload.c - operation generator.
 *
 *   - parsing of ratios, mixes, value size specifications and YCSB presets (including the rejection of E)
 *   - operation frequencies match the mix within 1% over 200,000 draws
 *   - sequential keys cover each thread's share exactly once per pass
 *   - gaussian keys: mean in the middle of the key space and the configured standard deviation (within 3%)
 *   - latest keys favour recently inserted keys; INSERT grows the key space
 *   - zipf keys: the most popular key is drawn far more often than under uniform keys
 *   - value sizes: range bounds, list frequencies within 1%
 *   - key text: prefix and 15 digits; counter keys use their own namespace
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runner.h"
#include "workload.h"

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

static void init(pace_workload *w) {
    char err[160];
    int rc = pace_wl_init(w, err, sizeof(err));
    EXPECT(rc == 0, "init: %s", err);
}

static void test_parsing(void) {
    pace_workload w;
    pace_wl_default(&w);
    EXPECT(pace_wl_parse_ratio(&w, "1:10") == 0 && w.weight[PACE_OP_SET] == 1 && w.weight[PACE_OP_GET] == 10,
           "ratio");
    EXPECT(pace_wl_parse_ratio(&w, "1") != 0 && pace_wl_parse_ratio(&w, "a:b") != 0 &&
               pace_wl_parse_ratio(&w, "0:0") != 0,
           "bad ratios rejected");
    EXPECT(pace_wl_parse_mix(&w, "get=80,set=15,del=3,incr=2") == 0 && w.weight[PACE_OP_DEL] == 3 &&
               w.weight[PACE_OP_MGET] == 0,
           "mix");
    EXPECT(pace_wl_parse_mix(&w, "get=1,bogus=2") != 0 && pace_wl_parse_mix(&w, "get") != 0,
           "bad mixes rejected");
    EXPECT(pace_wl_parse_vsize(&w, "128") == 0 && w.vmode == PACE_VSIZE_FIXED && w.vmin == 128, "fixed size");
    EXPECT(pace_wl_parse_vsize(&w, "100-200") == 0 && w.vmode == PACE_VSIZE_RANGE && w.vmax == 200, "range");
    EXPECT(pace_wl_parse_vsize(&w, "64:3,1024:1") == 0 && w.vmode == PACE_VSIZE_LIST && w.nvsizes == 2,
           "list");
    EXPECT(pace_wl_parse_vsize(&w, "0") != 0 && pace_wl_parse_vsize(&w, "9-3") != 0 &&
               pace_wl_parse_vsize(&w, "64:0") != 0,
           "bad sizes rejected");
    char err[160];
    EXPECT(pace_wl_preset(&w, "ycsb-d", err, sizeof(err)) == 0 && w.dist == PACE_DIST_LATEST &&
               w.weight[PACE_OP_INSERT] == 5,
           "ycsb-d");
    EXPECT(pace_wl_preset(&w, "ycsb-e", err, sizeof(err)) != 0 && strstr(err, "scan") != NULL,
           "ycsb-e rejected");
    EXPECT(pace_wl_preset(&w, "ycsb-x", err, sizeof(err)) != 0, "unknown preset rejected");
}

static void test_mix_frequencies(void) {
    pace_workload w;
    pace_wl_default(&w);
    pace_wl_parse_mix(&w, "get=50,set=20,del=10,incr=10,mget=5,rmw=5");
    init(&w);
    pace_wl_thread t;
    pace_wl_thread_init(&w, &t, 0, 1);
    pace_rng r;
    pace_rng_seed(&r, 7);
    unsigned count[PACE_OP_COUNT] = {0};
    enum { N = 200000 };
    pace_op op;
    for (unsigned i = 0; i < N; i++) {
        pace_wl_next(&w, &t, &r, &op);
        count[op.type]++;
    }
    static const double want[PACE_OP_COUNT] = {0.5, 0.2, 0.1, 0.1, 0.05, 0.05, 0.0};
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        double f = (double)count[i] / N;
        EXPECT(fabs(f - want[i]) < 0.01, "%s frequency %.4f, expected %.2f", pace_op_name((pace_op_type)i), f,
               want[i]);
    }
}

static void test_distributions(void) {
    pace_workload w;
    pace_rng r;
    pace_rng_seed(&r, 11);
    /* sequential: two threads, each covers its half exactly once per pass */
    pace_wl_default(&w);
    w.nkeys = 1000;
    w.dist = PACE_DIST_SEQUENTIAL;
    init(&w);
    unsigned char seen[1000] = {0};
    for (unsigned id = 0; id < 2; id++) {
        pace_wl_thread t;
        pace_wl_thread_init(&w, &t, id, 2);
        for (unsigned i = 0; i < 500; i++) {
            seen[pace_wl_key(&w, &t, &r)]++;
        }
    }
    int once = 1;
    for (unsigned i = 0; i < 1000; i++) {
        once &= seen[i] == 1;
    }
    EXPECT(once, "sequential keys cover the key space exactly once");

    /* gaussian */
    pace_wl_default(&w);
    w.nkeys = 1000000;
    w.dist = PACE_DIST_GAUSSIAN;
    w.gauss_sd = 0.1;
    init(&w);
    pace_wl_thread t;
    pace_wl_thread_init(&w, &t, 0, 1);
    double sum = 0;
    double sq = 0;
    enum { G = 200000 };
    for (unsigned i = 0; i < G; i++) {
        double k = (double)pace_wl_key(&w, &t, &r);
        sum += k;
        sq += k * k;
    }
    double mean = sum / G;
    double sd = sqrt(sq / G - mean * mean);
    EXPECT(fabs(mean - 500000.0) < 3000.0, "gaussian mean %.0f", mean);
    EXPECT(fabs(sd - 100000.0) < 3000.0, "gaussian standard deviation %.0f", sd);

    /* latest: after inserts, recent keys dominate */
    pace_wl_default(&w);
    w.nkeys = 10000;
    pace_wl_parse_mix(&w, "insert=1");
    w.dist = PACE_DIST_LATEST;
    init(&w);
    pace_wl_thread_init(&w, &t, 0, 1);
    pace_op op;
    for (unsigned i = 0; i < 1000; i++) {
        pace_wl_next(&w, &t, &r, &op);
    }
    EXPECT(atomic_load(&w.inserted) == 11000 && op.key == 10999, "inserts grow the key space to %llu",
           (unsigned long long)atomic_load(&w.inserted));
    unsigned recent = 0;
    for (unsigned i = 0; i < 20000; i++) {
        recent += pace_wl_key(&w, &t, &r) >= 10000;
    }
    EXPECT(recent > 20000 * 0.5, "latest: %u of 20000 draws among the 1000 newest keys", recent);

    /* zipf: concentration on the most popular key */
    pace_wl_default(&w);
    w.nkeys = 100000;
    w.dist = PACE_DIST_ZIPF;
    w.theta = 0.99;
    init(&w);
    pace_wl_thread_init(&w, &t, 0, 1);
    static unsigned hist[100000];
    memset(hist, 0, sizeof(hist));
    for (unsigned i = 0; i < 200000; i++) {
        hist[pace_wl_key(&w, &t, &r)]++;
    }
    unsigned top = 0;
    for (unsigned i = 0; i < 100000; i++) {
        top = hist[i] > top ? hist[i] : top;
    }
    EXPECT(top > 5000, "zipf: most popular key drawn %u times (uniform: about 2)", top);
}

static void test_value_sizes(void) {
    pace_workload w;
    pace_wl_default(&w);
    pace_rng r;
    pace_rng_seed(&r, 5);
    pace_wl_parse_vsize(&w, "100-200");
    unsigned lo = 1000;
    unsigned hi = 0;
    for (unsigned i = 0; i < 100000; i++) {
        unsigned v = pace_wl_vsize(&w, &r);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    EXPECT(lo == 100 && hi == 200, "range bounds %u..%u", lo, hi);
    pace_wl_parse_vsize(&w, "64:3,1024:1");
    unsigned small = 0;
    enum { N = 200000 };
    for (unsigned i = 0; i < N; i++) {
        small += pace_wl_vsize(&w, &r) == 64;
    }
    EXPECT(fabs((double)small / N - 0.75) < 0.01, "list frequency %.4f, expected 0.75", (double)small / N);
    EXPECT(pace_wl_vsize_max(&w) == 1024, "largest list size");
}

static void test_keys(void) {
    pace_config cfg;
    pace_config_default(&cfg);
    char key[PACE_KEY_MAX + 2];
    size_t n = pace_key_text(&cfg, 42, 0, key);
    EXPECT(n == 16 && memcmp(key, "k000000000000042", 16) == 0, "default key %.*s", (int)n, key);
    n = pace_key_text(&cfg, 42, 1, key);
    EXPECT(n == 17 && memcmp(key, "kc000000000000042", 17) == 0, "counter key %.*s", (int)n, key);
    strcpy(cfg.wl.prefix, "user:");
    cfg.wl.prefix_len = 5;
    n = pace_key_text(&cfg, 7, 0, key);
    EXPECT(n == 20 && memcmp(key, "user:000000000000007", 20) == 0, "prefixed key %.*s", (int)n, key);
}

int main(void) {
    test_parsing();
    test_mix_frequencies();
    test_distributions();
    test_value_sizes();
    test_keys();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_workload: ok");
    return 0;
}
