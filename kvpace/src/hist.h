/*
 * hist.h - log-linear latency histogram.
 *
 * Values are unsigned 64-bit integers (nanoseconds in kvpace). Values below
 * 2^PACE_HIST_SUB_BITS are stored exactly. Larger values fall into one of
 * 2^PACE_HIST_SUB_BITS sub-buckets per power of two, which bounds the relative
 * quantization error to 2^-PACE_HIST_SUB_BITS (0.78% for 7 bits).
 *
 * Recording is O(1), allocation-free and uses no atomics: each histogram is
 * owned by one thread and merged after the measurement ends.
 */
#ifndef PACE_HIST_H
#define PACE_HIST_H

#include <stdint.h>

#define PACE_HIST_SUB_BITS 7u
#define PACE_HIST_SUB (1u << PACE_HIST_SUB_BITS)
#define PACE_HIST_GROUPS (64u - PACE_HIST_SUB_BITS + 1u)
#define PACE_HIST_BUCKETS (PACE_HIST_GROUPS * PACE_HIST_SUB)

typedef struct pace_hist {
    uint64_t counts[PACE_HIST_BUCKETS];
    uint64_t total;
    uint64_t sum;
    uint64_t min;
    uint64_t max;
} pace_hist;

/* Resets all counters. */
void pace_hist_reset(pace_hist *h);

/* Maps a value to its bucket index in [0, PACE_HIST_BUCKETS). */
static inline unsigned pace_hist_index(uint64_t v) {
    if (v < PACE_HIST_SUB) {
        return (unsigned)v;
    }
    unsigned msb = 63u - (unsigned)__builtin_clzll(v);
    unsigned shift = msb - PACE_HIST_SUB_BITS;
    return ((shift + 1u) << PACE_HIST_SUB_BITS) + (unsigned)((v >> shift) - PACE_HIST_SUB);
}

/* Smallest value mapped to bucket idx. */
uint64_t pace_hist_bucket_low(unsigned idx);

/* Largest value mapped to bucket idx. */
uint64_t pace_hist_bucket_high(unsigned idx);

/* Records one sample. */
static inline void pace_hist_record(pace_hist *h, uint64_t v) {
    h->counts[pace_hist_index(v)]++;
    h->total++;
    h->sum += v;
    if (v < h->min) {
        h->min = v;
    }
    if (v > h->max) {
        h->max = v;
    }
}

/* Adds all samples of src to dst. */
void pace_hist_merge(pace_hist *dst, const pace_hist *src);

/*
 * Returns the value at quantile q in [0, 1]: the upper bound of the bucket that
 * contains the sample of rank ceil(q * total), clamped to [min, max].
 * Returns 0 for an empty histogram.
 */
uint64_t pace_hist_quantile(const pace_hist *h, double q);

/* Arithmetic mean of the recorded samples; 0 for an empty histogram. */
double pace_hist_mean(const pace_hist *h);

#endif
