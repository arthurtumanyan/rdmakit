/*
 * akv.c - key-value cache engine, see akv.h.
 *
 * Arena entry layout (all entries start at multiples of 8 bytes):
 *
 *   offset size field
 *   0      4    total    entry size in bytes including this header, multiple of 8
 *   4      1    flags    ENTRY_ACCESSED, ENTRY_PAD
 *   5      1    reserved
 *   6      2    klen     key length
 *   8      4    vlen     value length
 *   12     4    uflags   client flags
 *   16     8    hash     64-bit key hash
 *   24     8    cas      CAS version
 *   32     8    expiry   expiry time in milliseconds of the engine clock, 0 = none
 *   40     k    key
 *   40+k   v    value, followed by padding to a multiple of 8
 *
 * A PAD entry fills the unusable space at the end of the arena when the next entry does not fit before
 * the wrap-around. Only `total` and `flags` (the first 5 bytes) are written for it, because the gap can
 * be as small as 8 bytes.
 *
 * Index slot: 16-bit tag (never 0 for an occupied slot) and 48-bit value (arena offset / 8 + 1).
 *
 * Engine clock: milliseconds of CLOCK_MONOTONIC_COARSE since the engine was created, plus 1, so that 0 can
 * mean "no expiry". Expiry is therefore unaffected by changes of the wall clock. The coarse clock is read
 * from the vDSO without the timestamp counter (clock_gettime(2)); its resolution is one scheduler tick
 * (clock_getres: 1 to 10 ms), so an entry can outlive its time to live by up to one tick. memcached checks
 * expiry against a clock with a resolution of one second (memcached.c, current_time).
 *
 * CAS versions: per shard counter shifted left by 10 bits, ORed with the shard index (at most 1024
 * shards), so versions are unique within the engine without a shared counter.
 */
#include "akv.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"
#include "simd.h"

#define ENTRY_HDR 40u
#define ENTRY_ACCESSED 1u
#define ENTRY_PAD 2u
#define SLOTS 16u
#define WINDOW 2u /* buckets examined per lookup */
#define MAX_REINSERT_STREAK 64u
#define INITIAL_BUCKETS 16u
#define NO_POS UINT64_MAX

struct entry_hdr {
    uint32_t total;
    uint8_t flags;
    uint8_t reserved8;
    uint16_t klen;
    uint32_t vlen;
    uint32_t uflags;
    uint64_t hash;
    uint64_t cas;
    uint64_t expiry;
};
_Static_assert(sizeof(struct entry_hdr) == ENTRY_HDR, "entry header size");

struct bucket {
    uint16_t tags[SLOTS];
    uint8_t pos[SLOTS][6];
};
_Static_assert(sizeof(struct bucket) == 128, "bucket size");

struct shard {
    pthread_mutex_t lock;
    uint8_t *arena;
    uint64_t size; /* arena size in bytes, multiple of 8 */
    uint64_t head; /* logical (monotonic) append position */
    uint64_t tail; /* logical position of the oldest entry */
    struct bucket *buckets;
    uint64_t nbuckets;
    uint64_t mask;        /* bucket count - 1 */
    uint64_t max_buckets; /* growth bound */
    uint8_t *scratch;     /* holds an entry while it is reinserted */
    uint64_t scratch_size;
    int reinserting; /* set while scratch is in use: nested evictions must not reinsert */
    uint32_t index;  /* shard number, low bits of CAS versions */
    uint64_t cas_seq;
    akv_stats st;
    char pad[64]; /* keeps the locks of neighboring shards on different cache lines */
};

struct akv {
    akv_config cfg;
    struct shard *shards;
    uint32_t nshards;
    uint64_t seed;
    struct timespec epoch;
};

const char *akv_simd_name(void) {
    return AKV_SIMD_NAME;
}

void akv_config_default(akv_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->capacity = 256ull << 20;
    cfg->shards = 64;
    cfg->max_entry = 1u << 20;
}

/* Current engine time in milliseconds (>= 1). */
static uint64_t now_ms(const akv *s) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    int64_t ms = (int64_t)(ts.tv_sec - s->epoch.tv_sec) * 1000 + (ts.tv_nsec - s->epoch.tv_nsec) / 1000000;
    return (uint64_t)(ms < 0 ? 0 : ms) + 1;
}

/* Expiry time for a time to live (> 0), relative to now. */
static uint64_t expiry_of(uint64_t now, int64_t ttl_ms) {
    return ttl_ms > 0 ? now + (uint64_t)ttl_ms : 0;
}

static int64_t ttl_left(uint64_t expiry, uint64_t now) {
    if (expiry == 0) {
        return -1;
    }
    return expiry > now ? (int64_t)(expiry - now) : 0;
}

/* ---- index helpers ------------------------------------------------------------------------------- */

static inline uint64_t slot_get(const struct bucket *b, unsigned i) {
    const uint8_t *p = b->pos[i];
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40);
}

static inline void slot_set(struct bucket *b, unsigned i, uint16_t tag, uint64_t value) {
    uint8_t *p = b->pos[i];
    for (unsigned k = 0; k < 6; k++) {
        p[k] = (uint8_t)(value >> (8 * k));
    }
    b->tags[i] = tag;
}

static inline uint16_t tag_of(uint64_t h) {
    uint16_t t = (uint16_t)(h >> 48);
    return t != 0 ? t : 1;
}

static inline struct bucket *bucket_in(struct bucket *tab, uint64_t mask, uint64_t h, unsigned n) {
    return &tab[(h + n) & mask];
}

static inline struct bucket *bucket_at(struct shard *sh, uint64_t h, unsigned n) {
    return bucket_in(sh->buckets, sh->mask, h, n);
}

static inline struct entry_hdr *entry_at(const struct shard *sh, uint64_t phys) {
    return (struct entry_hdr *)(sh->arena + phys);
}

static inline uint8_t *entry_key(struct entry_hdr *e) {
    return (uint8_t *)e + ENTRY_HDR;
}

static inline uint8_t *entry_val(struct entry_hdr *e) {
    return (uint8_t *)e + ENTRY_HDR + e->klen;
}

/* Finds the slot referring to the entry at physical offset phys with the given hash; NULL when absent. */
static struct bucket *find_by_pos(struct shard *sh, uint64_t h, uint64_t phys, unsigned *slot) {
    uint16_t tag = tag_of(h);
    uint64_t want = phys / 8 + 1;
    for (unsigned n = 0; n < WINDOW; n++) {
        struct bucket *b = bucket_at(sh, h, n);
        uint32_t m = akv_match16(b->tags, tag);
        while (m != 0) {
            unsigned i = (unsigned)__builtin_ctz(m);
            m &= m - 1;
            if (slot_get(b, i) == want) {
                *slot = i;
                return b;
            }
        }
    }
    return NULL;
}

/* Finds the slot of key (hash h); NULL when absent. Expired entries are found too. */
static struct bucket *find_key(struct shard *sh, uint64_t h, const void *key, size_t klen, unsigned *slot) {
    uint16_t tag = tag_of(h);
    for (unsigned n = 0; n < WINDOW; n++) {
        struct bucket *b = bucket_at(sh, h, n);
        uint32_t m = akv_match16(b->tags, tag);
        while (m != 0) {
            unsigned i = (unsigned)__builtin_ctz(m);
            m &= m - 1;
            struct entry_hdr *e = entry_at(sh, (slot_get(b, i) - 1) * 8);
            if (e->hash == h && e->klen == klen && memcmp(entry_key(e), key, klen) == 0) {
                *slot = i;
                return b;
            }
        }
    }
    return NULL;
}

/* Finds the entry of key and removes it from the index when it has expired. NULL when absent. */
static struct entry_hdr *find_live(struct shard *sh, uint64_t h, const void *key, size_t klen, uint64_t now,
                                   struct bucket **bp, unsigned *slot) {
    struct bucket *b = find_key(sh, h, key, klen, slot);
    if (b == NULL) {
        return NULL;
    }
    struct entry_hdr *e = entry_at(sh, (slot_get(b, *slot) - 1) * 8);
    if (e->expiry != 0 && e->expiry <= now) {
        b->tags[*slot] = 0; /* the entry becomes dead log space */
        sh->st.expired++;
        sh->st.entries--;
        return NULL;
    }
    *bp = b;
    return e;
}

/* Places (tag, value) into a free slot of the window of h in table tab. Returns 0, or -1 when full. */
static int window_put(struct bucket *tab, uint64_t mask, uint64_t h, uint64_t value) {
    for (unsigned n = 0; n < WINDOW; n++) {
        struct bucket *b = bucket_in(tab, mask, h, n);
        for (unsigned i = 0; i < SLOTS; i++) {
            if (b->tags[i] == 0) {
                slot_set(b, i, tag_of(h), value);
                return 0;
            }
        }
    }
    return -1;
}

static struct bucket *table_alloc(uint64_t nbuckets) {
    /* Anonymous mappings are zero-filled and committed lazily. */
    void *p = mmap(NULL, nbuckets * sizeof(struct bucket), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/*
 * Doubles the index of a shard and rehashes every slot. Entries that find no slot in the new table (a
 * window of 32 slots already full, which is rare at the post-growth load) are dropped from the index.
 * Returns 0, or -1 when the bound is reached or memory is exhausted.
 */
static int index_grow(struct shard *sh) {
    uint64_t n = sh->nbuckets * 2;
    if (n > sh->max_buckets) {
        return -1;
    }
    struct bucket *tab = table_alloc(n);
    if (tab == NULL) {
        return -1;
    }
    uint64_t mask = n - 1;
    for (uint64_t b = 0; b < sh->nbuckets; b++) {
        struct bucket *ob = &sh->buckets[b];
        for (unsigned i = 0; i < SLOTS; i++) {
            if (ob->tags[i] == 0) {
                continue;
            }
            uint64_t v = slot_get(ob, i);
            const struct entry_hdr *e = entry_at(sh, (v - 1) * 8);
            if (window_put(tab, mask, e->hash, v) != 0) {
                sh->st.index_evictions++;
                sh->st.entries--;
            }
        }
    }
    munmap(sh->buckets, sh->nbuckets * sizeof(struct bucket));
    /* Atomic stores: akv_prefetch() reads both without the lock. */
    __atomic_store_n(&sh->buckets, tab, __ATOMIC_RELAXED);
    sh->nbuckets = n;
    __atomic_store_n(&sh->mask, mask, __ATOMIC_RELAXED);
    sh->st.index_grows++;
    return 0;
}

/*
 * Inserts a new index slot for an entry. Grows the index at 75% load or when the window is full; at the
 * growth bound a full window drops its oldest live entry (in log order), unless eviction is disabled.
 * Returns 0, or -ENOMEM (no_evict and no slot).
 */
static int index_insert(struct shard *sh, int no_evict, uint64_t h, uint64_t value) {
    if (sh->st.entries + 1 > sh->nbuckets * SLOTS * 3 / 4) {
        index_grow(sh);
    }
    while (window_put(sh->buckets, sh->mask, h, value) != 0) {
        if (index_grow(sh) == 0) {
            continue;
        }
        if (no_evict) {
            return -ENOMEM;
        }
        /* Window full at the bound: drop the oldest live entry of the window. */
        struct bucket *fb = NULL;
        unsigned fslot = 0;
        uint64_t best_age = UINT64_MAX;
        for (unsigned n = 0; n < WINDOW; n++) {
            struct bucket *cand = bucket_at(sh, h, n);
            for (unsigned i = 0; i < SLOTS; i++) {
                uint64_t p = (slot_get(cand, i) - 1) * 8;
                uint64_t age = (p + sh->size - sh->tail % sh->size) % sh->size;
                if (age < best_age) {
                    best_age = age;
                    fb = cand;
                    fslot = i;
                }
            }
        }
        fb->tags[fslot] = 0;
        sh->st.index_evictions++;
        sh->st.entries--;
    }
    sh->st.entries++;
    return 0;
}

/* ---- arena ----------------------------------------------------------------------------------------- */

static inline uint64_t entry_total(size_t klen, size_t vlen) {
    return (ENTRY_HDR + klen + vlen + 7) & ~(uint64_t)7;
}

/*
 * Frees the oldest log entry: skips dead entries, reclaims expired ones, gives a recently read live entry
 * a second chance. Returns 0, or -1 when the oldest entry is live and eviction is disabled.
 */
static int evict_one(akv *s, struct shard *sh, unsigned *streak, uint64_t now);

/*
 * Appends an entry of `total` bytes at the head, evicting from the tail as needed, and returns its
 * physical offset, or NO_POS when eviction is disabled and the space is held by live entries.
 * The caller fills the entry.
 */
static uint64_t arena_alloc(akv *s, struct shard *sh, uint64_t total, uint64_t now) {
    unsigned streak = 0;
    uint64_t pos = sh->head % sh->size;
    uint64_t pad = pos + total > sh->size ? sh->size - pos : 0;
    while (sh->size - (sh->head - sh->tail) < pad + total) {
        if (evict_one(s, sh, &streak, now) != 0) {
            return NO_POS;
        }
        pos = sh->head % sh->size;
        pad = pos + total > sh->size ? sh->size - pos : 0;
    }
    if (pad != 0) {
        struct entry_hdr *p = entry_at(sh, pos);
        p->total = (uint32_t)pad;
        p->flags = ENTRY_PAD;
        sh->head += pad;
    }
    uint64_t phys = sh->head % sh->size;
    sh->head += total;
    return phys;
}

static int evict_one(akv *s, struct shard *sh, unsigned *streak, uint64_t now) {
    uint64_t phys = sh->tail % sh->size;
    struct entry_hdr *e = entry_at(sh, phys);
    uint32_t total = e->total;
    if (e->flags & ENTRY_PAD) {
        sh->tail += total;
        return 0;
    }
    unsigned slot;
    struct bucket *b = find_by_pos(sh, e->hash, phys, &slot);
    if (b == NULL) { /* dead: overwritten, deleted or expired earlier */
        sh->tail += total;
        return 0;
    }
    if (e->expiry != 0 && e->expiry <= now) {
        b->tags[slot] = 0;
        sh->tail += total;
        sh->st.expired++;
        sh->st.entries--;
        return 0;
    }
    if (s->cfg.no_evict) {
        return -1;
    }
    if ((e->flags & ENTRY_ACCESSED) && !sh->reinserting && *streak < MAX_REINSERT_STREAK) {
        /* Second chance: copy to the head with the accessed bit cleared. The old copy is released first. */
        memcpy(sh->scratch, e, total);
        uint64_t hash = e->hash;
        b->tags[slot] = 0;
        sh->tail += total;
        sh->reinserting = 1;
        uint64_t np = arena_alloc(s, sh, total, now);
        sh->reinserting = 0;
        memcpy(entry_at(sh, np), sh->scratch, total);
        entry_at(sh, np)->flags = 0;
        /* Re-register the entry: the slot may have been cleared by nested evictions, so insert afresh. */
        if (window_put(sh->buckets, sh->mask, hash, np / 8 + 1) == 0) {
            sh->st.reinsertions++;
        } else {
            sh->st.index_evictions++;
            sh->st.entries--;
        }
        (*streak)++;
        return 0;
    }
    b->tags[slot] = 0;
    sh->tail += total;
    sh->st.evictions++;
    sh->st.entries--;
    return 0;
}

static void write_hdr(struct entry_hdr *e, uint64_t h, size_t klen, size_t vlen, uint64_t total,
                      uint32_t uflags, uint64_t cas, uint64_t expiry) {
    e->total = (uint32_t)total;
    e->flags = 0;
    e->reserved8 = 0;
    e->klen = (uint16_t)klen;
    e->vlen = (uint32_t)vlen;
    e->uflags = uflags;
    e->hash = h;
    e->cas = cas;
    e->expiry = expiry;
}

/* ---- lifecycle ------------------------------------------------------------------------------------- */

static uint64_t next_pow2(uint64_t v) {
    uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

static void shard_destroy(struct shard *sh) {
    if (sh->arena != NULL) {
        munmap(sh->arena, sh->size);
    }
    if (sh->buckets != NULL) {
        munmap(sh->buckets, sh->nbuckets * sizeof(struct bucket));
    }
    free(sh->scratch);
    pthread_mutex_destroy(&sh->lock);
}

akv *akv_create(const akv_config *in) {
    akv_config cfg = *in;
    if (cfg.shards == 0) {
        cfg.shards = 64;
    }
    if (cfg.max_entry == 0) {
        cfg.max_entry = 1u << 20;
    }
    if (cfg.shards > 1024 || (cfg.shards & (cfg.shards - 1)) != 0 ||
        cfg.capacity < (uint64_t)cfg.shards * 4096) {
        return NULL;
    }
    akv *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->cfg = cfg;
    s->nshards = cfg.shards;
    s->seed = cfg.hash_seed;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &s->epoch);
    if (s->seed == 0) {
        uint64_t seed = 0x243f6a8885a308d3ull ^ (uint64_t)(uintptr_t)s ^ ((uint64_t)getpid() << 32);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        s->seed = akv_mulfold(seed ^ (uint64_t)ts.tv_nsec, 0x9e3779b97f4a7c15ull) | 1;
    }
    s->shards = calloc(cfg.shards, sizeof(struct shard));
    if (s->shards == NULL) {
        free(s);
        return NULL;
    }
    uint64_t per_shard = (cfg.capacity / cfg.shards) & ~(uint64_t)7;
    /* Bound: one slot per smallest possible entry (header plus one key byte, rounded) at 75% load. */
    uint64_t max_slots = per_shard / entry_total(1, 0) + 1;
    uint64_t max_buckets = next_pow2(max_slots * 4 / (SLOTS * 3) + 1);
    uint64_t nbuckets = INITIAL_BUCKETS;
    if (cfg.avg_entry != 0) {
        uint64_t slots_needed = per_shard / cfg.avg_entry + 1;
        nbuckets = next_pow2(slots_needed * 4 / (SLOTS * 3) + 1);
    }
    if (nbuckets < 2) {
        nbuckets = 2;
    }
    if (max_buckets < nbuckets) {
        max_buckets = nbuckets;
    }
    for (uint32_t i = 0; i < cfg.shards; i++) {
        struct shard *sh = &s->shards[i];
        pthread_mutex_init(&sh->lock, NULL);
        sh->index = i;
        sh->size = per_shard;
        sh->arena = mmap(NULL, sh->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sh->arena == MAP_FAILED) {
            sh->arena = NULL;
            akv_destroy(s);
            return NULL;
        }
        if (cfg.huge_pages) {
            madvise(sh->arena, sh->size, MADV_HUGEPAGE);
        }
        sh->buckets = table_alloc(nbuckets);
        if (sh->buckets == NULL) {
            akv_destroy(s);
            return NULL;
        }
        sh->nbuckets = nbuckets;
        sh->mask = nbuckets - 1;
        sh->max_buckets = max_buckets;
        uint64_t biggest = entry_total(0xffff, cfg.max_entry);
        sh->scratch_size = biggest < per_shard / 2 ? biggest : per_shard / 2;
        sh->scratch = malloc(sh->scratch_size);
        if (sh->scratch == NULL) {
            akv_destroy(s);
            return NULL;
        }
    }
    return s;
}

void akv_destroy(akv *s) {
    if (s == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->nshards; i++) {
        shard_destroy(&s->shards[i]);
    }
    free(s->shards);
    free(s);
}

/* ---- operations ------------------------------------------------------------------------------------ */

static inline struct shard *shard_of(akv *s, uint64_t h) {
    return &s->shards[(h >> 32) & (s->nshards - 1)];
}

/*
 * Prefetches the first index bucket of the key (both cache lines of the 128-byte bucket). Reads the table
 * pointer and mask without the lock: during a concurrent index growth they may belong to different
 * tables, and the address may then lie outside the current table. A prefetch of any address neither
 * faults nor changes memory (Intel SDM vol. 2, PREFETCHh), so the hint is harmless.
 */
void akv_prefetch(akv *s, const void *key, size_t klen) {
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    struct bucket *tab = __atomic_load_n(&sh->buckets, __ATOMIC_RELAXED);
    uint64_t mask = __atomic_load_n(&sh->mask, __ATOMIC_RELAXED);
    const char *b = (const char *)(tab + (h & mask));
    __builtin_prefetch(b, 0, 3);
    __builtin_prefetch(b + 64, 0, 3);
}

/*
 * Second prefetch stage: finds the entry of the key under the shard lock (trylock: a contended shard is
 * skipped) and prefetches its header and key. Called after akv_prefetch() for every key of a group, when
 * the buckets are expected to be in the cache.
 */
void akv_prefetch_entry(akv *s, const void *key, size_t klen) {
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    if (pthread_mutex_trylock(&sh->lock) != 0) {
        return;
    }
    uint16_t tag = tag_of(h);
    struct bucket *b = bucket_at(sh, h, 0);
    uint32_t m = akv_match16(b->tags, tag);
    if (m != 0) {
        __builtin_prefetch(entry_at(sh, (slot_get(b, (unsigned)__builtin_ctz(m)) - 1) * 8), 0, 3);
    }
    pthread_mutex_unlock(&sh->lock);
}

static inline uint64_t next_cas(struct shard *sh) {
    return (++sh->cas_seq << 10) | sh->index;
}

/* Checks the size limits of an entry. Returns 0 or -E2BIG. */
static int size_ok(akv *s, struct shard *sh, size_t klen, size_t vlen) {
    uint64_t total = entry_total(klen, vlen);
    if (klen + vlen > s->cfg.max_entry || total > sh->size / 2 || total > sh->scratch_size) {
        return -E2BIG;
    }
    return 0;
}

/*
 * Writes a new version of key with the shard locked. `old` is the current live entry (or NULL) found by
 * the caller; parts[] are concatenated to form the value. Returns 0, -E2BIG or -ENOMEM.
 */
static int store_locked(akv *s, struct shard *sh, uint64_t h, const void *key, size_t klen,
                        struct entry_hdr *old, const void *const parts[2], const size_t plen[2],
                        uint32_t uflags, uint64_t expiry, uint64_t now, uint64_t *cas_out) {
    size_t vlen = plen[0] + plen[1];
    if (size_ok(s, sh, klen, vlen) != 0) {
        sh->st.rejected++;
        return -E2BIG;
    }
    uint64_t total = entry_total(klen, vlen);
    uint64_t cas = next_cas(sh);
    if (old != NULL && old->total == total) {
        /* Same size: overwrite in place; no log space is consumed and no dead entry is left. */
        uint8_t *v = entry_val(old);
        if (parts[0] != v) {
            memmove(v, parts[0], plen[0]);
        }
        if (plen[1] > 0) {
            memmove(v + plen[0], parts[1], plen[1]);
        }
        old->vlen = (uint32_t)vlen;
        old->uflags = uflags;
        old->cas = cas;
        old->expiry = expiry;
        sh->st.overwrites++;
        sh->st.puts++;
        if (cas_out != NULL) {
            *cas_out = cas;
        }
        return 0;
    }
    uint64_t phys = arena_alloc(s, sh, total, now);
    if (phys == NO_POS) {
        sh->st.rejected++;
        return -ENOMEM;
    }
    struct entry_hdr *e = entry_at(sh, phys);
    write_hdr(e, h, klen, vlen, total, uflags, cas, expiry);
    memcpy(entry_key(e), key, klen);
    /* parts may point into scratch copies made by the caller; the old entry may have been evicted. */
    if (plen[0] > 0) {
        memcpy(entry_val(e), parts[0], plen[0]);
    }
    if (plen[1] > 0) {
        memcpy(entry_val(e) + plen[0], parts[1], plen[1]);
    }
    uint64_t pos_value = phys / 8 + 1;
    unsigned slot;
    struct bucket *b =
        find_key(sh, h, key, klen, &slot); /* the allocation may have evicted the old version */
    if (b != NULL && slot_get(b, slot) != pos_value) {
        slot_set(b, slot, tag_of(h), pos_value); /* replace: the previous version is now dead log space */
        sh->st.overwrites++;
    } else if (b == NULL) {
        if (index_insert(sh, s->cfg.no_evict, h, pos_value) != 0) {
            /* The entry stays as dead log space. */
            sh->st.rejected++;
            return -ENOMEM;
        }
    }
    sh->st.puts++;
    if (cas_out != NULL) {
        *cas_out = cas;
    }
    return 0;
}

int akv_store(akv *s, const void *key, size_t klen, const void *val, size_t vlen, const akv_store_opts *opts,
              uint64_t *cas_out) {
    static const akv_store_opts defaults = {0};
    if (opts == NULL) {
        opts = &defaults;
    }
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    uint64_t now = now_ms(s);
    struct bucket *b = NULL;
    unsigned slot = 0;
    struct entry_hdr *old = find_live(sh, h, key, klen, now, &b, &slot);
    int rc = 0;
    switch (opts->mode) {
    case AKV_SET:
        break;
    case AKV_ADD:
        rc = old != NULL ? -EEXIST : 0;
        break;
    case AKV_REPLACE:
    case AKV_APPEND:
    case AKV_PREPEND:
        rc = old == NULL ? -ENOENT : 0;
        break;
    case AKV_CAS:
        rc = old == NULL ? -ENOENT : (old->cas != opts->cas ? -EEXIST : 0);
        break;
    default:
        rc = -EINVAL;
        break;
    }
    if (rc != 0) {
        pthread_mutex_unlock(&sh->lock);
        return rc;
    }
    if (opts->ttl_ms < 0 && opts->mode != AKV_APPEND && opts->mode != AKV_PREPEND) {
        /* Stored and expired at once: the key no longer exists. */
        if (old != NULL) {
            b->tags[slot] = 0;
            sh->st.entries--;
            sh->st.expired++;
        }
        sh->st.puts++;
        pthread_mutex_unlock(&sh->lock);
        if (cas_out != NULL) {
            *cas_out = 0;
        }
        return 0;
    }
    uint32_t uflags = opts->flags;
    uint64_t expiry = opts->keep_ttl && old != NULL ? old->expiry : expiry_of(now, opts->ttl_ms);
    const void *parts[2] = {val, NULL};
    size_t plen[2] = {vlen, 0};
    uint8_t *tmp = NULL;
    if (opts->mode == AKV_APPEND || opts->mode == AKV_PREPEND) {
        uflags = old->uflags;
        expiry = old->expiry;
        if (size_ok(s, sh, klen, (size_t)old->vlen + vlen) != 0) {
            sh->st.rejected++;
            pthread_mutex_unlock(&sh->lock);
            return -E2BIG;
        }
        /* Copy the old value: allocating the new version may evict or move the old one. */
        tmp = malloc(old->vlen > 0 ? old->vlen : 1);
        if (tmp == NULL) {
            pthread_mutex_unlock(&sh->lock);
            return -ENOMEM;
        }
        memcpy(tmp, entry_val(old), old->vlen);
        size_t olen = old->vlen;
        if (opts->mode == AKV_APPEND) {
            parts[0] = tmp;
            plen[0] = olen;
            parts[1] = val;
            plen[1] = vlen;
        } else {
            parts[0] = val;
            plen[0] = vlen;
            parts[1] = tmp;
            plen[1] = olen;
        }
        if (old->total != entry_total(klen, olen + vlen)) {
            old = NULL; /* not an in-place update; store_locked must not write into the old entry */
        } else if (opts->mode == AKV_PREPEND) {
            old = NULL; /* in-place prepend would overwrite the source; append the new version instead */
        }
    }
    rc = store_locked(s, sh, h, key, klen, old, parts, plen, uflags, expiry, now, cas_out);
    pthread_mutex_unlock(&sh->lock);
    free(tmp);
    return rc;
}

int akv_put(akv *s, const void *key, size_t klen, const void *val, size_t vlen) {
    return akv_store(s, key, klen, val, vlen, NULL, NULL);
}

int akv_get_meta(akv *s, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen, akv_meta *meta,
                 int64_t touch_ttl_ms) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    sh->st.gets++;
    uint64_t now = now_ms(s);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(sh, h, key, klen, now, &b, &slot);
    int rc;
    if (e == NULL) {
        rc = -ENOENT;
    } else {
        if (touch_ttl_ms != AKV_TTL_KEEP) {
            if (touch_ttl_ms < 0) {
                e->expiry = now; /* expired: reclaimed by the next lookup or by eviction */
            } else {
                e->expiry = expiry_of(now, touch_ttl_ms);
            }
        }
        *vlen = e->vlen;
        if (meta != NULL) {
            meta->flags = e->uflags;
            meta->cas = e->cas;
            meta->ttl_ms = ttl_left(e->expiry, now);
        }
        if (e->vlen > cap) {
            rc = -EMSGSIZE;
        } else {
            memcpy(buf, entry_val(e), e->vlen);
            e->flags |= ENTRY_ACCESSED;
            sh->st.hits++;
            rc = 0;
        }
    }
    pthread_mutex_unlock(&sh->lock);
    return rc;
}

int akv_get(akv *s, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    return akv_get_meta(s, key, klen, buf, cap, vlen, NULL, AKV_TTL_KEEP);
}

int akv_exists(akv *s, const void *key, size_t klen) {
    if (klen == 0 || klen > 0xffff) {
        return 0;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    struct bucket *b;
    unsigned slot;
    int found = find_live(sh, h, key, klen, now_ms(s), &b, &slot) != NULL;
    pthread_mutex_unlock(&sh->lock);
    return found;
}

int akv_del(akv *s, const void *key, size_t klen) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    struct bucket *b;
    unsigned slot;
    int rc = -ENOENT;
    if (find_live(sh, h, key, klen, now_ms(s), &b, &slot) != NULL) {
        b->tags[slot] = 0;
        sh->st.dels++;
        sh->st.entries--;
        rc = 0;
    }
    pthread_mutex_unlock(&sh->lock);
    return rc;
}

int akv_touch(akv *s, const void *key, size_t klen, int64_t ttl_ms) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    uint64_t now = now_ms(s);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(sh, h, key, klen, now, &b, &slot);
    int rc = -ENOENT;
    if (e != NULL) {
        if (ttl_ms < 0) {
            b->tags[slot] = 0;
            sh->st.entries--;
            sh->st.expired++;
        } else {
            e->expiry = expiry_of(now, ttl_ms);
            e->cas = next_cas(sh);
        }
        rc = 0;
    }
    pthread_mutex_unlock(&sh->lock);
    return rc;
}

/* Parses a decimal integer of at most 20 digits (optional '-' when signed). Returns 0 or -EINVAL. */
static int parse_int(const uint8_t *p, size_t n, int is_signed, int64_t *sv, uint64_t *uv) {
    if (n == 0 || n > 20) {
        return -EINVAL;
    }
    int neg = 0;
    size_t i = 0;
    if (is_signed && p[0] == '-') {
        neg = 1;
        i = 1;
        if (n == 1) {
            return -EINVAL;
        }
    }
    uint64_t v = 0;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return -EINVAL;
        }
        uint64_t d = (uint64_t)(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return -EINVAL;
        }
        v = v * 10 + d;
    }
    if (is_signed) {
        if ((!neg && v > (uint64_t)INT64_MAX) || (neg && v > (uint64_t)INT64_MAX + 1)) {
            return -EINVAL;
        }
        *sv = neg ? (int64_t)(0 - v) : (int64_t)v;
    } else {
        *uv = v;
    }
    return 0;
}

int akv_incr(akv *s, const void *key, size_t klen, int64_t delta, enum akv_incr_mode mode, int64_t *result,
             uint64_t *cas_out) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    uint64_t now = now_ms(s);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(sh, h, key, klen, now, &b, &slot);
    char text[24];
    size_t tlen = 0;
    int rc = 0;
    if (mode == AKV_INCR_UNSIGNED) {
        uint64_t v = 0;
        if (e == NULL) {
            rc = -ENOENT;
        } else {
            rc = parse_int(entry_val(e), e->vlen, 0, NULL, &v);
        }
        if (rc == 0) {
            if (delta >= 0) {
                v += (uint64_t)delta; /* wraps modulo 2^64, as memcached does */
            } else {
                uint64_t d = (uint64_t)0 - (uint64_t)delta;
                v = v > d ? v - d : 0;
            }
            *result = (int64_t)v;
            tlen = (size_t)snprintf(text, sizeof(text), "%llu", (unsigned long long)v);
        }
    } else {
        int64_t v = 0;
        if (e != NULL) {
            rc = parse_int(entry_val(e), e->vlen, 1, &v, NULL);
        }
        if (rc == 0) {
            if (__builtin_add_overflow(v, delta, &v)) {
                rc = -ERANGE;
            } else {
                *result = v;
                tlen = (size_t)snprintf(text, sizeof(text), "%lld", (long long)v);
            }
        }
    }
    if (rc == 0) {
        const void *parts[2] = {text, NULL};
        size_t plen[2] = {tlen, 0};
        uint32_t uflags = e != NULL ? e->uflags : 0;
        uint64_t expiry = e != NULL ? e->expiry : 0;
        rc = store_locked(s, sh, h, key, klen, e, parts, plen, uflags, expiry, now, cas_out);
    }
    pthread_mutex_unlock(&sh->lock);
    return rc;
}

void akv_flush(akv *s) {
    for (uint32_t i = 0; i < s->nshards; i++) {
        struct shard *sh = &s->shards[i];
        pthread_mutex_lock(&sh->lock);
        madvise(sh->arena, sh->size, MADV_DONTNEED); /* private anonymous memory reads back as zero */
        madvise(sh->buckets, sh->nbuckets * sizeof(struct bucket), MADV_DONTNEED);
        sh->head = 0;
        sh->tail = 0;
        sh->st.entries = 0;
        pthread_mutex_unlock(&sh->lock);
    }
}

int akv_foreach(akv *s, int (*fn)(void *arg, const akv_record *r), void *arg) {
    uint8_t *copy = NULL;
    size_t copy_cap = 0;
    int rc = 0;
    for (uint32_t i = 0; i < s->nshards && rc == 0; i++) {
        struct shard *sh = &s->shards[i];
        pthread_mutex_lock(&sh->lock);
        uint64_t now = now_ms(s);
        /* Copy live entries in log order; ttl is stored in the expiry field as the remaining time + 1. */
        size_t need = (size_t)(sh->head - sh->tail);
        if (need > copy_cap) {
            uint8_t *p = realloc(copy, need);
            if (p == NULL) {
                pthread_mutex_unlock(&sh->lock);
                rc = -ENOMEM;
                break;
            }
            copy = p;
            copy_cap = need;
        }
        size_t used = 0;
        for (uint64_t pos = sh->tail; pos < sh->head;) {
            uint64_t phys = pos % sh->size;
            struct entry_hdr *e = entry_at(sh, phys);
            uint32_t total = e->total;
            pos += total;
            if (e->flags & ENTRY_PAD) {
                continue;
            }
            unsigned slot;
            if (find_by_pos(sh, e->hash, phys, &slot) == NULL || (e->expiry != 0 && e->expiry <= now)) {
                continue;
            }
            memcpy(copy + used, e, total);
            struct entry_hdr *c = (struct entry_hdr *)(copy + used);
            c->expiry = e->expiry == 0 ? 0 : (uint64_t)ttl_left(e->expiry, now) + 1;
            used += total;
        }
        pthread_mutex_unlock(&sh->lock);
        for (size_t off = 0; off < used && rc == 0;) {
            struct entry_hdr *c = (struct entry_hdr *)(copy + off);
            off += c->total;
            akv_record r = {
                .key = entry_key(c),
                .klen = c->klen,
                .val = entry_val(c),
                .vlen = c->vlen,
                .flags = c->uflags,
                .cas = c->cas,
                .ttl_ms = c->expiry == 0 ? -1 : (int64_t)c->expiry - 1,
            };
            rc = fn(arg, &r);
        }
    }
    free(copy);
    return rc;
}

void akv_stats_get(akv *s, akv_stats *out) {
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < s->nshards; i++) {
        struct shard *sh = &s->shards[i];
        pthread_mutex_lock(&sh->lock);
        out->entries += sh->st.entries;
        out->arena_bytes += sh->size;
        out->log_bytes += sh->head - sh->tail;
        out->gets += sh->st.gets;
        out->hits += sh->st.hits;
        out->puts += sh->st.puts;
        out->overwrites += sh->st.overwrites;
        out->dels += sh->st.dels;
        out->evictions += sh->st.evictions;
        out->reinsertions += sh->st.reinsertions;
        out->index_evictions += sh->st.index_evictions;
        out->rejected += sh->st.rejected;
        out->expired += sh->st.expired;
        out->index_bytes += sh->nbuckets * sizeof(struct bucket);
        out->index_grows += sh->st.index_grows;
        pthread_mutex_unlock(&sh->lock);
    }
}
