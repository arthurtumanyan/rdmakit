/* hist.c - log-linear latency histogram, see hist.h. */
#include "hist.h"

#include <math.h>
#include <string.h>

void pace_hist_reset(pace_hist *h) {
    memset(h, 0, sizeof(*h));
    h->min = UINT64_MAX;
}

uint64_t pace_hist_bucket_low(unsigned idx) {
    if (idx < PACE_HIST_SUB) {
        return idx;
    }
    unsigned group = idx >> PACE_HIST_SUB_BITS;
    unsigned sub = idx & (PACE_HIST_SUB - 1u);
    return (uint64_t)(PACE_HIST_SUB + sub) << (group - 1u);
}

uint64_t pace_hist_bucket_high(unsigned idx) {
    if (idx < PACE_HIST_SUB) {
        return idx;
    }
    unsigned group = idx >> PACE_HIST_SUB_BITS;
    return pace_hist_bucket_low(idx) + ((1ull << (group - 1u)) - 1u);
}

void pace_hist_merge(pace_hist *dst, const pace_hist *src) {
    for (unsigned i = 0; i < PACE_HIST_BUCKETS; i++) {
        dst->counts[i] += src->counts[i];
    }
    dst->total += src->total;
    dst->sum += src->sum;
    if (src->min < dst->min) {
        dst->min = src->min;
    }
    if (src->max > dst->max) {
        dst->max = src->max;
    }
}

uint64_t pace_hist_quantile(const pace_hist *h, double q) {
    if (h->total == 0) {
        return 0;
    }
    if (q <= 0.0) {
        return h->min;
    }
    if (q >= 1.0) {
        return h->max;
    }
    uint64_t rank = (uint64_t)ceil(q * (double)h->total);
    if (rank == 0) {
        rank = 1;
    }
    uint64_t seen = 0;
    for (unsigned i = 0; i < PACE_HIST_BUCKETS; i++) {
        seen += h->counts[i];
        if (seen >= rank) {
            uint64_t v = pace_hist_bucket_high(i);
            if (v > h->max) {
                v = h->max;
            }
            if (v < h->min) {
                v = h->min;
            }
            return v;
        }
    }
    return h->max;
}

double pace_hist_mean(const pace_hist *h) {
    if (h->total == 0) {
        return 0.0;
    }
    return (double)h->sum / (double)h->total;
}
