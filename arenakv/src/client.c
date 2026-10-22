/* client.c - client for the native arenakv protocol, see akvclient.h. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "akvclient.h"
#include "hash.h"

/* Requests staged by akvc_post() before one duct_send_batch() (see akvc_push). */
#define AKVC_BATCH 32

struct akvc {
    duct_ctx *ctx;
    duct_ep *ep;
    uint32_t next_id;
    uint8_t *req;   /* staging area of 2 * req_cap bytes; posted requests are packed in order */
    size_t req_cap; /* largest request (max_msg) */
    size_t staged_bytes;
    unsigned nstaged;
    struct duct_msg staged[AKVC_BATCH];
    struct duct_rx rx;
    int rx_held;
    unsigned outstanding;
    unsigned window;
};

int akvc_connect(akvc **out, const struct duct_opts *opts, const char *host, const char *port) {
    akvc *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return -ENOMEM;
    }
    int rc = duct_ctx_create(opts, &c->ctx);
    if (rc == 0) {
        rc = duct_connect(c->ctx, host, port, &c->ep);
    }
    /* Requests are bounded by what the server accepts (less the encryption overhead on encrypted endpoints). */
    c->req_cap = opts->max_msg;
    if (rc == 0 && duct_ep_peer_max_msg(c->ep) < c->req_cap) {
        c->req_cap = duct_ep_peer_max_msg(c->ep);
    }
    c->req = malloc(2 * c->req_cap);
    if (rc == 0 && c->req == NULL) {
        rc = -ENOMEM;
    }
    if (rc != 0) {
        akvc_close(c);
        return rc;
    }
    /* Responses are held one at a time; requests in flight are bounded by both directions' buffers. */
    c->window = opts->recv_depth < opts->send_depth ? opts->recv_depth : opts->send_depth;
    if (c->window > 1) {
        c->window--;
    }
    c->next_id = 1;
    *out = c;
    return 0;
}

void akvc_close(akvc *c) {
    if (c == NULL) {
        return;
    }
    if (c->rx_held) {
        duct_rx_release(c->ep, &c->rx);
    }
    duct_ep_close(c->ep);
    duct_ctx_destroy(c->ctx);
    free(c->req);
    free(c);
}

unsigned akvc_window(const akvc *c) {
    return c->window;
}

unsigned akvc_outstanding(const akvc *c) {
    return c->outstanding;
}

int akvc_fd(const akvc *c) {
    return duct_ep_fd(c->ep);
}

int akvc_direct_fd(akvc *c) {
    return duct_ep_direct_fd(c->ep);
}

uint32_t akvc_wait_events(const akvc *c) {
    return duct_ep_wait_events(c->ep);
}

int akvc_push(akvc *c) {
    if (c->nstaged == 0) {
        return 0;
    }
    int rc = duct_send_batch(c->ep, c->staged, c->nstaged, 10000);
    c->nstaged = 0;
    c->staged_bytes = 0;
    return rc;
}

int akvc_arm(akvc *c) {
    int rc = akvc_push(c);
    return rc != 0 ? rc : duct_ep_arm(c->ep);
}

/* Builds a request at dst (room for c->req_cap bytes) and returns its length, or 0 when it does not fit. */
static size_t build(akvc *c, uint8_t *dst, uint8_t op, const void *key, size_t klen, const void *val,
                    size_t vlen, const struct akvp_ext *ext) {
    size_t x = ext != NULL ? AKVP_EXT : 0;
    if (AKVP_REQ_HDR + x + klen + vlen > c->req_cap || klen > 0xffff || vlen > UINT32_MAX) {
        return 0;
    }
    struct akvp_req r = {.op = op,
                         .flags = ext != NULL ? AKVP_F_EXT : 0,
                         .klen = (uint16_t)klen,
                         .reqid = c->next_id++,
                         .vlen = (uint32_t)vlen};
    akvp_req_encode(dst, &r);
    if (ext != NULL) {
        akvp_ext_encode(dst + AKVP_REQ_HDR, ext);
    }
    if (klen > 0) {
        memcpy(dst + AKVP_REQ_HDR + x, key, klen);
    }
    if (vlen > 0) {
        memcpy(dst + AKVP_REQ_HDR + x + klen, val, vlen);
    }
    return AKVP_REQ_HDR + x + klen + vlen;
}

int akvc_post(akvc *c, uint8_t op, const void *key, size_t klen, const void *val, size_t vlen,
              const struct akvp_ext *ext, uint32_t *reqid) {
    if (c->outstanding >= c->window) {
        return -EBUSY;
    }
    /* The staging area holds 2 * req_cap bytes: push first when the next request might not fit. */
    if (c->nstaged == AKVC_BATCH || c->staged_bytes > c->req_cap) {
        int rc = akvc_push(c);
        if (rc != 0) {
            return rc;
        }
    }
    uint8_t *dst = c->req + c->staged_bytes;
    size_t n = build(c, dst, op, key, klen, val, vlen, ext);
    if (n == 0) {
        return AKVP_TOOBIG;
    }
    c->staged[c->nstaged++] = (struct duct_msg){.data = dst, .len = n};
    c->staged_bytes += n;
    c->outstanding++;
    if (reqid != NULL) {
        *reqid = c->next_id - 1;
    }
    return 0;
}

int akvc_next(akvc *c, struct akvc_resp *r, int timeout_ms) {
    if (c->outstanding == 0) {
        return -EINVAL;
    }
    if (c->rx_held) {
        duct_rx_release(c->ep, &c->rx);
        c->rx_held = 0;
    }
    int rc = akvc_push(c);
    if (rc != 0) {
        return rc;
    }
    rc = duct_recv(c->ep, &c->rx, timeout_ms);
    if (rc != 0) {
        return rc;
    }
    c->rx_held = 1;
    c->outstanding--;
    struct akvp_resp rs;
    if (akvp_resp_decode(c->rx.data, c->rx.len, &rs) != 0) {
        return DUCT_EPROTO;
    }
    r->reqid = rs.reqid;
    r->status = rs.status;
    r->payload = (const uint8_t *)c->rx.data + AKVP_RESP_HDR;
    r->len = rs.vlen;
    return 0;
}

/* Sends one request and waits for its response. Returns the status or a negative error. */
static int call(akvc *c, uint8_t op, const void *key, size_t klen, const void *val, size_t vlen,
                const struct akvp_ext *ext, struct akvc_resp *r) {
    while (c->outstanding > 0) { /* collect responses of earlier pipelined requests first */
        struct akvc_resp skip;
        int rc = akvc_next(c, &skip, 10000);
        if (rc != 0) {
            return rc;
        }
    }
    uint32_t id;
    int rc = akvc_post(c, op, key, klen, val, vlen, ext, &id);
    if (rc != 0) {
        return rc;
    }
    rc = akvc_next(c, r, 10000);
    if (rc != 0) {
        return rc;
    }
    return r->reqid == id ? r->status : DUCT_EPROTO;
}

int akvc_auth(akvc *c, const char *credentials) {
    struct akvc_resp r;
    return call(c, AKVP_AUTH, NULL, 0, credentials, strlen(credentials), NULL, &r);
}

int akvc_store(akvc *c, uint8_t op, const void *key, size_t klen, const void *val, size_t vlen,
               const struct akvp_ext *ext, uint64_t *cas_out) {
    struct akvc_resp r;
    int rc = call(c, op, key, klen, val, vlen, ext, &r);
    if (rc == AKVP_OK && cas_out != NULL) {
        *cas_out = r.len == 8 ? akvp_get64(r.payload) : 0;
    }
    return rc;
}

int akvc_put(akvc *c, const void *key, size_t klen, const void *val, size_t vlen) {
    return akvc_store(c, AKVP_PUT, key, klen, val, vlen, NULL, NULL);
}

int akvc_get(akvc *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    struct akvc_resp r;
    int rc = call(c, AKVP_GET, key, klen, NULL, 0, NULL, &r);
    if (rc != AKVP_OK) {
        return rc;
    }
    *vlen = r.len;
    if (r.len > cap) {
        return AKVP_TOOBIG;
    }
    memcpy(buf, r.payload, r.len);
    return AKVP_OK;
}

int akvc_get_meta(akvc *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen, uint32_t *flags,
                  uint64_t *cas, int64_t *ttl_ms, int64_t touch_ttl_ms) {
    struct akvp_ext x = {.ttl_ms = touch_ttl_ms};
    struct akvc_resp r;
    int rc = call(c, AKVP_GETM, key, klen, NULL, 0, &x, &r);
    if (rc != AKVP_OK) {
        return rc;
    }
    if (r.len < AKVP_META) {
        return DUCT_EPROTO;
    }
    if (flags != NULL) {
        *flags = akvp_get32(r.payload);
    }
    if (cas != NULL) {
        *cas = akvp_get64(r.payload + 8);
    }
    if (ttl_ms != NULL) {
        *ttl_ms = (int64_t)akvp_get64(r.payload + 16);
    }
    *vlen = r.len - AKVP_META;
    if (*vlen > cap) {
        return AKVP_TOOBIG;
    }
    memcpy(buf, r.payload + AKVP_META, *vlen);
    return AKVP_OK;
}

int akvc_del(akvc *c, const void *key, size_t klen) {
    struct akvc_resp r;
    return call(c, AKVP_DEL, key, klen, NULL, 0, NULL, &r);
}

int akvc_exists(akvc *c, const void *key, size_t klen) {
    struct akvc_resp r;
    return call(c, AKVP_HAS, key, klen, NULL, 0, NULL, &r);
}

int akvc_touch(akvc *c, const void *key, size_t klen, int64_t ttl_ms) {
    struct akvp_ext x = {.ttl_ms = ttl_ms};
    struct akvc_resp r;
    return call(c, AKVP_TOUCH, key, klen, NULL, 0, &x, &r);
}

int akvc_incr(akvc *c, const void *key, size_t klen, int64_t delta, int is_signed, int64_t *result) {
    uint8_t d[8];
    akvp_put64(d, (uint64_t)delta);
    struct akvc_resp r;
    int rc = call(c, is_signed ? AKVP_INCRS : AKVP_INCR, key, klen, d, 8, NULL, &r);
    if (rc == AKVP_OK) {
        if (r.len != 8) {
            return DUCT_EPROTO;
        }
        *result = (int64_t)akvp_get64(r.payload);
    }
    return rc;
}

int akvc_flush(akvc *c) {
    struct akvc_resp r;
    return call(c, AKVP_FLUSH, NULL, 0, NULL, 0, NULL, &r);
}

int akvc_role(akvc *c, int *replica) {
    struct akvc_resp r;
    int rc = call(c, AKVP_ROLE, NULL, 0, NULL, 0, NULL, &r);
    if (rc == AKVP_OK) {
        if (r.len < 1) {
            return AKVP_ERROR;
        }
        *replica = r.payload[0] != 0;
    }
    return rc;
}

int akvc_stats(akvc *c, uint64_t out[AKVP_STATS_FIELDS]) {
    struct akvc_resp r;
    int rc = call(c, AKVP_STATS, NULL, 0, NULL, 0, NULL, &r);
    if (rc != AKVP_OK) {
        return rc;
    }
    if (r.len != AKVP_STATS_BYTES) {
        return DUCT_EPROTO;
    }
    for (unsigned i = 0; i < AKVP_STATS_FIELDS; i++) {
        out[i] = akvp_get64(r.payload + 8 * i);
    }
    return AKVP_OK;
}

int akvc_raw(akvc *c, const void *msg, size_t len, const uint8_t **resp, size_t *resp_len) {
    if (len > c->req_cap) {
        return -EMSGSIZE;
    }
    while (c->outstanding > 0) {
        struct akvc_resp skip;
        int rc = akvc_next(c, &skip, 10000);
        if (rc != 0) {
            return rc;
        }
    }
    if (c->rx_held) {
        duct_rx_release(c->ep, &c->rx);
        c->rx_held = 0;
    }
    int rc = duct_send(c->ep, msg, len, 10000);
    if (rc != 0) {
        return rc;
    }
    rc = duct_recv(c->ep, &c->rx, 10000);
    if (rc != 0) {
        return rc;
    }
    c->rx_held = 1;
    *resp = c->rx.data;
    *resp_len = c->rx.len;
    return 0;
}

/* ---- pool ------------------------------------------------------------------------------------------------ */

#define POOL_POINTS 160u
#define POOL_SEED 0x6b6574616d61ull /* fixed: placement must not change between processes */

struct point {
    uint64_t pos;
    unsigned server;
};

/* One ring entry: its servers ("host:port" each), the client of its primary, and a replica client. */
struct shard {
    char **addrs;
    unsigned naddrs;
    akvc *cur;
    unsigned cur_idx;
    akvc *reader;
    int reader_tried;
};

struct akvc_pool {
    struct duct_opts opts;
    struct shard *shards;
    unsigned n;
    struct point *ring;
    size_t npoints;
};

/* Connects to "host:port". Returns 0 or an error. */
static int connect_addr(const struct duct_opts *o, const char *addr, akvc **out) {
    char host[256];
    const char *colon = strrchr(addr, ':');
    if (colon == NULL || (size_t)(colon - addr) >= sizeof(host)) {
        return -EINVAL;
    }
    memcpy(host, addr, (size_t)(colon - addr));
    host[colon - addr] = '\0';
    return akvc_connect(out, o, host, colon + 1);
}

/*
 * Connects s->cur to the server of the entry that reports itself as primary, skipping `skip` (an index,
 * or UINT_MAX). With `any`, the first reachable server is taken when none is primary. Returns 0, or
 * -ENOTCONN when no server qualifies.
 */
static int find_primary(const struct duct_opts *o, struct shard *s, unsigned skip, int any) {
    akvc *fallback = NULL;
    unsigned fallback_idx = 0;
    for (unsigned i = 0; i < s->naddrs; i++) {
        if (i == skip) {
            continue;
        }
        akvc *c;
        if (connect_addr(o, s->addrs[i], &c) != 0) {
            continue;
        }
        int replica = 1;
        if (akvc_role(c, &replica) == AKVP_OK && !replica) {
            akvc_close(fallback);
            s->cur = c;
            s->cur_idx = i;
            return 0;
        }
        if (any && fallback == NULL) {
            fallback = c;
            fallback_idx = i;
        } else {
            akvc_close(c);
        }
    }
    if (fallback != NULL) {
        s->cur = fallback;
        s->cur_idx = fallback_idx;
        return 0;
    }
    return -ENOTCONN;
}

static int cmp_point(const void *a, const void *b) {
    const struct point *x = a;
    const struct point *y = b;
    return x->pos < y->pos ? -1 : x->pos > y->pos ? 1 : (int)x->server - (int)y->server;
}

int akvc_pool_create(akvc_pool **out, const struct duct_opts *opts, const char *const *servers, unsigned n) {
    if (n == 0) {
        return -EINVAL;
    }
    akvc_pool *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return -ENOMEM;
    }
    p->opts = *opts;
    p->shards = calloc(n, sizeof(*p->shards));
    p->ring = calloc((size_t)n * POOL_POINTS, sizeof(*p->ring));
    if (p->shards == NULL || p->ring == NULL) {
        akvc_pool_close(p);
        return -ENOMEM;
    }
    p->n = n;
    for (unsigned i = 0; i < n; i++) {
        struct shard *s = &p->shards[i];
        for (const char *a = servers[i]; *a != '\0';) {
            size_t len = strcspn(a, ",");
            char **na = realloc(s->addrs, (s->naddrs + 1) * sizeof(*na));
            char *copy = len > 0 ? malloc(len + 1) : NULL;
            if (na == NULL || (len > 0 && copy == NULL)) {
                free(copy);
                if (na != NULL) {
                    s->addrs = na;
                }
                akvc_pool_close(p);
                return -ENOMEM;
            }
            s->addrs = na;
            if (len > 0) {
                memcpy(copy, a, len);
                copy[len] = '\0';
                s->addrs[s->naddrs++] = copy;
            }
            a += len + (a[len] == ',');
        }
        if (s->naddrs == 0) {
            akvc_pool_close(p);
            return -EINVAL;
        }
        int rc =
            s->naddrs == 1 ? connect_addr(opts, s->addrs[0], &s->cur) : find_primary(opts, s, UINT_MAX, 1);
        if (rc != 0) {
            akvc_pool_close(p);
            return rc;
        }
        /* Points depend on the entry's text only, so every client computes the same ring. */
        for (unsigned k = 0; k < POOL_POINTS; k++) {
            char label[1100];
            int len = snprintf(label, sizeof(label), "%s-%u", servers[i], k);
            p->ring[p->npoints++] =
                (struct point){.pos = akv_hash64(label, (size_t)len, POOL_SEED), .server = i};
        }
    }
    qsort(p->ring, p->npoints, sizeof(*p->ring), cmp_point);
    *out = p;
    return 0;
}

unsigned akvc_pool_index(const akvc_pool *p, const void *key, size_t klen) {
    uint64_t h = akv_hash64(key, klen, POOL_SEED);
    size_t lo = 0;
    size_t hi = p->npoints;
    while (lo < hi) { /* first point at or after h, wrapping to the start of the ring */
        size_t mid = lo + (hi - lo) / 2;
        if (p->ring[mid].pos < h) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return p->ring[lo == p->npoints ? 0 : lo].server;
}

akvc *akvc_pool_pick(akvc_pool *p, const void *key, size_t klen) {
    return p->shards[akvc_pool_index(p, key, klen)].cur;
}

akvc *akvc_pool_pick_read(akvc_pool *p, const void *key, size_t klen) {
    struct shard *s = &p->shards[akvc_pool_index(p, key, klen)];
    if (s->reader == NULL && !s->reader_tried) {
        s->reader_tried = 1;
        for (unsigned i = 0; i < s->naddrs && s->reader == NULL; i++) {
            akvc *c;
            if (i == s->cur_idx || connect_addr(&p->opts, s->addrs[i], &c) != 0) {
                continue;
            }
            int replica = 0;
            if (akvc_role(c, &replica) == AKVP_OK && replica) {
                s->reader = c;
            } else {
                akvc_close(c);
            }
        }
    }
    return s->reader != NULL ? s->reader : s->cur;
}

int akvc_pool_reconnect(akvc_pool *p, const void *key, size_t klen) {
    struct shard *s = &p->shards[akvc_pool_index(p, key, klen)];
    akvc_close(s->cur);
    s->cur = NULL;
    akvc_close(s->reader); /* a promoted replica stops being one */
    s->reader = NULL;
    s->reader_tried = 0;
    return find_primary(&p->opts, s, UINT_MAX, 0);
}

void akvc_pool_close(akvc_pool *p) {
    if (p == NULL) {
        return;
    }
    for (unsigned i = 0; p->shards != NULL && i < p->n; i++) {
        struct shard *s = &p->shards[i];
        akvc_close(s->cur);
        akvc_close(s->reader);
        for (unsigned k = 0; k < s->naddrs; k++) {
            free(s->addrs[k]);
        }
        free(s->addrs);
    }
    free(p->shards);
    free(p->ring);
    free(p);
}
