/*
 * rng.h - xoshiro256** pseudo-random generator (Blackman, Vigna 2018).
 *
 * Not cryptographically secure. State is seeded from a 64-bit value through
 * splitmix64. One generator instance per thread; instances are not thread-safe.
 */
#ifndef PACE_RNG_H
#define PACE_RNG_H

#include <stdint.h>

typedef struct pace_rng {
    uint64_t s[4];
} pace_rng;

/* splitmix64 step: advances *state and returns the next output. */
static inline uint64_t pace_splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Seeds the generator; distinct seeds give independent streams. */
static inline void pace_rng_seed(pace_rng *r, uint64_t seed) {
    uint64_t sm = seed;
    for (int i = 0; i < 4; i++) {
        r->s[i] = pace_splitmix64(&sm);
    }
}

static inline uint64_t pace_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* Returns 64 uniformly distributed bits. */
static inline uint64_t pace_rng_next(pace_rng *r) {
    uint64_t result = pace_rotl(r->s[1] * 5u, 7) * 9u;
    uint64_t t = r->s[1] << 17;
    r->s[2] ^= r->s[0];
    r->s[3] ^= r->s[1];
    r->s[1] ^= r->s[2];
    r->s[0] ^= r->s[3];
    r->s[2] ^= t;
    r->s[3] = pace_rotl(r->s[3], 45);
    return result;
}

/* Returns a double uniformly distributed in [0, 1) with 53 bits of resolution. */
static inline double pace_rng_double(pace_rng *r) {
    return (double)(pace_rng_next(r) >> 11) * 0x1.0p-53;
}

/* Returns an integer in [0, bound). bound must be non-zero. Uses multiply-shift (bias below 2^-64 * bound). */
static inline uint64_t pace_rng_below(pace_rng *r, uint64_t bound) {
    return (uint64_t)(((unsigned __int128)pace_rng_next(r) * bound) >> 64);
}

#endif
