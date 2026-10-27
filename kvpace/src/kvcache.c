/* kvcache.c - KV-cache block workload, see kvcache.h. */
#include "kvcache.h"

#include <stdlib.h>
#include <string.h>

struct conv_slot {
    uint64_t id;    /* conversation identity, part of the block keys */
    uint32_t group; /* prefix group of the conversation */
    uint32_t turns; /* turns already completed */
};

struct pace_kvcache_state {
    pace_kvcache_cfg cfg;
    pace_rng rng;
    pace_zipf group_zipf;
    pace_zipf slot_zipf;
    struct conv_slot *slots;
    uint64_t next_conv; /* next conversation identity of this thread */
    unsigned thread_id;
    unsigned char *block;  /* scratch: block to write */
    unsigned char *expect; /* scratch: expected content of a hit */
    unsigned char *got;    /* scratch: received content */
};

/* 64-bit mixing of four words (splitmix64 chain). */
static uint64_t mix(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t s = a;
    uint64_t h = pace_splitmix64(&s);
    s = h ^ b;
    h = pace_splitmix64(&s);
    s = h ^ c;
    h = pace_splitmix64(&s);
    s = h ^ d;
    return pace_splitmix64(&s);
}

size_t pace_kvcache_block_key(uint64_t workload_seed, uint64_t group, uint64_t conv, uint64_t index,
                              char *key) {
    static const char hex[] = "0123456789abcdef";
    uint64_t h = mix(workload_seed, group, conv, index);
    key[0] = 'b';
    for (int i = 0; i < 16; i++) {
        key[1 + i] = hex[(h >> (60 - 4 * i)) & 15];
    }
    return PACE_KVCACHE_KEY_LEN;
}

/* Fills buf with the content belonging to the block identified by key. */
static void block_content(const char *key, size_t klen, unsigned char *buf, uint32_t size) {
    uint64_t state = 0x517cc1b727220a95ull;
    for (size_t i = 0; i < klen; i++) {
        state = (state ^ (uint8_t)key[i]) * 0x100000001b3ull;
    }
    for (uint32_t i = 0; i < size; i += 8) {
        uint64_t v = pace_splitmix64(&state);
        uint32_t n = size - i < 8 ? size - i : 8;
        memcpy(buf + i, &v, n);
    }
}

void pace_kvcache_defaults(pace_kvcache_cfg *cfg) {
    cfg->block_size = 16384;
    cfg->groups = 16;
    cfg->shared_blocks = 32;
    cfg->turn_blocks = 8;
    cfg->active_conv = 256;
    cfg->max_turns = 16;
    cfg->new_conv_prob = 0.05;
    cfg->theta = 0.9;
    cfg->seed = 1;
}

static void slot_renew(struct pace_kvcache_state *st, struct conv_slot *s) {
    s->id = ((uint64_t)st->thread_id << 40) | st->next_conv++;
    s->group = (uint32_t)pace_zipf_next(&st->group_zipf, &st->rng);
    s->turns = 0;
}

pace_kvcache_state *pace_kvcache_create(const pace_kvcache_cfg *cfg, unsigned thread_id,
                                        uint64_t thread_seed) {
    if (cfg->block_size == 0 || cfg->groups == 0 || cfg->active_conv == 0 || cfg->turn_blocks == 0 ||
        cfg->max_turns == 0) {
        return NULL;
    }
    pace_kvcache_state *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->cfg = *cfg;
    st->thread_id = thread_id;
    pace_rng_seed(&st->rng, thread_seed);
    if (pace_zipf_init(&st->group_zipf, cfg->groups, cfg->theta) != 0 ||
        pace_zipf_init(&st->slot_zipf, cfg->active_conv, cfg->theta) != 0) {
        free(st);
        return NULL;
    }
    st->slots = calloc(cfg->active_conv, sizeof(*st->slots));
    st->block = malloc(cfg->block_size);
    st->expect = malloc(cfg->block_size);
    st->got = malloc((size_t)cfg->block_size + 64);
    if (st->slots == NULL || st->block == NULL || st->expect == NULL || st->got == NULL) {
        pace_kvcache_destroy(st);
        return NULL;
    }
    for (uint32_t i = 0; i < cfg->active_conv; i++) {
        slot_renew(st, &st->slots[i]);
    }
    return st;
}

void pace_kvcache_destroy(pace_kvcache_state *st) {
    if (st == NULL) {
        return;
    }
    free(st->slots);
    free(st->block);
    free(st->expect);
    free(st->got);
    free(st);
}

/* Returns the key of block i of the chain of a conversation (template blocks first). */
static size_t chain_key(const pace_kvcache_state *st, const struct conv_slot *s, uint32_t i, char *key) {
    if (i < st->cfg.shared_blocks) {
        return pace_kvcache_block_key(st->cfg.seed, s->group, 0, i, key);
    }
    return pace_kvcache_block_key(st->cfg.seed, s->group, s->id, i - st->cfg.shared_blocks + 1, key);
}

void pace_kvcache_begin(pace_kvcache_state *st, pace_kvcache_req *r) {
    const pace_kvcache_cfg *cfg = &st->cfg;
    uint32_t slot = (uint32_t)pace_zipf_next(&st->slot_zipf, &st->rng);
    struct conv_slot *s = &st->slots[slot];
    if (pace_rng_double(&st->rng) < cfg->new_conv_prob || s->turns >= cfg->max_turns) {
        slot_renew(st, s);
    }
    memset(r, 0, sizeof(*r));
    r->slot = slot;
    r->total = cfg->shared_blocks + (s->turns + 1) * cfg->turn_blocks;
    s->turns++;
    r->stats.requests = 1;
}

int pace_kvcache_op(pace_kvcache_state *st, pace_kvcache_req *r, pace_req *op) {
    if (r->index >= r->total) {
        return 0;
    }
    const struct conv_slot *s = &st->slots[r->slot];
    size_t klen = chain_key(st, s, r->index, r->key);
    *op = (pace_req){.key = r->key, .klen = klen};
    if (!r->putting) {
        op->type = PACE_OP_GET;
        r->stats.block_gets++;
    } else {
        block_content(r->key, klen, st->block, st->cfg.block_size);
        op->type = PACE_OP_SET;
        op->val = st->block;
        op->vlen = st->cfg.block_size;
    }
    return 1;
}

int pace_kvcache_result(pace_kvcache_state *st, pace_kvcache_req *r, int status, const void *val,
                        size_t vlen) {
    const pace_kvcache_cfg *cfg = &st->cfg;
    if (status == PACE_ERR) {
        return PACE_ERR;
    }
    if (r->putting) {
        r->stats.block_puts++;
        r->stats.bytes_put += cfg->block_size;
        r->index++;
        return PACE_OK;
    }
    if (status == PACE_NOTFOUND) {
        r->putting = 1; /* the recomputed tail starts at the first miss */
        return PACE_OK;
    }
    r->stats.block_hits++;
    r->stats.bytes_get += vlen;
    block_content(r->key, PACE_KVCACHE_KEY_LEN, st->expect, cfg->block_size);
    if (val == NULL || vlen != cfg->block_size || memcmp(val, st->expect, cfg->block_size) != 0) {
        r->stats.corrupt++;
    }
    r->index++;
    return PACE_OK;
}

int pace_kvcache_request(pace_kvcache_state *st, const pace_driver *drv, pace_conn *conn,
                         pace_kvcache_stats *out) {
    pace_kvcache_req r;
    pace_req op;
    int rc = PACE_OK;
    pace_kvcache_begin(st, &r);
    while (rc == PACE_OK && pace_kvcache_op(st, &r, &op)) {
        if (op.type == PACE_OP_GET) {
            size_t got = 0;
            int g = drv->get(conn, op.key, op.klen, st->got, st->cfg.block_size + 64, &got);
            rc = pace_kvcache_result(st, &r, g, st->got, got);
        } else {
            rc = pace_kvcache_result(st, &r, drv->put(conn, op.key, op.klen, op.val, op.vlen), NULL, 0);
        }
    }
    out->requests += r.stats.requests;
    out->block_gets += r.stats.block_gets;
    out->block_hits += r.stats.block_hits;
    out->block_puts += r.stats.block_puts;
    out->bytes_get += r.stats.bytes_get;
    out->bytes_put += r.stats.bytes_put;
    out->corrupt += r.stats.corrupt;
    return rc;
}
