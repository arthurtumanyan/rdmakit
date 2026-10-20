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
 *   12     4    reserved
 *   16     8    hash     64-bit key hash
 *   24     k    key
 *   24+k   v    value, followed by padding to a multiple of 8
 *
 * A PAD entry fills the unusable space at the end of the arena when the next entry does not fit before
 * the wrap-around. Only `total` and `flags` (the first 5 bytes) are written for it, because the gap can
 * be as small as 8 bytes.
 *
 * Index slot: 16-bit tag (never 0 for an occupied slot) and 48-bit value (arena offset / 8 + 1).
 */
#include "akv.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"
#include "simd.h"

#define ENTRY_HDR 24u
#define ENTRY_ACCESSED 1u
#define ENTRY_PAD 2u
#define SLOTS 16u
#define WINDOW 2u /* buckets examined per lookup */
#define MAX_REINSERT_STREAK 64u

struct entry_hdr {
    uint32_t total;
    uint8_t flags;
    uint8_t reserved8;
    uint16_t klen;
    uint32_t vlen;
    uint32_t reserved32;
    uint64_t hash;
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
    uint64_t mask;    /* bucket count - 1 */
    uint8_t *scratch; /* holds an entry while it is reinserted */
    uint64_t scratch_size;
    int reinserting; /* set while scratch is in use: nested evictions must not reinsert */
    akv_stats st;
    char pad[64]; /* keeps the locks of neighboring shards on different cache lines */
};

struct akv {
    akv_config cfg;
    struct shard *shards;
    uint32_t nshards;
    uint64_t seed;
};

const char *akv_simd_name(void) {
    return AKV_SIMD_NAME;
}

void akv_config_default(akv_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->capacity = 256ull << 20;
    cfg->shards = 64;
    cfg->max_entry = 1u << 20;
    cfg->avg_entry = 256;
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

static inline struct bucket *bucket_at(struct shard *sh, uint64_t h, unsigned n) {
    return &sh->buckets[(h + n) & sh->mask];
}

static inline struct entry_hdr *entry_at(const struct shard *sh, uint64_t phys) {
    return (struct entry_hdr *)(sh->arena + phys);
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

/* Finds the slot of key (hash h); NULL when absent. */
static struct bucket *find_key(struct shard *sh, uint64_t h, const void *key, size_t klen, unsigned *slot) {
    uint16_t tag = tag_of(h);
    for (unsigned n = 0; n < WINDOW; n++) {
        struct bucket *b = bucket_at(sh, h, n);
        uint32_t m = akv_match16(b->tags, tag);
        while (m != 0) {
            unsigned i = (unsigned)__builtin_ctz(m);
            m &= m - 1;
            const struct entry_hdr *e = entry_at(sh, (slot_get(b, i) - 1) * 8);
            if (e->hash == h && e->klen == klen && memcmp((const uint8_t *)e + ENTRY_HDR, key, klen) == 0) {
                *slot = i;
                return b;
            }
        }
    }
    return NULL;
}

/* ---- arena ----------------------------------------------------------------------------------------- */

static inline uint64_t entry_total(size_t klen, size_t vlen) {
    return (ENTRY_HDR + klen + vlen + 7) & ~(uint64_t)7;
}

/* Frees the oldest log entry: skips dead entries, gives a recently read live entry a second chance. */
static void evict_one(struct shard *sh, unsigned *streak);

/*
 * Appends an entry of `total` bytes at the head, evicting from the tail as needed, and returns its
 * physical offset. The caller fills the entry.
 */
static uint64_t arena_alloc(struct shard *sh, uint64_t total) {
    unsigned streak = 0;
    uint64_t pos = sh->head % sh->size;
    uint64_t pad = pos + total > sh->size ? sh->size - pos : 0;
    while (sh->size - (sh->head - sh->tail) < pad + total) {
        evict_one(sh, &streak);
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

static void write_entry(struct shard *sh, uint64_t phys, uint64_t h, const void *key, size_t klen,
                        const void *val, size_t vlen, uint64_t total) {
    struct entry_hdr *e = entry_at(sh, phys);
    e->total = (uint32_t)total;
    e->flags = 0;
    e->reserved8 = 0;
    e->klen = (uint16_t)klen;
    e->vlen = (uint32_t)vlen;
    e->reserved32 = 0;
    e->hash = h;
    memcpy((uint8_t *)e + ENTRY_HDR, key, klen);
    if (vlen > 0) {
        memcpy((uint8_t *)e + ENTRY_HDR + klen, val, vlen);
    }
}

static void evict_one(struct shard *sh, unsigned *streak) {
    uint64_t phys = sh->tail % sh->size;
    struct entry_hdr *e = entry_at(sh, phys);
    uint32_t total = e->total;
    if (e->flags & ENTRY_PAD) {
        sh->tail += total;
        return;
    }
    unsigned slot;
    struct bucket *b = find_by_pos(sh, e->hash, phys, &slot);
    if (b == NULL) { /* dead: overwritten or deleted */
        sh->tail += total;
        return;
    }
    if ((e->flags & ENTRY_ACCESSED) && !sh->reinserting && *streak < MAX_REINSERT_STREAK) {
        /* Second chance: copy to the head with the accessed bit cleared. The old copy is released first. */
        memcpy(sh->scratch, e, total);
        uint64_t hash = e->hash;
        b->tags[slot] = 0;
        sh->tail += total;
        sh->reinserting = 1;
        uint64_t np = arena_alloc(sh, total);
        sh->reinserting = 0;
        memcpy(entry_at(sh, np), sh->scratch, total);
        entry_at(sh, np)->flags = 0;
        /* Re-register the entry: the slot may have been cleared by nested evictions, so insert afresh. */
        unsigned free_slot = SLOTS;
        struct bucket *fb = NULL;
        for (unsigned n = 0; n < WINDOW && fb == NULL; n++) {
            struct bucket *cand = bucket_at(sh, hash, n);
            for (unsigned i = 0; i < SLOTS; i++) {
                if (cand->tags[i] == 0) {
                    fb = cand;
                    free_slot = i;
                    break;
                }
            }
        }
        if (fb != NULL) {
            slot_set(fb, free_slot, tag_of(hash), np / 8 + 1);
            sh->st.reinsertions++;
        } else {
            sh->st.index_evictions++;
            sh->st.entries--;
        }
        (*streak)++;
        return;
    }
    b->tags[slot] = 0;
    sh->tail += total;
    sh->st.evictions++;
    sh->st.entries--;
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
    if (cfg.avg_entry == 0) {
        cfg.avg_entry = 256;
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
    uint64_t slots_needed = per_shard / cfg.avg_entry + 1;
    uint64_t nbuckets = next_pow2(slots_needed * 4 / (SLOTS * 3) + 1);
    if (nbuckets < 2) {
        nbuckets = 2;
    }
    for (uint32_t i = 0; i < cfg.shards; i++) {
        struct shard *sh = &s->shards[i];
        pthread_mutex_init(&sh->lock, NULL);
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
        sh->mask = nbuckets - 1;
        /* Anonymous mappings are zero-filled and committed lazily: untouched buckets cost no resident memory. */
        sh->buckets = mmap(NULL, nbuckets * sizeof(struct bucket), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sh->buckets == MAP_FAILED) {
            sh->buckets = NULL;
            akv_destroy(s);
            return NULL;
        }
        sh->nbuckets = nbuckets;
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

int akv_put(akv *s, const void *key, size_t klen, const void *val, size_t vlen) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    uint64_t total = entry_total(klen, vlen);
    if (klen + vlen > s->cfg.max_entry || total > sh->size / 2 || total > sh->scratch_size) {
        pthread_mutex_lock(&sh->lock);
        sh->st.rejected++;
        pthread_mutex_unlock(&sh->lock);
        return -E2BIG;
    }
    pthread_mutex_lock(&sh->lock);
    sh->st.puts++;

    /* An entry of the same size is overwritten in place: no log space is consumed and no dead entry is left. */
    unsigned slot;
    struct bucket *b = find_key(sh, h, key, klen, &slot);
    if (b != NULL) {
        struct entry_hdr *old = entry_at(sh, (slot_get(b, slot) - 1) * 8);
        if (old->total == total) {
            old->vlen = (uint32_t)vlen;
            if (vlen > 0) {
                memcpy((uint8_t *)old + ENTRY_HDR + klen, val, vlen);
            }
            sh->st.overwrites++;
            pthread_mutex_unlock(&sh->lock);
            return 0;
        }
    }

    uint64_t phys = arena_alloc(sh, total);
    write_entry(sh, phys, h, key, klen, val, vlen, total);
    uint64_t pos_value = phys / 8 + 1;

    b = find_key(sh, h, key, klen, &slot); /* the allocation may have evicted the old version */
    if (b != NULL) {
        slot_set(b, slot, tag_of(h), pos_value); /* replace: the previous version is now dead log space */
        sh->st.overwrites++;
    } else {
        struct bucket *fb = NULL;
        unsigned fslot = 0;
        for (unsigned n = 0; n < WINDOW && fb == NULL; n++) {
            struct bucket *cand = bucket_at(sh, h, n);
            for (unsigned i = 0; i < SLOTS; i++) {
                if (cand->tags[i] == 0) {
                    fb = cand;
                    fslot = i;
                    break;
                }
            }
        }
        if (fb == NULL) {
            /* Window full: drop the oldest live entry of the window (smallest age in log order). */
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
            sh->st.index_evictions++;
            sh->st.entries--;
        }
        slot_set(fb, fslot, tag_of(h), pos_value);
        sh->st.entries++;
    }
    pthread_mutex_unlock(&sh->lock);
    return 0;
}

int akv_get(akv *s, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    sh->st.gets++;
    unsigned slot;
    struct bucket *b = find_key(sh, h, key, klen, &slot);
    int rc;
    if (b == NULL) {
        rc = -ENOENT;
    } else {
        struct entry_hdr *e = entry_at(sh, (slot_get(b, slot) - 1) * 8);
        *vlen = e->vlen;
        if (e->vlen > cap) {
            rc = -EMSGSIZE;
        } else {
            memcpy(buf, (const uint8_t *)e + ENTRY_HDR + klen, e->vlen);
            e->flags |= ENTRY_ACCESSED;
            sh->st.hits++;
            rc = 0;
        }
    }
    pthread_mutex_unlock(&sh->lock);
    return rc;
}

int akv_del(akv *s, const void *key, size_t klen) {
    if (klen == 0 || klen > 0xffff) {
        return -EINVAL;
    }
    uint64_t h = akv_hash64(key, klen, s->seed);
    struct shard *sh = shard_of(s, h);
    pthread_mutex_lock(&sh->lock);
    unsigned slot;
    struct bucket *b = find_key(sh, h, key, klen, &slot);
    int rc = -ENOENT;
    if (b != NULL) {
        b->tags[slot] = 0;
        sh->st.dels++;
        sh->st.entries--;
        rc = 0;
    }
    pthread_mutex_unlock(&sh->lock);
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
        pthread_mutex_unlock(&sh->lock);
    }
}
