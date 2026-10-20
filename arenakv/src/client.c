/* client.c - synchronous client for the akvproto protocol, see akvclient.h. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "akvclient.h"

struct akvc {
    duct_ctx *ctx;
    duct_ep *ep;
    uint32_t next_id;
    uint8_t *req;
    size_t req_cap;
    struct duct_rx rx;
    int rx_held;
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
    c->req_cap = opts->max_msg;
    c->req = malloc(c->req_cap);
    if (rc == 0 && c->req == NULL) {
        rc = -ENOMEM;
    }
    if (rc != 0) {
        akvc_close(c);
        return rc;
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

/* Sends c->req[0, len) and waits for the response; the response message stays held in c->rx. */
static int exchange(akvc *c, size_t len) {
    if (c->rx_held) {
        duct_rx_release(c->ep, &c->rx);
        c->rx_held = 0;
    }
    int rc = duct_send(c->ep, c->req, len, 10000);
    if (rc != 0) {
        return rc;
    }
    rc = duct_recv(c->ep, &c->rx, 10000);
    if (rc != 0) {
        return rc;
    }
    c->rx_held = 1;
    return 0;
}

/* Builds a request of the given op in c->req and returns its length, or 0 when it does not fit. */
static size_t build(akvc *c, uint8_t op, const void *key, size_t klen, const void *val, size_t vlen) {
    if (AKVP_REQ_HDR + klen + vlen > c->req_cap || klen > 0xffff) {
        return 0;
    }
    struct akvp_req r = {.op = op, .klen = (uint16_t)klen, .reqid = c->next_id++, .vlen = (uint32_t)vlen};
    akvp_req_encode(c->req, &r);
    if (klen > 0) {
        memcpy(c->req + AKVP_REQ_HDR, key, klen);
    }
    if (vlen > 0) {
        memcpy(c->req + AKVP_REQ_HDR + klen, val, vlen);
    }
    return AKVP_REQ_HDR + klen + vlen;
}

/* Validates the held response against the last request id; returns the status or a negative error. */
static int status_of(akvc *c, struct akvp_resp *rs) {
    if (akvp_resp_decode(c->rx.data, c->rx.len, rs) != 0 || rs->reqid != c->next_id - 1) {
        return DUCT_EPROTO;
    }
    return rs->status;
}

int akvc_put(akvc *c, const void *key, size_t klen, const void *val, size_t vlen) {
    size_t n = build(c, AKVP_PUT, key, klen, val, vlen);
    if (n == 0) {
        return AKVP_TOOBIG;
    }
    int rc = exchange(c, n);
    if (rc != 0) {
        return rc;
    }
    struct akvp_resp rs;
    return status_of(c, &rs);
}

int akvc_get(akvc *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    size_t n = build(c, AKVP_GET, key, klen, NULL, 0);
    if (n == 0) {
        return AKVP_TOOBIG;
    }
    int rc = exchange(c, n);
    if (rc != 0) {
        return rc;
    }
    struct akvp_resp rs;
    rc = status_of(c, &rs);
    if (rc != AKVP_OK) {
        return rc;
    }
    *vlen = rs.vlen;
    if (rs.vlen > cap) {
        return AKVP_TOOBIG;
    }
    memcpy(buf, (const uint8_t *)c->rx.data + AKVP_RESP_HDR, rs.vlen);
    return AKVP_OK;
}

int akvc_del(akvc *c, const void *key, size_t klen) {
    size_t n = build(c, AKVP_DEL, key, klen, NULL, 0);
    if (n == 0) {
        return AKVP_TOOBIG;
    }
    int rc = exchange(c, n);
    if (rc != 0) {
        return rc;
    }
    struct akvp_resp rs;
    return status_of(c, &rs);
}

int akvc_stats(akvc *c, uint64_t out[AKVP_STATS_FIELDS]) {
    size_t n = build(c, AKVP_STATS, NULL, 0, NULL, 0);
    int rc = exchange(c, n);
    if (rc != 0) {
        return rc;
    }
    struct akvp_resp rs;
    rc = status_of(c, &rs);
    if (rc != AKVP_OK) {
        return rc;
    }
    if (rs.vlen != AKVP_STATS_BYTES) {
        return DUCT_EPROTO;
    }
    for (unsigned i = 0; i < AKVP_STATS_FIELDS; i++) {
        out[i] = akvp_get64((const uint8_t *)c->rx.data + AKVP_RESP_HDR + 8 * i);
    }
    return AKVP_OK;
}

int akvc_raw(akvc *c, const void *msg, size_t len, const uint8_t **resp, size_t *resp_len) {
    if (len > c->req_cap) {
        return -EMSGSIZE;
    }
    memcpy(c->req, msg, len);
    int rc = exchange(c, len);
    if (rc != 0) {
        return rc;
    }
    *resp = c->rx.data;
    *resp_len = c->rx.len;
    return 0;
}
