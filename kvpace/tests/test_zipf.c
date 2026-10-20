/*
 * test_zipf.c - generator distribution tests.
 *
 * Compares empirical rank frequencies of the Zipfian generator against the
 * exact probabilities 1 / ((i + 1)^theta * zeta(n, theta)), checks theta = 0
 * for uniformity, and checks that the generator stays within range.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rng.h"
#include "zipf.h"

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

enum { N = 1000, SAMPLES = 4000000 };

static void test_skewed(void) {
    pace_zipf z;
    EXPECT(pace_zipf_init(&z, N, 0.99) == 0, "init");
    pace_rng rng;
    pace_rng_seed(&rng, 11);
    static uint64_t count[N];
    for (int i = 0; i < SAMPLES; i++) {
        uint64_t r = pace_zipf_next(&z, &rng);
        EXPECT(r < N, "rank %" PRIu64 " out of range", r);
        if (r < N) {
            count[r]++;
        }
    }
    double zeta = 0.0;
    for (int i = 1; i <= N; i++) {
        zeta += 1.0 / pow(i, 0.99);
    }
    /* The two most probable ranks are generated exactly. */
    for (int i = 0; i < 2; i++) {
        double expect = 1.0 / (pow(i + 1, 0.99) * zeta);
        double got = (double)count[i] / SAMPLES;
        EXPECT(fabs(got - expect) / expect < 0.02, "rank %d: expected %.5f, got %.5f", i, expect, got);
    }
    /* Mass of the top 100 ranks, including the approximated range. */
    double expect_top = 0.0;
    uint64_t got_top = 0;
    for (int i = 0; i < 100; i++) {
        expect_top += 1.0 / (pow(i + 1, 0.99) * zeta);
        got_top += count[i];
    }
    double got_frac = (double)got_top / SAMPLES;
    EXPECT(fabs(got_frac - expect_top) < 0.03, "top-100 mass: expected %.4f, got %.4f", expect_top, got_frac);
    EXPECT(count[0] > count[1] && count[1] > count[5] && count[5] > count[500],
           "rank order is not decreasing");
}

static void test_uniform(void) {
    pace_zipf z;
    EXPECT(pace_zipf_init(&z, N, 0.0) == 0, "init theta=0");
    pace_rng rng;
    pace_rng_seed(&rng, 5);
    static uint64_t count[N];
    for (int i = 0; i < SAMPLES; i++) {
        uint64_t r = pace_zipf_next(&z, &rng);
        if (r < N) {
            count[r]++;
        }
    }
    double mean = (double)SAMPLES / N;
    for (int i = 0; i < N; i++) {
        EXPECT(fabs(count[i] - mean) < 6.0 * sqrt(mean), "bucket %d count %" PRIu64 " (mean %.1f)", i,
               count[i], mean);
    }
}

static void test_invalid(void) {
    pace_zipf z;
    EXPECT(pace_zipf_init(&z, 0, 0.5) != 0, "n = 0 rejected");
    EXPECT(pace_zipf_init(&z, 10, 1.0) != 0, "theta = 1 rejected");
    EXPECT(pace_zipf_init(&z, 10, -0.1) != 0, "negative theta rejected");
    EXPECT(pace_zipf_init(&z, 1, 0.5) == 0, "n = 1 accepted");
    pace_rng rng;
    pace_rng_seed(&rng, 1);
    EXPECT(pace_zipf_next(&z, &rng) == 0, "n = 1 always yields rank 0");
}

static void test_scramble(void) {
    for (uint64_t r = 0; r < 100000; r++) {
        EXPECT(pace_zipf_scramble(r, 777) < 777, "scramble out of range");
    }
}

int main(void) {
    test_skewed();
    test_uniform();
    test_invalid();
    test_scramble();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_zipf: ok");
    return 0;
}
