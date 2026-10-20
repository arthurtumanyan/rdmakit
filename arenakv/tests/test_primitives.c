/*
 * test_primitives.c - tag matching and hash function tests.
 *
 * akv_match16() must equal the scalar reference for every combination of equal/unequal lanes; the hash
 * must change about half of its output bits when one input bit flips (avalanche) and distribute
 * sequential keys evenly over buckets.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hash.h"
#include "simd.h"

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

static uint64_t rng_state = 0x1234567890abcdefull;

static uint64_t rnd(void) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 11;
}

static void test_match(void) {
    uint16_t tags[16] __attribute__((aligned(16)));
    /* Every subset of lanes equal to the needle (2^16 patterns), distinct non-matching values elsewhere. */
    for (uint32_t pattern = 0; pattern < (1u << 16); pattern++) {
        uint16_t needle = (uint16_t)(0x1000 + (pattern & 0xff));
        for (unsigned i = 0; i < 16; i++) {
            tags[i] = (pattern >> i) & 1 ? needle : (uint16_t)(needle ^ 0x8000u ^ (1u << (i % 8)));
        }
        uint32_t got = akv_match16(tags, needle);
        uint32_t want = akv_match16_scalar(tags, needle);
        EXPECT(got == want && want == pattern, "pattern %x: got %x scalar %x", pattern, got, want);
    }
    for (int iter = 0; iter < 1000000; iter++) {
        for (unsigned i = 0; i < 16; i++) {
            tags[i] = (uint16_t)(rnd() & 0x3);
        }
        uint16_t needle = (uint16_t)(rnd() & 0x3);
        EXPECT(akv_match16(tags, needle) == akv_match16_scalar(tags, needle), "random lanes mismatch");
    }
    /* Extreme values: 0x0000, 0xFFFF, 0x8000, 0x7FFF. */
    static const uint16_t special[] = {0x0000, 0xffff, 0x8000, 0x7fff, 0x0001, 0xfffe};
    for (unsigned a = 0; a < 6; a++) {
        for (unsigned b = 0; b < 6; b++) {
            for (unsigned i = 0; i < 16; i++) {
                tags[i] = special[(i + a) % 6];
            }
            EXPECT(akv_match16(tags, special[b]) == akv_match16_scalar(tags, special[b]), "special values");
        }
    }
}

static void test_avalanche(void) {
    enum { LEN = 24, TRIALS = 20000 };
    uint8_t key[LEN];
    double total = 0.0;
    uint64_t flips = 0;
    for (int t = 0; t < TRIALS; t++) {
        for (int i = 0; i < LEN; i++) {
            key[i] = (uint8_t)rnd();
        }
        uint64_t h0 = akv_hash64(key, LEN, 77);
        unsigned bit = (unsigned)(rnd() % (LEN * 8));
        key[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        uint64_t h1 = akv_hash64(key, LEN, 77);
        total += (double)__builtin_popcountll(h0 ^ h1);
        flips++;
    }
    double mean = total / (double)flips;
    EXPECT(mean > 31.0 && mean < 33.0, "mean output bits changed per input bit flip: %.2f (expect about 32)",
           mean);
}

static void test_distribution(void) {
    enum { N = 1 << 20, BUCKETS = 4096 };
    static uint32_t count[BUCKETS];
    char key[24];
    for (int i = 0; i < N; i++) {
        int len = snprintf(key, sizeof(key), "k%015d", i);
        uint64_t h = akv_hash64(key, (size_t)len, 5);
        count[h % BUCKETS]++;
    }
    double mean = (double)N / BUCKETS;
    double chi = 0.0;
    for (int b = 0; b < BUCKETS; b++) {
        double d = (double)count[b] - mean;
        chi += d * d / mean;
    }
    /* Chi-square with 4095 degrees of freedom: mean 4095, sd about 90. Accept +-6 sd. */
    EXPECT(fabs(chi - (BUCKETS - 1)) < 6.0 * sqrt(2.0 * (BUCKETS - 1)), "chi-square %.1f for sequential keys",
           chi);
}

static void test_seed_and_length(void) {
    const char *a = "abcdefgh";
    EXPECT(akv_hash64(a, 8, 1) != akv_hash64(a, 8, 2), "seed must change the hash");
    EXPECT(akv_hash64(a, 7, 1) != akv_hash64(a, 8, 1), "length must change the hash");
    uint8_t zeros[16] = {0};
    EXPECT(akv_hash64(zeros, 8, 1) != akv_hash64(zeros, 16, 1), "zero keys of different length differ");
    EXPECT(akv_hash64("", 0, 1) == akv_hash64("", 0, 1), "empty input is deterministic");
}

int main(void) {
    printf("vector implementation: %s\n", AKV_SIMD_NAME);
    test_match();
    test_avalanche();
    test_distribution();
    test_seed_and_length();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_primitives: ok");
    return 0;
}
