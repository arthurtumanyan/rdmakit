/*
 * zipf.h - Zipfian rank generator.
 *
 * Implements the rejection-free generator of Gray et al., "Quickly Generating
 * Billion-Record Synthetic Databases" (SIGMOD 1994), as used by YCSB. Rank i
 * (0-based) is drawn with probability proportional to 1 / (i + 1)^theta.
 *
 * theta = 0 yields a uniform distribution. theta must be in [0, 1); 0.99 is the
 * YCSB default. The generator is approximate for ranks beyond the two most
 * probable ones.
 *
 * Initialization computes the generalized harmonic number in O(n) time.
 */
#ifndef PACE_ZIPF_H
#define PACE_ZIPF_H

#include <stdint.h>

#include "rng.h"

typedef struct pace_zipf {
    uint64_t n;
    double theta;
    double alpha;
    double zetan;
    double eta;
    double half_pow_theta;
} pace_zipf;

/* Initializes the generator for ranks [0, n). Returns 0, or -1 for invalid n or theta. */
int pace_zipf_init(pace_zipf *z, uint64_t n, double theta);

/* Draws a rank in [0, n). */
uint64_t pace_zipf_next(const pace_zipf *z, pace_rng *r);

/*
 * Maps a rank to a key index in [0, n) with a fixed pseudo-random function so that
 * popular ranks are spread over the key space instead of clustering at low indices.
 * The mapping is not a permutation: distinct ranks may collide.
 */
uint64_t pace_zipf_scramble(uint64_t rank, uint64_t n);

#endif
