/*
 * akv.h - in-memory key-value cache engine.
 *
 * The engine stores byte-string keys and values in a fixed amount of memory and evicts old entries when
 * the memory is full (unless eviction is disabled). It is a cache: an entry may disappear at any time
 * after it was stored, but a lookup never returns a value other than the most recent one stored for the
 * key.
 *
 * Every entry carries 32 client flags (opaque to the engine, as in the memcached protocol), a 64-bit CAS
 * version that changes on every modification, and an optional expiry time. Expired entries are never
 * returned; their memory is reclaimed when a lookup meets them or when the log tail reaches them.
 *
 * Structure:
 *   - The key space is partitioned into shards by hash. Each shard has its own lock, arena and index, so
 *     operations on different shards run in parallel.
 *   - A shard arena is a circular log. New entries are appended at the head; space is reclaimed at the
 *     tail. Overwritten and deleted entries stay in the log as dead space until the tail passes them.
 *   - The index is an open-addressing hash table of 128-byte buckets (16 slots: 16-bit tags and 48-bit
 *     arena offsets). A lookup compares the 16 tags of two adjacent buckets with vector instructions and
 *     verifies the key only for tag matches. The index of a shard starts small and doubles when it is
 *     three quarters full or when a key finds no free slot in its window, up to a bound derived from
 *     the shard size (growth rehashes the shard while holding its lock).
 *   - A store of the same total entry size as the existing entry overwrites it in place.
 *   - Eviction follows the log order with a second chance: an entry that was read since it was written (or
 *     since its last reinsertion) is copied to the head once instead of being evicted.
 *
 * Thread safety: all functions are thread-safe. Operations lock one shard; akv_stats_get, akv_flush and
 * akv_foreach lock the shards one after another.
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
    /*
     * Expected mean entry size in bytes. 0 (default) starts every shard with a small index that grows
     * with the data. A non-zero value sizes the initial index for capacity / avg_entry entries, which
     * avoids growth pauses at the cost of index memory touched by sparse keys.
     */
    uint32_t avg_entry;
    uint64_t hash_seed; /* seed of the key hash; 0 selects a random seed */
    int huge_pages;     /* request transparent huge pages for the arenas (advisory) */
    /* When non-zero, stores that need the space of live entries fail with -ENOMEM instead of evicting. */
    int no_evict;
} akv_config;

typedef struct akv_stats {
    uint64_t entries;     /* live entries (including expired ones not yet reclaimed) */
    uint64_t arena_bytes; /* total arena size */
    uint64_t log_bytes;   /* bytes between tail and head (live + dead entries) */
    uint64_t gets;
    uint64_t hits;
    uint64_t puts;       /* successful stores of any mode, including increments */
    uint64_t overwrites; /* stores that replaced an existing entry */
    uint64_t dels;
    uint64_t evictions;       /* live entries removed to make room */
    uint64_t reinsertions;    /* live entries given a second chance */
    uint64_t index_evictions; /* live entries removed because their index window was full */
    uint64_t rejected;        /* stores refused (entry too large, or no space with no_evict) */
    uint64_t expired;         /* entries removed because their expiry time passed */
    uint64_t index_bytes;     /* memory of the index tables */
    uint64_t index_grows;     /* index doublings */
} akv_stats;

/* Number of fields of struct akv_stats (all uint64_t). */
#define AKV_STATS_FIELDS 15u

/* Store modes (memcached and RESP semantics). */
enum akv_mode {
    AKV_SET = 0,     /* store unconditionally */
    AKV_ADD = 1,     /* store only if the key is absent (-EEXIST otherwise) */
    AKV_REPLACE = 2, /* store only if the key is present (-ENOENT otherwise) */
    AKV_APPEND = 3,  /* append to the existing value, keeping its flags and expiry (-ENOENT if absent) */
    AKV_PREPEND = 4, /* prepend to the existing value, keeping its flags and expiry (-ENOENT if absent) */
    AKV_CAS = 5, /* store only if the entry's CAS equals opts.cas (-EEXIST on mismatch, -ENOENT if absent) */
};

/* Options of akv_store(). Zero-initialized options mean: SET, flags 0, no expiry. */
typedef struct akv_store_opts {
    enum akv_mode mode;
    uint32_t flags; /* client flags stored with the entry */
    /* Time to live in milliseconds: > 0 expires after that time, 0 never expires, < 0 expires at once. */
    int64_t ttl_ms;
    int keep_ttl; /* keep the existing entry's expiry instead of ttl_ms (RESP KEEPTTL) */
    uint64_t cas; /* expected CAS version for AKV_CAS */
} akv_store_opts;

/* Metadata of an entry returned by akv_get_meta(). */
typedef struct akv_meta {
    uint32_t flags;
    uint64_t cas;
    int64_t ttl_ms; /* remaining time to live in milliseconds, -1 when the entry does not expire */
} akv_meta;

/* Value of akv_get_meta()'s touch_ttl_ms that leaves the expiry unchanged. */
#define AKV_TTL_KEEP INT64_MIN

/* Increment semantics of akv_incr(). */
enum akv_incr_mode {
    /* memcached: unsigned 64-bit value, missing key is an error, increments wrap, decrements stop at 0. */
    AKV_INCR_UNSIGNED = 0,
    /* RESP: signed 64-bit value, a missing key counts as 0, overflow is an error. */
    AKV_INCR_SIGNED = 1,
};

/* One entry as seen by akv_foreach(). Pointers are valid during the callback only. */
typedef struct akv_record {
    const void *key;
    size_t klen;
    const void *val;
    size_t vlen;
    uint32_t flags;
    uint64_t cas;
    int64_t ttl_ms; /* remaining time to live, -1 = no expiry */
} akv_record;

/* Fills cfg with defaults: 256 MiB, 64 shards, 1 MiB entries, growing index, eviction enabled. */
void akv_config_default(akv_config *cfg);

/* Creates an engine. Returns NULL on invalid configuration or when memory cannot be allocated. */
akv *akv_create(const akv_config *cfg);

/* Frees an engine. No operation may be in progress. */
void akv_destroy(akv *s);

/*
 * Stores value under key according to opts (NULL = SET without flags and expiry). On success the new
 * CAS version is written to *cas_out when it is not NULL.
 * Returns 0, -EINVAL (empty key or key longer than 65535 bytes), -E2BIG (key + value larger than
 * max_entry or than half a shard arena), -EEXIST / -ENOENT (condition of the mode not met), or -ENOMEM
 * (no_evict and no free space).
 */
int akv_store(akv *s, const void *key, size_t klen, const void *val, size_t vlen, const akv_store_opts *opts,
              uint64_t *cas_out);

/* Stores value under key unconditionally, without flags or expiry. Same as akv_store(..., NULL, NULL). */
int akv_put(akv *s, const void *key, size_t klen, const void *val, size_t vlen);

/*
 * Copies the value of key into buf (capacity cap) and stores its length in *vlen and, when meta is not
 * NULL, its metadata in *meta. touch_ttl_ms other than AKV_TTL_KEEP sets a new expiry as in akv_touch()
 * (memcached gat). Returns 0 on a hit, -ENOENT when the key is absent or expired, -EINVAL for an invalid
 * key, or -EMSGSIZE when the value is larger than cap (*vlen and *meta are then set and buf is untouched).
 */
int akv_get_meta(akv *s, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen, akv_meta *meta,
                 int64_t touch_ttl_ms);

/* akv_get_meta() without metadata and touch. */
int akv_get(akv *s, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen);

/* Returns 1 when key is present and not expired, 0 otherwise. Does not count as a read for eviction. */
int akv_exists(akv *s, const void *key, size_t klen);

/*
 * Cache hint: starts loading the index bucket of key, without a lock and without changing anything, so
 * that the lookups of a batch of requests overlap their memory latency (call it for every key of the
 * batch, then perform the operations). Thread-safe.
 */
void akv_prefetch(akv *s, const void *key, size_t klen);

/* Second stage of akv_prefetch(): starts loading the entry of key (takes the shard lock briefly). */
void akv_prefetch_entry(akv *s, const void *key, size_t klen);

/* Removes key. Returns 0 when it existed, -ENOENT otherwise. */
int akv_del(akv *s, const void *key, size_t klen);

/*
 * Sets the expiry of key: ttl_ms > 0 expires after that time, 0 removes the expiry, < 0 deletes the key.
 * Returns 0, or -ENOENT when the key is absent or expired.
 */
int akv_touch(akv *s, const void *key, size_t klen, int64_t ttl_ms);

/*
 * Adds delta to the decimal integer stored under key (see enum akv_incr_mode) and stores the result as
 * decimal text, keeping flags and expiry. The new value is written to *result. Returns 0, -ENOENT
 * (missing key, unsigned mode), -EINVAL (value is not a decimal integer of the mode), -ERANGE (signed
 * overflow), or the errors of akv_store().
 */
int akv_incr(akv *s, const void *key, size_t klen, int64_t delta, enum akv_incr_mode mode, int64_t *result,
             uint64_t *cas_out);

/* Removes all entries and returns the memory of the arenas to the system. */
void akv_flush(akv *s);

/*
 * Calls fn for every live, unexpired entry, shard by shard, in log order within a shard. Each shard is
 * copied while its lock is held and fn runs without any lock, so fn may call engine functions. Returns
 * 0, the first non-zero value returned by fn, or -ENOMEM.
 */
int akv_foreach(akv *s, int (*fn)(void *arg, const akv_record *r), void *arg);

/* Sums the counters of all shards. */
void akv_stats_get(akv *s, akv_stats *out);

/* Name of the vector implementation used for tag matching ("sse2", "neon" or "scalar"). */
const char *akv_simd_name(void);

#endif
