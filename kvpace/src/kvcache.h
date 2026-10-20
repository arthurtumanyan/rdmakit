/*
 * kvcache.h - KV-cache block workload ("prefix chains").
 *
 * Models the access pattern of an inference KV-cache store. A request needs the cache blocks of a token
 * prefix. Blocks are immutable and keyed by their position in a chain, so requests that share a prefix
 * share blocks.
 *
 * Structure of one request:
 *   - The prompt consists of `shared_blocks` template blocks (identical for every request of a prefix
 *     group, for example a system prompt) followed by the blocks of a conversation: turn t of a
 *     conversation has (t + 1) * turn_blocks conversation blocks (all earlier turns plus the new one).
 *   - The store is probed with GET for the blocks in order until the first miss (longest cached prefix).
 *   - Every block from the first miss to the end of the chain is written with PUT (the recomputed tail).
 *
 * Per thread there are `active_conv` conversation slots. A request picks a slot with a Zipfian
 * distribution; with probability `new_conv_prob` the slot is first replaced by a new conversation.
 * Each request advances the turn counter of its slot. Prefix groups are drawn with a Zipfian
 * distribution as well. All thread states are independent, so there is no shared mutable state.
 *
 * Block content is a deterministic function of the block key; hits are verified byte for byte.
 */
#ifndef PACE_KVCACHE_H
#define PACE_KVCACHE_H

#include <stddef.h>
#include <stdint.h>

#include "driver.h"
#include "rng.h"
#include "zipf.h"

typedef struct pace_kvcache_cfg {
    uint32_t block_size;    /* bytes per block */
    uint32_t groups;        /* prefix groups (distinct templates) */
    uint32_t shared_blocks; /* template blocks per request */
    uint32_t turn_blocks;   /* conversation blocks added per turn */
    uint32_t active_conv;   /* conversation slots per thread */
    uint32_t max_turns;     /* a conversation restarts after this many turns */
    double new_conv_prob;   /* probability that a request starts a new conversation */
    double theta;           /* Zipf skew for group and slot selection, in [0, 1) */
    uint64_t seed;          /* workload identity: keys of different seeds never collide */
} pace_kvcache_cfg;

/*
 * Counters of one request (or the sum over requests).
 *
 * Because probing stops at the first miss, a request has at most one missing GET; the probe hit rate
 * block_hits / block_gets therefore overstates cache effectiveness. The reuse rate
 * block_hits / (block_hits + block_puts) is the fraction of needed blocks that did not have to be
 * recomputed.
 */
typedef struct pace_kvcache_stats {
    uint64_t requests;
    uint64_t block_gets;
    uint64_t block_hits;
    uint64_t block_puts;
    uint64_t bytes_get;
    uint64_t bytes_put;
    uint64_t corrupt; /* hits whose content differs from the expected block */
} pace_kvcache_stats;

typedef struct pace_kvcache_state pace_kvcache_state;

/* Sets default parameters (16 KiB blocks, 16 groups, 32 shared blocks, 8 blocks per turn, 256 slots). */
void pace_kvcache_defaults(pace_kvcache_cfg *cfg);

/* Creates the per-thread state. thread_id and thread_seed make threads independent. Returns NULL on failure. */
pace_kvcache_state *pace_kvcache_create(const pace_kvcache_cfg *cfg, unsigned thread_id,
                                        uint64_t thread_seed);

/* Frees the state. */
void pace_kvcache_destroy(pace_kvcache_state *st);

/*
 * Executes one request on conn and adds its counters to *out. Returns PACE_OK or PACE_ERR (driver error;
 * counters of the partially executed request are still added).
 */
int pace_kvcache_request(pace_kvcache_state *st, const pace_driver *drv, pace_conn *conn,
                         pace_kvcache_stats *out);

/* Writes the key of a block into key (at least PACE_KVCACHE_KEY_LEN bytes) and returns the key length. */
#define PACE_KVCACHE_KEY_LEN 17
size_t pace_kvcache_block_key(uint64_t workload_seed, uint64_t group, uint64_t conv, uint64_t index,
                              char *key);

#endif
