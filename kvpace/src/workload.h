/*
 * workload.h - operation generator of the KV scenario.
 *
 * A workload draws operations from a weighted mix, keys from a distribution over the key space, and
 * value sizes from a fixed size, a uniform range, or a weighted list. All randomness comes from the
 * caller's generator, so a seed reproduces the sequence of each thread.
 *
 * Key distributions:
 *   uniform     every key with equal probability
 *   zipf        Zipfian with skew theta; popular ranks are scattered over the key space (zipf.h)
 *   sequential  each thread walks its own share of the key space in order and wraps
 *   gaussian    normal around the middle of the key space, standard deviation gauss_sd * keys, clamped
 *   latest      Zipfian over recency: rank 0 is the most recently inserted key (YCSB workload D)
 *
 * Operations: GET, SET, DEL, INCR (delta 1), MGET (mget_keys keys), RMW (GET then SET of one key, YCSB
 * workload F), INSERT (SET of a new key beyond the current key space; the key space grows).
 *
 * YCSB presets (Cooper et al., "Benchmarking Cloud Serving Systems with YCSB", SoCC 2010, table 2):
 *   ycsb-a  50% read, 50% update, zipfian       ycsb-b  95% read, 5% update, zipfian
 *   ycsb-c  100% read, zipfian                  ycsb-d  95% read, 5% insert, latest
 *   ycsb-f  50% read, 50% read-modify-write, zipfian
 * Workload E needs range scans, which key-value protocols do not offer; it is rejected.
 */
#ifndef PACE_WORKLOAD_H
#define PACE_WORKLOAD_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "rng.h"
#include "zipf.h"

typedef enum {
    PACE_OP_GET = 0,
    PACE_OP_SET = 1,
    PACE_OP_DEL = 2,
    PACE_OP_INCR = 3,
    PACE_OP_MGET = 4,
    PACE_OP_RMW = 5,
    PACE_OP_INSERT = 6,
    PACE_OP_COUNT = 7,
} pace_op_type;

typedef enum {
    PACE_DIST_UNIFORM = 0,
    PACE_DIST_ZIPF = 1,
    PACE_DIST_SEQUENTIAL = 2,
    PACE_DIST_GAUSSIAN = 3,
    PACE_DIST_LATEST = 4,
} pace_dist;

typedef enum {
    PACE_VSIZE_FIXED = 0,
    PACE_VSIZE_RANGE = 1,
    PACE_VSIZE_LIST = 2,
} pace_vsize_mode;

#define PACE_VSIZE_LIST_MAX 16u
#define PACE_KEY_PREFIX_MAX 32u
#define PACE_MGET_MAX 256u
/* Decimal digits after the prefix: keys are prefix + 15 digits, unique for indices below 10^15. */
#define PACE_KEY_DIGITS 15u
#define PACE_KEY_MAX (PACE_KEY_PREFIX_MAX + PACE_KEY_DIGITS)

typedef struct pace_workload {
    double weight[PACE_OP_COUNT]; /* relative weights; normalized by pace_wl_init */
    double cum[PACE_OP_COUNT];    /* cumulative probabilities */
    pace_dist dist;
    double theta;    /* zipf and latest */
    double gauss_sd; /* fraction of the key space; 0 selects 1/6 */
    uint64_t nkeys;
    pace_vsize_mode vmode;
    unsigned vmin;
    unsigned vmax;
    unsigned vsizes[PACE_VSIZE_LIST_MAX];
    double vcum[PACE_VSIZE_LIST_MAX];
    unsigned nvsizes;
    unsigned mget_keys;
    char prefix[PACE_KEY_PREFIX_MAX + 1];
    size_t prefix_len;
    pace_zipf zipf;
    int has_zipf;
    atomic_uint_fast64_t inserted; /* keys in the key space, grows with INSERT */
} pace_workload;

/* Per-thread state of the generator. */
typedef struct pace_wl_thread {
    uint64_t cursor; /* sequential distribution */
    uint64_t start;
    uint64_t span;
} pace_wl_thread;

/* One generated operation. keys[1..] are used by MGET only. */
typedef struct pace_op {
    pace_op_type type;
    uint64_t key;
    uint64_t keys[PACE_MGET_MAX];
    unsigned nkeys;
    unsigned value_size;
} pace_op;

/*
 * Sets defaults: 90% GET / 10% SET, uniform keys, fixed 64-byte values, prefix "k", one key per MGET.
 */
void pace_wl_default(pace_workload *w);

/*
 * Validates and prepares the workload for nkeys keys (weights, cumulative tables, Zipf constants).
 * Returns 0, or -1 with a message in err (capacity errlen).
 */
int pace_wl_init(pace_workload *w, char *err, size_t errlen);

/* Parses a memtier-style ratio "SET:GET" (for example "1:10") into the mix. Returns 0 or -1. */
int pace_wl_parse_ratio(pace_workload *w, const char *s);

/* Parses a mix "get=80,set=15,del=3,incr=2,mget=0,rmw=0,insert=0" (weights). Returns 0 or -1. */
int pace_wl_parse_mix(pace_workload *w, const char *s);

/* Parses a value size specification: "N", "A-B" (uniform range) or "S:W,S:W,..." (weighted list). */
int pace_wl_parse_vsize(pace_workload *w, const char *s);

/* Applies a YCSB preset (ycsb-a, ycsb-b, ycsb-c, ycsb-d, ycsb-f). Returns 0, or -1 for unknown or E. */
int pace_wl_preset(pace_workload *w, const char *name, char *err, size_t errlen);

/* Prepares the state of thread id of nthreads. */
void pace_wl_thread_init(const pace_workload *w, pace_wl_thread *t, unsigned id, unsigned nthreads);

/* Draws the next operation. */
void pace_wl_next(pace_workload *w, pace_wl_thread *t, pace_rng *r, pace_op *op);

/* Draws one key index of the configured distribution. */
uint64_t pace_wl_key(pace_workload *w, pace_wl_thread *t, pace_rng *r);

/* Draws a value size. */
unsigned pace_wl_vsize(const pace_workload *w, pace_rng *r);

/* Largest value size the workload can produce. */
unsigned pace_wl_vsize_max(const pace_workload *w);

/* Writes the key of index idx into out (at least PACE_KEY_MAX bytes) and returns its length. */
size_t pace_wl_key_text(const pace_workload *w, uint64_t idx, char *out);

/* Name of an operation type ("get", "set", ...). */
const char *pace_op_name(pace_op_type t);

#endif
