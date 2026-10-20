/*
 * akv.h - in-memory key-value cache engine.
 *
 * The engine stores byte-string keys and values in a fixed amount of memory and evicts old entries when
 * the memory is full. It is a cache: an entry may disappear at any time after it was stored, but a lookup
 * never returns a value other than the most recent one stored for the key.
 *
 * Structure:
 *   - The key space is partitioned into shards by hash. Each shard has its own lock, arena and index, so
 *     operations on different shards run in parallel.
 *   - A shard arena is a circular log. New entries are appended at the head; space is reclaimed at the
 *     tail. Overwritten and deleted entries stay in the log as dead space until the tail passes them.
 *   - The index is an open-addressing hash table of 128-byte buckets (16 slots: 16-bit tags and 48-bit
 *     arena offsets). A lookup compares the 16 tags of two adjacent buckets with vector instructions and
 *     verifies the key only for tag matches.
 *   - A put of the same total entry size as the existing entry overwrites it in place.
 *   - Eviction follows the log order with a second chance: an entry that was read since it was written (or
 *     since its last reinsertion) is copied to the head once instead of being evicted.
 *
 * Thread safety: all functions are thread-safe. Operations lock one shard.
 */
#ifndef AKV_H
#define AKV_H

#include <stddef.h>
#include <stdint.h>

typedef struct akv akv;

typedef struct akv_config {
    uint64_t capacity;  /* total arena memory in bytes, split evenly over the shards */
    uint32_t shards;    /* number of shards, power of two in [1, 1024]; 0 selects 64 */
    uint32_t max_entry; /* largest accepted key + value size in bytes; 0 selects 1 MiB */
    uint32_t avg_entry; /* expected mean entry size in bytes, used to size the index; 0 selects 256 */
    uint64_t hash_seed; /* seed of the key hash; 0 selects a random seed */
    int huge_pages;     /* request transparent huge pages for the arenas (advisory) */
} akv_config;

typedef struct akv_stats {
    uint64_t entries;     /* live entries */
    uint64_t arena_bytes; /* total arena size */
    uint64_t log_bytes;   /* bytes between tail and head (live + dead entries) */
    uint64_t gets;
    uint64_t hits;
    uint64_t puts;
    uint64_t overwrites; /* puts that replaced an existing entry */
    uint64_t dels;
    uint64_t evictions;       /* live entries removed to make room */
    uint64_t reinsertions;    /* live entries given a second chance */
    uint64_t index_evictions; /* live entries removed because their index window was full */
    uint64_t rejected;        /* puts refused (entry too large) */
} akv_stats;

/* Fills cfg with defaults: 256 MiB, 64 shards, 1 MiB entries. */
void akv_config_default(akv_config *cfg);

/* Creates an engine. Returns NULL on invalid configuration or when memory cannot be allocated. */
akv *akv_create(const akv_config *cfg);

/* Frees an engine. No operation may be in progress. */
void akv_destroy(akv *s);

/*
 * Stores value under key, replacing an existing entry. Returns 0, -EINVAL (empty key or key longer than
 * 65535 bytes), or -E2BIG (key + value larger than max_entry or than half a shard arena).
 */
int akv_put(akv *s, const void *key, size_t klen, const void *val, size_t vlen);

/*
 * Copies the value of key into buf (capacity cap) and stores its length in *vlen.
 * Returns 0 on a hit, -ENOENT when the key is absent, or -EMSGSIZE when the value is larger than cap
 * (*vlen then holds the required size and buf is untouched).
 */
int akv_get(akv *s, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen);

/* Removes key. Returns 0 when it existed, -ENOENT otherwise. */
int akv_del(akv *s, const void *key, size_t klen);

/* Sums the counters of all shards. */
void akv_stats_get(akv *s, akv_stats *out);

/* Name of the vector implementation used for tag matching ("sse2", "neon" or "scalar"). */
const char *akv_simd_name(void);

#endif
