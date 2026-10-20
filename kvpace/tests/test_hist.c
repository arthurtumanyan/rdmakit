/*
 * test_hist.c - histogram accuracy tests.
 *
 * Verifies: exact storage below 2^SUB_BITS, bucket bounds contain their
 * values, relative quantization error <= 2^-SUB_BITS over the full 64-bit
 * range, quantile selection, and merge.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "hist.h"
#include "rng.h"

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

static void test_exact_range(void) {
    for (uint64_t v = 0; v < PACE_HIST_SUB; v++) {
        unsigned idx = pace_hist_index(v);
        EXPECT(idx == v, "index of %" PRIu64 " is %u", v, idx);
        EXPECT(pace_hist_bucket_low(idx) == v && pace_hist_bucket_high(idx) == v, "bounds of exact bucket %u",
               idx);
    }
}

static void test_bounds_and_error(void) {
    pace_rng rng;
    pace_rng_seed(&rng, 7);
    for (int i = 0; i < 2000000; i++) {
        uint64_t v = pace_rng_next(&rng) >> (pace_rng_next(&rng) & 63);
        unsigned idx = pace_hist_index(v);
        EXPECT(idx < PACE_HIST_BUCKETS, "index %u out of range for %" PRIu64, idx, v);
        uint64_t lo = pace_hist_bucket_low(idx);
        uint64_t hi = pace_hist_bucket_high(idx);
        EXPECT(lo <= v && v <= hi, "value %" PRIu64 " outside [%" PRIu64 ", %" PRIu64 "]", v, lo, hi);
        if (v >= PACE_HIST_SUB) {
            double err = (double)(hi - lo) / (double)lo;
            EXPECT(err <= 1.0 / PACE_HIST_SUB + 1e-12, "relative width %g for %" PRIu64, err, v);
        }
    }
    EXPECT(pace_hist_index(UINT64_MAX) == PACE_HIST_BUCKETS - 1, "max value maps to last bucket");
    EXPECT(pace_hist_bucket_high(PACE_HIST_BUCKETS - 1) == UINT64_MAX, "last bucket reaches UINT64_MAX");
}

static void test_adjacent_buckets(void) {
    for (unsigned i = 1; i < PACE_HIST_BUCKETS; i++) {
        EXPECT(pace_hist_bucket_low(i) == pace_hist_bucket_high(i - 1) + 1, "gap or overlap before bucket %u",
               i);
    }
}

static void test_quantiles(void) {
    pace_hist h;
    pace_hist_reset(&h);
    EXPECT(pace_hist_quantile(&h, 0.5) == 0, "empty histogram quantile");
    for (uint64_t v = 1; v <= 100; v++) {
        pace_hist_record(&h, v);
    }
    EXPECT(h.total == 100 && h.min == 1 && h.max == 100, "counters");
    EXPECT(pace_hist_quantile(&h, 0.5) == 50, "p50 = %" PRIu64, pace_hist_quantile(&h, 0.5));
    EXPECT(pace_hist_quantile(&h, 0.99) == 99, "p99 = %" PRIu64, pace_hist_quantile(&h, 0.99));
    EXPECT(pace_hist_quantile(&h, 0.0) == 1 && pace_hist_quantile(&h, 1.0) == 100, "extremes");
    EXPECT(pace_hist_mean(&h) == 50.5, "mean = %g", pace_hist_mean(&h));

    pace_hist big;
    pace_hist_reset(&big);
    for (int i = 0; i < 1000; i++) {
        pace_hist_record(&big, 1000000);
    }
    uint64_t p = pace_hist_quantile(&big, 0.5);
    EXPECT(p >= 1000000 && p <= 1000000 + 1000000 / PACE_HIST_SUB, "p50 of constant series = %" PRIu64, p);
}

static void test_merge(void) {
    pace_hist a;
    pace_hist b;
    pace_hist_reset(&a);
    pace_hist_reset(&b);
    for (uint64_t v = 1; v <= 50; v++) {
        pace_hist_record(&a, v);
    }
    for (uint64_t v = 51; v <= 100; v++) {
        pace_hist_record(&b, v);
    }
    pace_hist_merge(&a, &b);
    EXPECT(a.total == 100 && a.min == 1 && a.max == 100, "merged counters");
    EXPECT(pace_hist_quantile(&a, 0.5) == 50, "merged p50");
}

int main(void) {
    test_exact_range();
    test_bounds_and_error();
    test_adjacent_buckets();
    test_quantiles();
    test_merge();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_hist: ok");
    return 0;
}
