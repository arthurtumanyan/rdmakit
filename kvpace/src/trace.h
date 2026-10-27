/*
 * trace.h - request traces of an inference KV-cache, replayed by the kvcache scenario.
 *
 * Format: JSON lines in the layout of the Mooncake traces (FAST 2025, "Mooncake: A KVCache-centric
 * Disaggregated Architecture for LLM Serving"), one request per line:
 *
 *   {"timestamp": 27482, "input_length": 6955, "output_length": 52, "hash_ids": [46, 47, 48, ...]}
 *
 * timestamp is the arrival time in milliseconds from the start of the trace; hash_ids lists the blocks of
 * the request's prompt in order (one id per block of tokens). Requests that share a prompt prefix share
 * the ids of its blocks. Other fields are ignored. Lines are requests in arrival order: timestamps must
 * not decrease. Empty lines are skipped.
 */
#ifndef PACE_TRACE_H
#define PACE_TRACE_H

#include <stddef.h>
#include <stdint.h>

typedef struct pace_trace {
    uint64_t n;        /* requests */
    uint64_t *t_us;    /* arrival time of request i in microseconds from the start of the trace */
    uint64_t *first;   /* index of the first block id of request i in ids */
    uint32_t *nblocks; /* block ids of request i (at least 1) */
    uint64_t *ids;     /* block ids of all requests, request after request */
    uint64_t nids;
} pace_trace;

/* Loads a trace. Returns 0, or -1 with a message (file and line) in err. */
int pace_trace_load(const char *path, pace_trace *out, char *err, size_t errlen);

/* Frees the arrays of a trace. */
void pace_trace_free(pace_trace *t);

#endif
