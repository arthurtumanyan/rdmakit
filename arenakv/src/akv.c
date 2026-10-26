/*
 * akv.c - key-value cache engine, see akv.h.
 *
 * Arena entry layout (all entries start at multiples of 8 bytes):
 *
 *   offset size field
 *   0      4    total    entry size in bytes including this header, multiple of 8
 *   4      1    flags    ENTRY_ACCESSED, ENTRY_PAD, ENTRY_DEAD
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
 * ENTRY_DEAD marks an entry that was deleted, replaced, expired or dropped from the index while it was
 * still between tail and head. Liveness is decided by the index; the flag exists so that a warm restart
 * can rebuild the index from the arena alone. Entries behind the tail are never examined, so evictions at
 * the tail do not set it.
 *
 * Index slot: 16-bit tag (never 0 for an occupied slot) and 48-bit value (arena offset / 8 + 1).
 *
 * Index growth is incremental: a doubling allocates the new table and keeps the old one; each later store
 * and lookup moves MIGRATE_STEP buckets of the old table into the new one (index_migrate). While a move
 * is in progress every slot lives in exactly one of the two tables, lookups examine both, and new slots
 * go into the new table only. A doubling that is due before the move has finished completes it first,
 * so at most two tables exist per shard.
 *
 * Engine clock: milliseconds of CLOCK_MONOTONIC_COARSE since the engine was created, plus 1, so that 0 can
 * mean "no expiry". Expiry is therefore unaffected by changes of the wall clock. The coarse clock is read
 * from the vDSO without the timestamp counter (clock_gettime(2)); its resolution is one scheduler tick
 * (clock_getres: 1 to 10 ms), so an entry can outlive its time to live by up to one tick. memcached checks
 * expiry against a clock with a resolution of one second (memcached.c, current_time).
 *
 * CAS versions: per shard counter shifted left by 10 bits, ORed with the shard index (at most 1024
 * shards), so versions are unique within the engine without a shared counter.
 *
 * Warm-restart file (akv_config.shm_path), little-endian, mapped shared:
 *   header   struct warm_hdr: magic "AKVWARM1", version, clean flag, the configuration that must match,
 *            hash seed, wall-clock and engine time at detach, and head, tail and CAS counter per shard;
 *            padded to a multiple of 4096 bytes
 *   arenas   one per shard, `per_shard` bytes each, in shard order
 * The clean flag is set by akv_detach() after all other header fields and cleared when an engine attaches,
 * so a file left by a crash is never trusted. Engine times in the arenas are converted on attach using
 * the wall-clock time that passed between detach and attach (CLOCK_REALTIME; a clock step during the
 * downtime shifts expiry times by the same amount).
 */
#include "akv.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"
#include "simd.h"

#define ENTRY_HDR 40u
#define ENTRY_ACCESSED 1u
#define ENTRY_PAD 2u
#define ENTRY_DEAD 4u
#define SLOTS 16u
#define WINDOW 2u /* buckets examined per lookup */
#define MAX_REINSERT_STREAK 64u
#define INITIAL_BUCKETS 16u
/*
 * Old-table buckets moved per store or lookup during a move. A doubling to N buckets leaves the table at
 * 37.5% load, so the next doubling for load is at least 6N new keys away (16 slots x (75% - 37.5%)); four
 * buckets per operation finish the move after N / 8 operations. A doubling caused by a full window before
 * that completes the move at once (index_grow). Four buckets rather than one keep the move's accesses to
 * both tables sequential: filling one shard with 4 million keys from the minimal index took 10% longer in
 * total with one bucket per operation and less than 1% longer with four than with a rehash in one piece
 * (i9-12900K, one core).
 */
#define MIGRATE_STEP 4u
/* Moved old-table buckets returned to the kernel at once: 512 buckets of 128 bytes = 64 KiB. */
#define RELEASE_BUCKETS 512u
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
    /* Previous table while its slots are moved into `buckets` (NULL otherwise); buckets below `moved` are
     * empty. */
    struct bucket *old;
    uint64_t old_nbuckets;
    uint64_t old_mask;
    uint64_t moved;
    uint8_t *scratch; /* holds an entry while it is reinserted */
    uint64_t scratch_size;
    int reinserting; /* set while scratch is in use: nested evictions must not reinsert */
    uint32_t index;  /* shard number, low bits of CAS versions */
    uint64_t cas_seq;
    akv_stats st;
    char pad[64]; /* keeps the locks of neighboring shards on different cache lines */
};

struct warm_shard {
    uint64_t head;
    uint64_t tail;
    uint64_t cas_seq;
};

struct warm_hdr {
    char magic[8];
    uint32_t version;
    uint32_t clean; /* 1 after akv_detach(), 0 while an engine is attached */
    uint64_t capacity;
    uint32_t shards;
    uint32_t max_entry;
    uint64_t seed;
    uint64_t per_shard;
    int64_t wall_ms;    /* CLOCK_REALTIME at detach, milliseconds */
    uint64_t engine_ms; /* engine clock at detach */
    uint64_t reserved[4];
    struct warm_shard sh[];
};

#define WARM_MAGIC "AKVWARM1"
#define WARM_VERSION 1u

struct akv {
    akv_config cfg;
    struct shard *shards;
    uint32_t nshards;
    uint64_t seed;
    struct timespec epoch;
    int64_t wall_epoch_ms; /* CLOCK_REALTIME in milliseconds at engine time 1 */
    int quiet;             /* set while the index is rebuilt: no change records */
    int follower;          /* expired entries are hidden from lookups but not removed (akv_set_follower) */
    /* Warm-restart file mapping (fd < 0 without one). */
    int shm_fd;
    uint8_t *shm_base;
    size_t shm_size;
    struct warm_hdr *warm;
    akv_warm_info warm_info;
};

/* Lock set held by the calling thread (akv_lock_keys / akv_lock_all). */
static _Thread_local struct {
    const akv *store;
    akv_lockset set;
    uint64_t txn;
} held;

/* Set while akv_apply() runs on this thread: lookups see entries whose expiry has passed. */
static _Thread_local int applying;

static inline int is_held(const akv *s, const struct shard *sh) {
    return held.store == s && ((held.set.bits[sh->index >> 6] >> (sh->index & 63)) & 1) != 0;
}

/* Locks a shard unless the calling thread holds it through a lock set. */
static inline void sh_lock(const akv *s, struct shard *sh) {
    if (!is_held(s, sh)) {
        pthread_mutex_lock(&sh->lock);
    }
}

static inline void sh_unlock(const akv *s, struct shard *sh) {
    if (!is_held(s, sh)) {
        pthread_mutex_unlock(&sh->lock);
    }
}

static int64_t wall_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

/* Expiry time for a time to live (> 0), relative to now; times to live above AKV_TTL_MAX are clamped, so
 * expiry times stay below 2^63 and every later conversion is free of overflow. */
static uint64_t expiry_of(uint64_t now, int64_t ttl_ms) {
    if (ttl_ms <= 0) {
        return 0;
    }
    return now + (uint64_t)(ttl_ms < AKV_TTL_MAX ? ttl_ms : AKV_TTL_MAX);
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

/* ---- change feed --------------------------------------------------------------------------------- */

static inline uint64_t held_txn(const akv *s) {
    return held.store == s ? held.txn : 0;
}

static int64_t unix_expiry(const akv *s, uint64_t expiry) {
    return expiry == 0 ? 0 : s->wall_epoch_ms + (int64_t)expiry - 1;
}

static void emit_slow(akv *s, enum akv_change_kind kind, enum akv_del_reason reason, struct entry_hdr *e) {
    akv_change c = {.kind = kind, .reason = reason, .key = entry_key(e), .klen = e->klen, .txn = held_txn(s)};
    if (kind != AKV_CHG_DEL) {
        c.cas = e->cas;
        c.expire_unix_ms = unix_expiry(s, e->expiry);
    }
    if (kind == AKV_CHG_PUT) {
        c.val = entry_val(e);
        c.vlen = e->vlen;
        c.flags = e->uflags;
    }
    s->cfg.on_change(s->cfg.on_change_arg, &c);
}

/* Reports the state of entry e (PUT, TOUCH) or its removal (DEL) to the change feed, if any. */
static inline void emit(akv *s, enum akv_change_kind kind, enum akv_del_reason reason, struct entry_hdr *e) {
    if (__builtin_expect(s->cfg.on_change != NULL && !s->quiet, 0)) {
        emit_slow(s, kind, reason, e);
    }
}

/* Marks an entry that is no longer live and reports its removal. */
static inline void kill_entry(akv *s, struct entry_hdr *e, enum akv_del_reason reason) {
    e->flags |= ENTRY_DEAD;
    emit(s, AKV_CHG_DEL, reason, e);
}

/*
 * Whether the old table may still hold a slot for hash h: a move is in progress and the window of h in the
 * old table is not entirely below the move position. A window that wraps past the last bucket ends at
 * index old_nbuckets or beyond, which `moved` reaches only when the move is complete.
 */
static inline int old_may_hold(const struct shard *sh, uint64_t h) {
    return sh->old != NULL && (h & sh->old_mask) + WINDOW > sh->moved;
}

/* Finds the slot in table tab holding value `want` for hash h; NULL when absent. */
static struct bucket *window_find_pos(struct bucket *tab, uint64_t mask, uint64_t h, uint64_t want,
                                      unsigned *slot) {
    uint16_t tag = tag_of(h);
    for (unsigned n = 0; n < WINDOW; n++) {
        struct bucket *b = bucket_in(tab, mask, h, n);
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

/* Finds the slot referring to the entry at physical offset phys with the given hash; NULL when absent. */
static struct bucket *find_by_pos(struct shard *sh, uint64_t h, uint64_t phys, unsigned *slot) {
    uint64_t want = phys / 8 + 1;
    struct bucket *b = window_find_pos(sh->buckets, sh->mask, h, want, slot);
    if (b == NULL && old_may_hold(sh, h)) {
        b = window_find_pos(sh->old, sh->old_mask, h, want, slot);
    }
    return b;
}

/* Finds the slot of key (hash h) in table tab; NULL when absent. */
static struct bucket *window_find_key(const struct shard *sh, struct bucket *tab, uint64_t mask, uint64_t h,
                                      const void *key, size_t klen, unsigned *slot) {
    uint16_t tag = tag_of(h);
    for (unsigned n = 0; n < WINDOW; n++) {
        struct bucket *b = bucket_in(tab, mask, h, n);
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

/* Finds the slot of key (hash h) in either table; NULL when absent. Expired entries are found too. */
static struct bucket *find_key(struct shard *sh, uint64_t h, const void *key, size_t klen, unsigned *slot) {
    struct bucket *b = window_find_key(sh, sh->buckets, sh->mask, h, key, klen, slot);
    if (b == NULL && old_may_hold(sh, h)) {
        b = window_find_key(sh, sh->old, sh->old_mask, h, key, klen, slot);
    }
    return b;
}

/* Finds the entry of key and removes it from the index when it has expired. NULL when absent. */
static struct entry_hdr *find_live(akv *s, struct shard *sh, uint64_t h, const void *key, size_t klen,
                                   uint64_t now, struct bucket **bp, unsigned *slot) {
    struct bucket *b = find_key(sh, h, key, klen, slot);
    if (b == NULL) {
        return NULL;
    }
    struct entry_hdr *e = entry_at(sh, (slot_get(b, *slot) - 1) * 8);
    if (e->expiry != 0 && e->expiry <= now && !applying) {
        if (s->follower) {
            return NULL; /* hidden; removed by the leader's DEL record or by eviction */
        }
        b->tags[*slot] = 0; /* the entry becomes dead log space */
        sh->st.expired++;
        sh->st.entries--;
        kill_entry(s, e, AKV_DEL_EXPIRED);
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

/* Releases the old table of an unfinished move without moving its slots (flush, failed warm restore). */
static void index_drop_old(struct shard *sh) {
    if (sh->old != NULL) {
        munmap(sh->old, sh->old_nbuckets * sizeof(struct bucket));
        sh->old = NULL;
    }
}

/*
 * Moves up to `count` buckets of the old table into the current one, and releases the old table after its
 * last bucket. Entries that find no slot in the new table (a window of 32 slots already full, which is rare
 * at the post-growth load) are dropped from the index. Must not run while the caller still uses a bucket
 * pointer obtained from a lookup: a moved slot changes its bucket.
 *
 * The headers of a bucket's entries are prefetched before its slots are moved, so their cache misses
 * overlap, and the next bucket is prefetched for the following step. Every RELEASE_BUCKETS moved buckets
 * return their pages to the kernel (MADV_DONTNEED): the final munmap then frees little, and lookups that
 * still read a released bucket see zero tags, which is what a moved bucket holds.
 */
static void index_migrate(akv *s, struct shard *sh, uint64_t count) {
    while (sh->old != NULL && count-- > 0) {
        struct bucket *ob = &sh->old[sh->moved];
        for (unsigned i = 0; i < SLOTS; i++) {
            if (ob->tags[i] != 0) {
                __builtin_prefetch(entry_at(sh, (slot_get(ob, i) - 1) * 8), 0, 0);
            }
        }
        if (sh->moved + 1 < sh->old_nbuckets) {
            __builtin_prefetch(ob + 1, 1, 0);
            __builtin_prefetch((const char *)(ob + 1) + 64, 1, 0);
        }
        for (unsigned i = 0; i < SLOTS; i++) {
            if (ob->tags[i] == 0) {
                continue;
            }
            uint64_t v = slot_get(ob, i);
            struct entry_hdr *e = entry_at(sh, (v - 1) * 8);
            ob->tags[i] = 0;
            if (window_put(sh->buckets, sh->mask, e->hash, v) != 0) {
                sh->st.index_evictions++;
                sh->st.entries--;
                kill_entry(s, e, AKV_DEL_EVICTED);
            }
        }
        if (++sh->moved == sh->old_nbuckets) {
            index_drop_old(sh);
        } else if (sh->moved % RELEASE_BUCKETS == 0) {
            madvise(sh->old + (sh->moved - RELEASE_BUCKETS), RELEASE_BUCKETS * sizeof(struct bucket),
                    MADV_DONTNEED);
        }
    }
}

/* Advances an unfinished move by MIGRATE_STEP buckets; called at the end of stores and lookups. */
static inline void index_step(akv *s, struct shard *sh) {
    if (__builtin_expect(sh->old != NULL, 0)) {
        index_migrate(s, sh, MIGRATE_STEP);
    }
}

/*
 * Doubles the index of a shard: the current table becomes the old table of a move (index_migrate), after
 * an unfinished earlier move has been completed. Returns 0, or -1 when the bound is reached or memory is
 * exhausted (the index is then unchanged apart from the completed move).
 */
static int index_grow(akv *s, struct shard *sh) {
    index_migrate(s, sh, UINT64_MAX);
    uint64_t n = sh->nbuckets * 2;
    if (n > sh->max_buckets) {
        return -1;
    }
    struct bucket *tab = table_alloc(n);
    if (tab == NULL) {
        return -1;
    }
    sh->old = sh->buckets;
    sh->old_nbuckets = sh->nbuckets;
    sh->old_mask = sh->mask;
    sh->moved = 0;
    /* Atomic stores: akv_prefetch() reads both without the lock. */
    __atomic_store_n(&sh->buckets, tab, __ATOMIC_RELAXED);
    sh->nbuckets = n;
    __atomic_store_n(&sh->mask, n - 1, __ATOMIC_RELAXED);
    sh->st.index_grows++;
    return 0;
}

/*
 * Inserts a new index slot for an entry. Grows the index at 75% load or when the window is full; at the
 * growth bound a full window drops its oldest live entry (in log order), unless eviction is disabled.
 * Every insert advances an unfinished move, so a move ends within N / 8 inserts of its doubling while the
 * new table is still near 37.5% load, also where no lookups or stores run in between (warm restart
 * rebuild). Returns 0, or -ENOMEM (no_evict and no slot).
 */
static int index_insert(akv *s, struct shard *sh, int no_evict, uint64_t h, uint64_t value) {
    index_step(s, sh);
    if (sh->st.entries + 1 > sh->nbuckets * SLOTS * 3 / 4) {
        index_grow(s, sh);
    }
    while (window_put(sh->buckets, sh->mask, h, value) != 0) {
        if (index_grow(s, sh) == 0) {
            continue;
        }
        if (no_evict) {
            return -ENOMEM;
        }
        /* Window full at the bound: drop the oldest live entry of the window. */
        struct bucket *fb = bucket_at(sh, h, 0); /* replaced below: every age is below UINT64_MAX */
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
        struct entry_hdr *dropped = entry_at(sh, (slot_get(fb, fslot) - 1) * 8);
        fb->tags[fslot] = 0;
        sh->st.index_evictions++;
        sh->st.entries--;
        kill_entry(s, dropped, AKV_DEL_EVICTED);
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
        emit(s, AKV_CHG_DEL, AKV_DEL_EXPIRED, e);
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
            kill_entry(s, entry_at(sh, np), AKV_DEL_EVICTED);
        }
        (*streak)++;
        return 0;
    }
    b->tags[slot] = 0;
    sh->tail += total;
    sh->st.evictions++;
    sh->st.entries--;
    emit(s, AKV_CHG_DEL, AKV_DEL_EVICTED, e);
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

static void shard_destroy(const akv *s, struct shard *sh) {
    if (sh->arena != NULL && s->shm_fd < 0) {
        munmap(sh->arena, sh->size);
    }
    if (sh->buckets != NULL) {
        munmap(sh->buckets, sh->nbuckets * sizeof(struct bucket));
    }
    index_drop_old(sh);
    free(sh->scratch);
    pthread_mutex_destroy(&sh->lock);
}

/* Size of the warm-restart header for n shards, rounded up to a page. */
static size_t warm_hdr_size(uint32_t n) {
    size_t bytes = sizeof(struct warm_hdr) + (size_t)n * sizeof(struct warm_shard);
    return (bytes + 4095) & ~(size_t)4095;
}

/*
 * Opens and maps the warm-restart file and decides whether its content can be restored (*restore). A file
 * that cannot be restored is truncated and resized, which zeroes it. On restore the stored hash seed
 * replaces s->seed. Returns 0 or -errno.
 */
static int warm_open(akv *s, uint64_t per_shard, int *restore) {
    size_t hsz = warm_hdr_size(s->nshards);
    size_t size = hsz + (size_t)per_shard * s->nshards;
    int fd = open(s->cfg.shm_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -errno;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    enum akv_warm_state state = AKV_WARM_CREATED;
    *restore = 0;
    struct warm_hdr h;
    if (st.st_size > 0) {
        state = AKV_WARM_CORRUPT;
        if ((size_t)st.st_size >= sizeof(h) && pread(fd, &h, sizeof(h), 0) == (ssize_t)sizeof(h) &&
            memcmp(h.magic, WARM_MAGIC, 8) == 0 && h.version == WARM_VERSION) {
            if ((size_t)st.st_size != size || h.capacity != s->cfg.capacity || h.shards != s->nshards ||
                h.max_entry != s->cfg.max_entry || h.per_shard != per_shard ||
                (s->cfg.hash_seed != 0 && h.seed != s->cfg.hash_seed)) {
                state = AKV_WARM_MISMATCH;
            } else if (h.clean != 1) {
                state = AKV_WARM_UNCLEAN;
            } else {
                state = AKV_WARM_RESTORED;
                *restore = 1;
                s->seed = h.seed;
            }
        }
    }
    if (!*restore && (ftruncate(fd, 0) != 0 || ftruncate(fd, (off_t)size) != 0)) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    s->shm_fd = fd;
    s->shm_base = p;
    s->shm_size = size;
    s->warm = p;
    s->warm_info.state = state;
    struct warm_hdr *w = s->warm;
    if (!*restore) {
        memcpy(w->magic, WARM_MAGIC, 8);
        w->version = WARM_VERSION;
        w->capacity = s->cfg.capacity;
        w->shards = s->nshards;
        w->max_entry = s->cfg.max_entry;
        w->seed = s->seed;
        w->per_shard = per_shard;
    }
    w->clean = 0; /* in use: a crash from now on leaves the file untrusted */
    return 0;
}

/*
 * Rebuilds the index of one shard from its arena between the stored tail and head. Dead entries and
 * entries whose time ran out during the downtime (elapsed_ms of wall-clock time after engine time
 * old_now) are skipped; remaining expiry times are moved to the current engine clock. Returns 0, or -1
 * when a log position or an entry header is inconsistent.
 */
static int warm_rebuild(akv *s, struct shard *sh, const struct warm_shard *ws, int64_t elapsed_ms,
                        uint64_t old_now, uint64_t now) {
    if (ws->tail > ws->head || ws->head - ws->tail > sh->size) {
        return -1;
    }
    sh->head = ws->head;
    sh->tail = ws->tail;
    sh->cas_seq = ws->cas_seq;
    for (uint64_t pos = sh->tail; pos < sh->head;) {
        uint64_t phys = pos % sh->size;
        struct entry_hdr *e = entry_at(sh, phys);
        uint32_t total = e->total;
        if (total < 8 || total % 8 != 0 || total > sh->head - pos || total > sh->size - phys) {
            return -1;
        }
        pos += total;
        if (e->flags & ENTRY_PAD) {
            if (phys + total != sh->size) {
                return -1;
            }
            continue;
        }
        if (total < ENTRY_HDR || e->klen == 0 || entry_total(e->klen, e->vlen) != total) {
            return -1;
        }
        if (e->flags & ENTRY_DEAD) {
            continue;
        }
        if (e->hash != akv_hash64(entry_key(e), e->klen, s->seed)) {
            return -1;
        }
        if (e->expiry != 0) {
            if (e->expiry > (uint64_t)AKV_TTL_MAX * 2) {
                return -1; /* larger than any expiry the engine writes */
            }
            int64_t left = (int64_t)e->expiry - (int64_t)old_now - elapsed_ms;
            if (left <= 0) {
                e->flags |= ENTRY_DEAD;
                s->warm_info.expired++;
                continue;
            }
            e->expiry = now + (uint64_t)left;
        }
        uint64_t value = phys / 8 + 1;
        unsigned slot;
        struct bucket *b = find_key(sh, e->hash, entry_key(e), e->klen, &slot);
        if (b != NULL) {
            /* An older version without the dead mark: the later one in log order is current. */
            entry_at(sh, (slot_get(b, slot) - 1) * 8)->flags |= ENTRY_DEAD;
            slot_set(b, slot, tag_of(e->hash), value);
            continue;
        }
        if (index_insert(s, sh, 0, e->hash, value) != 0) {
            e->flags |= ENTRY_DEAD;
        }
    }
    return 0;
}

/* Restores all shards from the warm-restart file; on an inconsistency every shard starts empty. */
static void warm_restore(akv *s) {
    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const struct warm_hdr *w = s->warm;
    int64_t elapsed;
    if (__builtin_sub_overflow(wall_now_ms(), w->wall_ms, &elapsed) || elapsed > AKV_TTL_MAX) {
        elapsed = AKV_TTL_MAX; /* a damaged time: every entry with an expiry is treated as expired */
    }
    if (elapsed < 0) {
        elapsed = 0;
    }
    uint64_t now = now_ms(s);
    s->quiet = 1;
    int bad = w->engine_ms > (uint64_t)AKV_TTL_MAX * 2; /* larger than any engine time */
    for (uint32_t i = 0; i < s->nshards && !bad; i++) {
        bad = warm_rebuild(s, &s->shards[i], &w->sh[i], elapsed, w->engine_ms, now) != 0;
    }
    if (bad) {
        for (uint32_t i = 0; i < s->nshards; i++) {
            struct shard *sh = &s->shards[i];
            madvise(sh->arena, sh->size, MADV_REMOVE);
            memset(sh->buckets, 0, sh->nbuckets * sizeof(struct bucket));
            index_drop_old(sh);
            sh->head = 0;
            sh->tail = 0;
            sh->st.entries = 0;
        }
        s->warm_info.state = AKV_WARM_CORRUPT;
        s->warm_info.expired = 0;
    }
    s->quiet = 0;
    for (uint32_t i = 0; i < s->nshards; i++) {
        s->warm_info.entries += s->shards[i].st.entries;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    s->warm_info.rebuild_us =
        (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
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
        errno = EINVAL;
        return NULL;
    }
    akv *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->cfg = cfg;
    s->nshards = cfg.shards;
    s->seed = cfg.hash_seed;
    s->shm_fd = -1;
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
    int restore = 0;
    if (cfg.shm_path != NULL) {
        int rc = warm_open(s, per_shard, &restore);
        if (rc != 0) {
            free(s->shards);
            free(s);
            errno = -rc;
            return NULL;
        }
    }
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
        if (s->shm_fd >= 0) {
            sh->arena = s->shm_base + warm_hdr_size(s->nshards) + (size_t)i * per_shard;
        } else {
            sh->arena = mmap(NULL, sh->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (sh->arena == MAP_FAILED) {
                sh->arena = NULL;
                akv_destroy(s);
                return NULL;
            }
            if (cfg.huge_pages) {
                madvise(sh->arena, sh->size, MADV_HUGEPAGE);
            }
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
    s->wall_epoch_ms = wall_now_ms() - (int64_t)(now_ms(s) - 1);
    if (restore) {
        warm_restore(s);
    }
    return s;
}

void akv_destroy(akv *s) {
    if (s == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->nshards; i++) {
        shard_destroy(s, &s->shards[i]);
    }
    if (s->shm_fd >= 0) {
        munmap(s->shm_base, s->shm_size);
        close(s->shm_fd);
    }
    free(s->shards);
    free(s);
}

int akv_detach(akv *s) {
    if (s->shm_fd < 0) {
        akv_destroy(s);
        return -EINVAL;
    }
    struct warm_hdr *w = s->warm;
    for (uint32_t i = 0; i < s->nshards; i++) {
        const struct shard *sh = &s->shards[i];
        w->sh[i] = (struct warm_shard){.head = sh->head, .tail = sh->tail, .cas_seq = sh->cas_seq};
    }
    w->wall_ms = wall_now_ms();
    w->engine_ms = now_ms(s);
    /* The flag is written last: a reader that sees it set sees all other fields. */
    __atomic_store_n(&w->clean, 1u, __ATOMIC_RELEASE);
    akv_destroy(s);
    return 0;
}

void akv_warm_get(const akv *s, akv_warm_info *out) {
    *out = s->warm_info;
}

/* ---- operations ------------------------------------------------------------------------------------ */

static inline struct shard *shard_of(akv *s, uint64_t h) {
    return &s->shards[(h >> 32) & (s->nshards - 1)];
}

/*
 * Prefetches the first index bucket of the key (both cache lines of the 128-byte bucket). Reads the table
 * pointer and mask without the lock: during a concurrent index growth they may belong to different
 * tables, and the address may then lie outside the current table. A prefetch of any address neither
 * faults nor changes memory (Intel SDM vol. 2, PREFETCHh), so the hint is harmless. During a move a key
 * may still be in the old table, which is not prefetched.
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
 * the buckets are expected to be in the cache. Examines the first bucket of the current table only.
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
        if (plen[0] > 0 && parts[0] != v) { /* an empty value may come without a buffer (replayed records) */
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
        emit(s, AKV_CHG_PUT, AKV_DEL_CLIENT, old);
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
        /* Replace: the previous version is now dead log space. */
        entry_at(sh, (slot_get(b, slot) - 1) * 8)->flags |= ENTRY_DEAD;
        slot_set(b, slot, tag_of(h), pos_value);
        sh->st.overwrites++;
    } else if (b == NULL) {
        if (index_insert(s, sh, s->cfg.no_evict, h, pos_value) != 0) {
            /* The entry stays as dead log space. */
            e->flags |= ENTRY_DEAD;
            sh->st.rejected++;
            return -ENOMEM;
        }
    }
    sh->st.puts++;
    emit(s, AKV_CHG_PUT, AKV_DEL_CLIENT, e);
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
    sh_lock(s, sh);
    uint64_t now = now_ms(s);
    struct bucket *b = NULL;
    unsigned slot = 0;
    struct entry_hdr *old = find_live(s, sh, h, key, klen, now, &b, &slot);
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
        sh_unlock(s, sh);
        return rc;
    }
    if (opts->ttl_ms < 0 && opts->mode != AKV_APPEND && opts->mode != AKV_PREPEND) {
        /* Stored and expired at once: the key no longer exists. */
        if (old != NULL) {
            b->tags[slot] = 0;
            sh->st.entries--;
            sh->st.expired++;
            kill_entry(s, old, AKV_DEL_CLIENT);
        }
        sh->st.puts++;
        sh_unlock(s, sh);
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
            sh_unlock(s, sh);
            return -E2BIG;
        }
        /* Copy the old value: allocating the new version may evict or move the old one. */
        tmp = malloc(old->vlen > 0 ? old->vlen : 1);
        if (tmp == NULL) {
            sh_unlock(s, sh);
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
    index_step(s, sh);
    sh_unlock(s, sh);
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
    sh_lock(s, sh);
    sh->st.gets++;
    uint64_t now = now_ms(s);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(s, sh, h, key, klen, now, &b, &slot);
    int rc;
    if (e == NULL) {
        rc = -ENOENT;
    } else {
        int touch = touch_ttl_ms != AKV_TTL_KEEP;
        if (touch && touch_ttl_ms >= 0) {
            e->expiry = expiry_of(now, touch_ttl_ms);
            e->cas = next_cas(sh);
            emit(s, AKV_CHG_TOUCH, AKV_DEL_CLIENT, e);
        }
        *vlen = e->vlen;
        if (meta != NULL) {
            meta->flags = e->uflags;
            meta->cas = e->cas;
            meta->ttl_ms = touch && touch_ttl_ms < 0 ? 0 : ttl_left(e->expiry, now);
        }
        if (e->vlen > cap) {
            rc = -EMSGSIZE;
        } else {
            memcpy(buf, entry_val(e), e->vlen);
            e->flags |= ENTRY_ACCESSED;
            sh->st.hits++;
            rc = 0;
        }
        if (touch && touch_ttl_ms < 0) {
            /* Returned once, then removed (memcached gat with a negative expiry). */
            b->tags[slot] = 0;
            sh->st.entries--;
            sh->st.expired++;
            kill_entry(s, e, AKV_DEL_CLIENT);
        }
    }
    index_step(s, sh);
    sh_unlock(s, sh);
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
    sh_lock(s, sh);
    struct bucket *b;
    unsigned slot;
    int found = find_live(s, sh, h, key, klen, now_ms(s), &b, &slot) != NULL;
    sh_unlock(s, sh);
    return found;
}

int akv_del(akv *s, const void *key, size_t klen) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    sh_lock(s, sh);
    struct bucket *b;
    unsigned slot;
    int rc = -ENOENT;
    struct entry_hdr *e = find_live(s, sh, h, key, klen, now_ms(s), &b, &slot);
    if (e != NULL) {
        b->tags[slot] = 0;
        sh->st.dels++;
        sh->st.entries--;
        kill_entry(s, e, AKV_DEL_CLIENT);
        rc = 0;
    }
    sh_unlock(s, sh);
    return rc;
}

int akv_touch(akv *s, const void *key, size_t klen, int64_t ttl_ms) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    sh_lock(s, sh);
    uint64_t now = now_ms(s);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(s, sh, h, key, klen, now, &b, &slot);
    int rc = -ENOENT;
    if (e != NULL) {
        if (ttl_ms < 0) {
            b->tags[slot] = 0;
            sh->st.entries--;
            sh->st.expired++;
            kill_entry(s, e, AKV_DEL_CLIENT);
        } else {
            e->expiry = expiry_of(now, ttl_ms);
            e->cas = next_cas(sh);
            emit(s, AKV_CHG_TOUCH, AKV_DEL_CLIENT, e);
        }
        rc = 0;
    }
    sh_unlock(s, sh);
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
    sh_lock(s, sh);
    uint64_t now = now_ms(s);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(s, sh, h, key, klen, now, &b, &slot);
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
        index_step(s, sh);
    }
    sh_unlock(s, sh);
    return rc;
}

void akv_flush(akv *s) {
    for (uint32_t i = 0; i < s->nshards; i++) {
        sh_lock(s, &s->shards[i]);
    }
    if (s->cfg.on_change != NULL && !s->quiet) {
        akv_change c = {.kind = AKV_CHG_FLUSH, .txn = held_txn(s)};
        s->cfg.on_change(s->cfg.on_change_arg, &c);
    }
    for (uint32_t i = 0; i < s->nshards; i++) {
        struct shard *sh = &s->shards[i];
        /* Private anonymous memory reads back as zero after MADV_DONTNEED; shared file pages need MADV_REMOVE. */
        madvise(sh->arena, sh->size, s->shm_fd >= 0 ? MADV_REMOVE : MADV_DONTNEED);
        madvise(sh->buckets, sh->nbuckets * sizeof(struct bucket), MADV_DONTNEED);
        index_drop_old(sh);
        sh->head = 0;
        sh->tail = 0;
        sh->st.entries = 0;
    }
    for (uint32_t i = s->nshards; i-- > 0;) {
        sh_unlock(s, &s->shards[i]);
    }
}

int akv_foreach(akv *s, int (*fn)(void *arg, const akv_record *r), void *arg) {
    uint8_t *copy = NULL;
    size_t copy_cap = 0;
    int rc = 0;
    for (uint32_t i = 0; i < s->nshards && rc == 0; i++) {
        struct shard *sh = &s->shards[i];
        sh_lock(s, sh);
        uint64_t now = now_ms(s);
        /* Copy live entries in log order; ttl is stored in the expiry field as the remaining time + 1. */
        size_t need = (size_t)(sh->head - sh->tail);
        if (need == 0) {
            sh_unlock(s, sh);
            continue;
        }
        if (need > copy_cap) {
            uint8_t *p = realloc(copy, need);
            if (p == NULL) {
                sh_unlock(s, sh);
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
        sh_unlock(s, sh);
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
        sh_lock(s, sh);
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
        if (sh->old != NULL) {
            out->index_bytes += sh->old_nbuckets * sizeof(struct bucket);
        }
        out->index_grows += sh->st.index_grows;
        sh_unlock(s, sh);
    }
}

/* ---- replay, lock sets ----------------------------------------------------------------------------- */

/* Engine-clock expiry of an absolute Unix expiry; a passed time maps to 1 (expired), none to 0. */
static uint64_t engine_expiry(const akv *s, int64_t expire_unix_ms) {
    if (expire_unix_ms == 0) {
        return 0;
    }
    int64_t e;
    if (__builtin_sub_overflow(expire_unix_ms, s->wall_epoch_ms - 1, &e)) {
        e = expire_unix_ms < 0 ? 1 : AKV_TTL_MAX; /* records from a damaged source */
    }
    return e < 1 ? 1 : (uint64_t)(e < AKV_TTL_MAX ? e : AKV_TTL_MAX);
}

/* Sets the expiry of the entry of key (engine clock) under the shard lock; 0 when present, -ENOENT. */
static int set_expiry(akv *s, const void *key, size_t klen, uint64_t expiry) {
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    sh_lock(s, sh);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(s, sh, h, key, klen, now_ms(s), &b, &slot);
    int rc = -ENOENT;
    if (e != NULL) {
        e->expiry = expiry;
        e->cas = next_cas(sh);
        emit(s, AKV_CHG_TOUCH, AKV_DEL_CLIENT, e);
        rc = 0;
    }
    sh_unlock(s, sh);
    return rc;
}

/*
 * Records are applied with lookups that see expired entries, and expiry times are applied as absolute
 * times even when they have passed: a record produced before an expiry (a touch that removes it, an
 * append) is then applied to the same entry it saw. Expiry itself arrives as a DEL record. Valkey treats
 * expiry the same way while loading an append-only file and on replicas, which delete expired keys only
 * on the primary's DEL.
 */
int akv_apply(akv *s, const akv_change *c) {
    int rc = 0;
    applying = 1;
    switch (c->kind) {
    case AKV_CHG_PUT: {
        akv_store_opts o = {.mode = AKV_SET, .flags = c->flags};
        rc = akv_store(s, c->key, c->klen, c->val, c->vlen, &o, NULL);
        if (rc == 0 && c->expire_unix_ms != 0) {
            rc = set_expiry(s, c->key, c->klen, engine_expiry(s, c->expire_unix_ms));
        }
        break;
    }
    case AKV_CHG_DEL:
        rc = akv_del(s, c->key, c->klen);
        break;
    case AKV_CHG_TOUCH:
        rc = c->klen == 0 || c->klen > 0xffff
                 ? -EINVAL
                 : set_expiry(s, c->key, c->klen, engine_expiry(s, c->expire_unix_ms));
        break;
    case AKV_CHG_FLUSH:
        akv_flush(s);
        break;
    default:
        rc = -EINVAL;
        break;
    }
    applying = 0;
    return rc == -ENOENT ? 0 : rc;
}

void akv_set_follower(akv *s, int on) {
    __atomic_store_n(&s->follower, on != 0, __ATOMIC_RELAXED);
}

static void lock_set(akv *s, const akv_lockset *set, uint64_t txn) {
    for (uint32_t i = 0; i < s->nshards; i++) {
        if ((set->bits[i >> 6] >> (i & 63)) & 1) {
            pthread_mutex_lock(&s->shards[i].lock);
        }
    }
    held.store = s;
    held.set = *set;
    held.txn = txn;
}

void akv_lock_keys(akv *s, const void *const *keys, const size_t *lens, size_t n, uint64_t txn,
                   akv_lockset *set) {
    memset(set, 0, sizeof(*set));
    for (size_t i = 0; i < n; i++) {
        if (lens[i] == 0 || lens[i] > 0xffff) {
            continue; /* rejected by the operation itself */
        }
        uint32_t idx = shard_of(s, akv_hash64(keys[i], lens[i], s->seed))->index;
        set->bits[idx >> 6] |= 1ull << (idx & 63);
    }
    lock_set(s, set, txn);
}

void akv_lock_all(akv *s, uint64_t txn, akv_lockset *set) {
    memset(set, 0, sizeof(*set));
    for (uint32_t i = 0; i < s->nshards; i++) {
        set->bits[i >> 6] |= 1ull << (i & 63);
    }
    lock_set(s, set, txn);
}

void akv_unlock(akv *s, const akv_lockset *set) {
    memset(&held, 0, sizeof(held));
    for (uint32_t i = s->nshards; i-- > 0;) {
        if ((set->bits[i >> 6] >> (i & 63)) & 1) {
            pthread_mutex_unlock(&s->shards[i].lock);
        }
    }
}

uint64_t akv_cas_of(akv *s, const void *key, size_t klen) {
    if (klen == 0 || klen > 0xffff) {
        return 0;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    sh_lock(s, sh);
    struct bucket *b;
    unsigned slot;
    struct entry_hdr *e = find_live(s, sh, h, key, klen, now_ms(s), &b, &slot);
    uint64_t cas = e != NULL ? e->cas : 0;
    sh_unlock(s, sh);
    return cas;
}
