/*
 * server.c - key-value server, see akvd.h.
 *
 * Request handling (per connection thread):
 *   receive one message -> validate header and length -> execute -> send one response -> release the
 *   receive buffer. A request that fails validation is answered with AKVP_BADREQ and the connection stays
 *   open, unless the message is too short to carry a request identifier, in which case the connection is
 *   closed. Transport errors end the connection.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "akvproto.h"
#include "akvd.h"

struct conn {
    struct akvd *d;
    duct_ep *ep;
    pthread_t thread;
    struct conn *next;
    atomic_int done;
};

struct akvd {
    akvd_config cfg;
    akv *store;
    duct_ctx *ctx;
    duct_listener *listener;
    pthread_t accept_thread;
    atomic_int stopping;
    atomic_uint nconn;
    pthread_mutex_t conns_lock;
    struct conn *conns;
};

void akvd_config_default(akvd_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    akv_config_default(&cfg->store);
    cfg->transport = DUCT_TRANSPORT_AUTO;
    cfg->wait = DUCT_WAIT_POLL;
    cfg->port = "7600";
    cfg->max_msg = 64u * 1024u;
    cfg->recv_depth = 16;
    cfg->send_depth = 16;
}

/* Executes one request; returns 0 to continue, or a negative value to close the connection. */
static int handle(struct conn *c, const uint8_t *msg, size_t len, uint8_t *out, size_t out_cap) {
    size_t peer_cap = duct_ep_peer_max_msg(c->ep);
    if (out_cap > peer_cap) {
        out_cap = peer_cap; /* the peer cannot receive larger responses; oversize values answer AKVP_TOOBIG */
    }
    struct akvd *d = c->d;
    struct akvp_req rq;
    if (akvp_req_decode(msg, len, &rq) != 0) {
        return -1;
    }
    size_t payload = 0;
    uint8_t status = AKVP_OK;
    const uint8_t *key = msg + AKVP_REQ_HDR;
    const uint8_t *val = key + rq.klen;
    int valid = (size_t)AKVP_REQ_HDR + rq.klen + rq.vlen == len && rq.flags == 0;

    switch (rq.op) {
    case AKVP_GET:
        if (!valid || rq.klen == 0 || rq.vlen != 0) {
            status = AKVP_BADREQ;
            break;
        }
        {
            size_t vlen = 0;
            int rc = akv_get(d->store, key, rq.klen, out + AKVP_RESP_HDR, out_cap - AKVP_RESP_HDR, &vlen);
            if (rc == 0) {
                payload = vlen;
            } else {
                status = rc == -ENOENT ? AKVP_NOTFOUND : (rc == -EMSGSIZE ? AKVP_TOOBIG : AKVP_ERROR);
            }
        }
        break;
    case AKVP_PUT:
        if (!valid || rq.klen == 0) {
            status = AKVP_BADREQ;
            break;
        }
        {
            int rc = akv_put(d->store, key, rq.klen, val, rq.vlen);
            status =
                rc == 0 ? AKVP_OK : (rc == -E2BIG ? AKVP_TOOBIG : (rc == -EINVAL ? AKVP_BADREQ : AKVP_ERROR));
        }
        break;
    case AKVP_DEL:
        if (!valid || rq.klen == 0 || rq.vlen != 0) {
            status = AKVP_BADREQ;
            break;
        }
        status = akv_del(d->store, key, rq.klen) == 0 ? AKVP_OK : AKVP_NOTFOUND;
        break;
    case AKVP_STATS:
        if (!valid || rq.klen != 0 || rq.vlen != 0) {
            status = AKVP_BADREQ;
            break;
        }
        {
            akv_stats st;
            akv_stats_get(d->store, &st);
            const uint64_t fields[AKVP_STATS_FIELDS] = {
                st.entries,   st.arena_bytes,  st.log_bytes,       st.gets,
                st.hits,      st.puts,         st.overwrites,      st.dels,
                st.evictions, st.reinsertions, st.index_evictions, st.rejected};
            for (unsigned i = 0; i < AKVP_STATS_FIELDS; i++) {
                akvp_put64(out + AKVP_RESP_HDR + 8 * i, fields[i]);
            }
            payload = AKVP_STATS_BYTES;
        }
        break;
    default:
        status = AKVP_BADREQ;
        break;
    }
    struct akvp_resp rs = {.status = status, .reqid = rq.reqid, .vlen = (uint32_t)payload};
    akvp_resp_encode(out, &rs);
    return duct_send(c->ep, out, AKVP_RESP_HDR + payload, 10000) == 0 ? 0 : -1;
}

static void *conn_main(void *arg) {
    struct conn *c = arg;
    struct akvd *d = c->d;
    size_t out_cap = d->cfg.max_msg;
    uint8_t *out = malloc(out_cap);
    if (out != NULL) {
        for (;;) {
            struct duct_rx rx;
            int rc = duct_recv(c->ep, &rx, 500);
            if (rc == -ETIMEDOUT) {
                if (atomic_load(&d->stopping)) {
                    break;
                }
                continue;
            }
            if (rc != 0) {
                break;
            }
            int hr = handle(c, rx.data, rx.len, out, out_cap);
            duct_rx_release(c->ep, &rx);
            if (hr != 0) {
                break;
            }
        }
    }
    free(out);
    duct_ep_close(c->ep);
    c->ep = NULL;
    atomic_fetch_sub(&d->nconn, 1);
    atomic_store(&c->done, 1);
    return NULL;
}

/* Joins and frees finished connection threads. */
static void reap(struct akvd *d, int all) {
    pthread_mutex_lock(&d->conns_lock);
    struct conn **pp = &d->conns;
    while (*pp != NULL) {
        struct conn *c = *pp;
        if (all || atomic_load(&c->done)) {
            *pp = c->next;
            pthread_mutex_unlock(&d->conns_lock);
            pthread_join(c->thread, NULL);
            free(c);
            pthread_mutex_lock(&d->conns_lock);
            pp = &d->conns;
            continue;
        }
        pp = &c->next;
    }
    pthread_mutex_unlock(&d->conns_lock);
}

static void *accept_main(void *arg) {
    struct akvd *d = arg;
    while (!atomic_load(&d->stopping)) {
        duct_ep *ep = NULL;
        int rc = duct_accept(d->listener, 200, &ep);
        reap(d, 0);
        if (rc != 0) {
            continue;
        }
        struct conn *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            duct_ep_close(ep);
            continue;
        }
        c->d = d;
        c->ep = ep;
        atomic_fetch_add(&d->nconn, 1);
        if (pthread_create(&c->thread, NULL, conn_main, c) != 0) {
            atomic_fetch_sub(&d->nconn, 1);
            duct_ep_close(ep);
            free(c);
            continue;
        }
        pthread_mutex_lock(&d->conns_lock);
        c->next = d->conns;
        d->conns = c;
        pthread_mutex_unlock(&d->conns_lock);
    }
    return NULL;
}

akvd *akvd_start(const akvd_config *cfg, int *err) {
    int dummy;
    if (err == NULL) {
        err = &dummy;
    }
    *err = 0;
    struct akvd *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        *err = -ENOMEM;
        return NULL;
    }
    d->cfg = *cfg;
    if (d->cfg.max_msg == 0) {
        d->cfg.max_msg = 64u * 1024u;
    }
    if (d->cfg.recv_depth == 0) {
        d->cfg.recv_depth = 16;
    }
    if (d->cfg.send_depth == 0) {
        d->cfg.send_depth = 16;
    }
    pthread_mutex_init(&d->conns_lock, NULL);
    d->store = akv_create(&d->cfg.store);
    if (d->store == NULL) {
        *err = -EINVAL;
        goto fail;
    }
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = d->cfg.transport;
    o.wait = d->cfg.wait;
    o.max_msg = d->cfg.max_msg;
    o.recv_depth = d->cfg.recv_depth;
    o.send_depth = d->cfg.send_depth;
    if ((*err = duct_ctx_create(&o, &d->ctx)) != 0) {
        goto fail;
    }
    if ((*err = duct_listen(d->ctx, d->cfg.addr, d->cfg.port, &d->listener)) != 0) {
        goto fail;
    }
    if (pthread_create(&d->accept_thread, NULL, accept_main, d) != 0) {
        *err = -EAGAIN;
        goto fail;
    }
    return d;
fail:
    if (d->listener != NULL) {
        duct_listener_close(d->listener);
    }
    duct_ctx_destroy(d->ctx);
    akv_destroy(d->store);
    pthread_mutex_destroy(&d->conns_lock);
    free(d);
    return NULL;
}

int akvd_port(const akvd *d) {
    return duct_listener_port(d->listener);
}

akv *akvd_store(akvd *d) {
    return d->store;
}

unsigned akvd_connections(const akvd *d) {
    return atomic_load(&d->nconn);
}

void akvd_stop(akvd *d) {
    if (d == NULL) {
        return;
    }
    atomic_store(&d->stopping, 1);
    pthread_join(d->accept_thread, NULL);
    reap(d, 1);
    duct_listener_close(d->listener);
    duct_ctx_destroy(d->ctx);
    akv_destroy(d->store);
    pthread_mutex_destroy(&d->conns_lock);
    free(d);
}
