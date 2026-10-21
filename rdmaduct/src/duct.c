/* duct.c - context management, region registry, argument validation and transport dispatch. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

void duct_opts_default(struct duct_opts *o) {
    o->transport = DUCT_TRANSPORT_AUTO;
    o->max_msg = 64u * 1024u;
    o->recv_depth = 32;
    o->send_depth = 32;
    o->connect_timeout_ms = 10000;
    o->wait = DUCT_WAIT_POLL;
}

const char *duct_strerror(int err) {
    switch (err) {
    case 0:
        return "success";
    case DUCT_ECLOSED:
        return "connection closed by peer";
    case DUCT_EPROTO:
        return "protocol violation";
    case DUCT_EWC:
        return "RDMA work completion error";
    case DUCT_ENODEV:
        return "no usable RDMA device";
    case DUCT_EACCESS:
        return "remote memory access violation";
    default:
        return err < 0 && err > -1000 ? strerror(-err) : "unknown error";
    }
}

int duct_ctx_create(const struct duct_opts *o, duct_ctx **out) {
    if (o->max_msg == 0 || o->max_msg > DUCT_LIMIT_MAX_MSG || o->recv_depth == 0 ||
        o->recv_depth > DUCT_LIMIT_DEPTH || o->send_depth == 0 || o->send_depth > DUCT_LIMIT_DEPTH) {
        return -EINVAL;
    }
    duct_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -ENOMEM;
    }
    ctx->opts = *o;
    ctx->transport = o->transport;
    pthread_mutex_init(&ctx->mr_lock, NULL);
    ctx->next_key = 1;
    if (o->transport == DUCT_TRANSPORT_RDMA && o->recv_depth < 2) {
        pthread_mutex_destroy(&ctx->mr_lock);
        free(ctx);
        return -EINVAL;
    }
#ifdef DUCT_HAVE_RDMA
    if (ctx->transport == DUCT_TRANSPORT_AUTO) {
        ctx->transport = duct_rdma_available() ? DUCT_TRANSPORT_RDMA : DUCT_TRANSPORT_TCP;
    }
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
        int rc = duct_rdma_ctx_init(ctx);
        if (rc != 0) {
            pthread_mutex_destroy(&ctx->mr_lock);
            free(ctx);
            return rc;
        }
    }
#else
    if (ctx->transport == DUCT_TRANSPORT_AUTO) {
        ctx->transport = DUCT_TRANSPORT_TCP;
    }
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
        pthread_mutex_destroy(&ctx->mr_lock);
        free(ctx);
        return DUCT_ENODEV;
    }
#endif
    *out = ctx;
    return 0;
}

void duct_ctx_destroy(duct_ctx *ctx) {
    if (ctx == NULL) {
        return;
    }
#ifdef DUCT_HAVE_RDMA
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
        duct_rdma_ctx_fini(ctx);
    }
#endif
    pthread_mutex_destroy(&ctx->mr_lock);
    free(ctx);
}

enum duct_transport duct_ctx_transport(const duct_ctx *ctx) {
    return ctx->transport;
}

int duct_listen(duct_ctx *ctx, const char *addr, const char *port, duct_listener **out) {
#ifdef DUCT_HAVE_RDMA
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
        return duct_rdma_listen(ctx, addr, port, out);
    }
#endif
    return duct_tcp_listen(ctx, addr, port, out);
}

int duct_connect(duct_ctx *ctx, const char *addr, const char *port, duct_ep **out) {
#ifdef DUCT_HAVE_RDMA
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
        return duct_rdma_connect(ctx, addr, port, out);
    }
#endif
    return duct_tcp_connect(ctx, addr, port, out);
}

int duct_listener_port(const duct_listener *l) {
    return l->ops->port(l);
}

int duct_accept(duct_listener *l, int timeout_ms, duct_ep **out) {
    return l->ops->accept(l, timeout_ms, out);
}

void duct_listener_close(duct_listener *l) {
    if (l != NULL) {
        l->ops->close(l);
    }
}

void duct_ep_close(duct_ep *ep) {
    if (ep != NULL) {
        ep->ops->close(ep);
    }
}

uint32_t duct_ep_peer_max_msg(const duct_ep *ep) {
    return ep->peer_max_msg;
}

int duct_send(duct_ep *ep, const void *data, size_t len, int timeout_ms) {
    if (len > ep->peer_max_msg) {
        return -EMSGSIZE;
    }
    return ep->ops->send(ep, data, len, timeout_ms);
}

int duct_send_batch(duct_ep *ep, const struct duct_msg *msgs, unsigned n, int timeout_ms) {
    for (unsigned i = 0; i < n; i++) {
        if (msgs[i].len > ep->peer_max_msg) {
            return -EMSGSIZE;
        }
    }
    return n == 0 ? 0 : ep->ops->send_batch(ep, msgs, n, timeout_ms);
}

int duct_send_some(duct_ep *ep, const struct duct_msg *msgs, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (msgs[i].len > ep->peer_max_msg) {
            return -EMSGSIZE;
        }
    }
    return n == 0 ? 0 : ep->ops->send_some(ep, msgs, n);
}

int duct_recv(duct_ep *ep, struct duct_rx *rx, int timeout_ms) {
    return ep->ops->recv(ep, rx, timeout_ms);
}

int duct_rx_release(duct_ep *ep, struct duct_rx *rx) {
    return ep->ops->release(ep, rx);
}

/* ---- event-loop integration ------------------------------------------------------------------------ */

int duct_ep_fd(const duct_ep *ep) {
    return ep->ops->fd(ep);
}

int duct_ep_arm(duct_ep *ep) {
    return ep->ops->arm(ep);
}

int duct_ep_direct_fd(duct_ep *ep) {
    return ep->ops->direct_fd(ep);
}

uint32_t duct_ep_wait_events(const duct_ep *ep) {
    return ep->ops->wait_events(ep);
}

int duct_progress(duct_ep *ep) {
    return ep->ops->progress(ep);
}

unsigned duct_ep_send_space(duct_ep *ep) {
    return ep->ops->send_space(ep);
}

int duct_listener_fd(const duct_listener *l) {
    return l->ops->fd(l);
}

/* ---- registered memory ----------------------------------------------------------------------------- */

int duct_mr_reg(duct_ctx *ctx, void *addr, size_t len, unsigned access, duct_mr **out) {
    unsigned all = DUCT_ACCESS_REMOTE_READ | DUCT_ACCESS_REMOTE_WRITE | DUCT_ACCESS_REMOTE_ATOMIC;
    if (addr == NULL || len == 0 || (access & ~all) != 0) {
        return -EINVAL;
    }
    struct duct_mr *mr = calloc(1, sizeof(*mr));
    if (mr == NULL) {
        return -ENOMEM;
    }
    mr->ctx = ctx;
    mr->addr = addr;
    mr->len = len;
    mr->access = access;
#ifdef DUCT_HAVE_RDMA
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
        int rc = duct_rdma_mr_reg(ctx, mr);
        if (rc != 0) {
            free(mr);
            return rc;
        }
    }
#endif
    pthread_mutex_lock(&ctx->mr_lock);
    if (ctx->transport != DUCT_TRANSPORT_RDMA) {
        /* Registry keys are never reused within a context, so a stale descriptor cannot match a new region. */
        mr->key = ctx->next_key++;
    }
    mr->next = ctx->mrs;
    ctx->mrs = mr;
    pthread_mutex_unlock(&ctx->mr_lock);
    *out = mr;
    return 0;
}

void duct_mr_dereg(duct_mr *mr) {
    if (mr == NULL) {
        return;
    }
    duct_ctx *ctx = mr->ctx;
    pthread_mutex_lock(&ctx->mr_lock);
    for (struct duct_mr **pp = &ctx->mrs; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == mr) {
            *pp = mr->next;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->mr_lock);
#ifdef DUCT_HAVE_RDMA
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
        duct_rdma_mr_dereg(mr);
    }
#endif
    free(mr);
}

void *duct_mr_addr(const duct_mr *mr) {
    return mr->addr;
}

void duct_mr_export(const duct_mr *mr, struct duct_mr_desc *out) {
    struct duct_desc d = {.access = mr->access, .addr = (uintptr_t)mr->addr, .len = mr->len, .key = mr->key};
    duct_desc_encode(&d, out);
}

uint8_t *duct_mr_resolve(duct_ctx *ctx, uint32_t key, uint64_t addr, uint64_t len, unsigned need) {
    uint8_t *p = NULL;
    pthread_mutex_lock(&ctx->mr_lock);
    for (struct duct_mr *mr = ctx->mrs; mr != NULL; mr = mr->next) {
        if (mr->key != key) {
            continue;
        }
        uint64_t base = (uintptr_t)mr->addr;
        if ((mr->access & need) == need && addr >= base && len <= mr->len && addr - base <= mr->len - len) {
            p = mr->addr + (addr - base);
        }
        break;
    }
    pthread_mutex_unlock(&ctx->mr_lock);
    return p;
}

/* ---- posted operations ----------------------------------------------------------------------------- */

int duct_send_mr(duct_ep *ep, const duct_mr *mr, size_t off, size_t len, int timeout_ms, uint64_t *seq) {
    if (mr->ctx != ep->ctx || off > mr->len || len > mr->len - off) {
        return -EINVAL;
    }
    if (len > ep->peer_max_msg) {
        return -EMSGSIZE;
    }
    return ep->ops->send_mr(ep, mr, off, len, timeout_ms, seq);
}

/*
 * Validates a one-sided operation against the local region and the remote descriptor and fills op.
 * need: right the remote region must grant. Atomic operations require 8-byte alignment on both sides.
 */
static int rma_prepare(duct_ep *ep, const duct_mr *local, size_t loff, const struct duct_mr_desc *remote,
                       uint64_t roff, size_t len, unsigned need, int atomic, struct duct_rma *op) {
    struct duct_desc d;
    if (local->ctx != ep->ctx || duct_desc_decode(remote, &d) != 0) {
        return -EINVAL;
    }
    if (len == 0 || len > DUCT_LIMIT_MAX_MSG || loff > local->len || len > local->len - loff ||
        roff > d.len || len > d.len - roff) {
        return -EINVAL;
    }
    if ((d.access & need) != need) {
        return -EINVAL;
    }
    op->laddr = local->addr + loff;
    op->lkey = local->lkey;
    op->len = len;
    op->raddr = d.addr + roff;
    op->rkey = d.key;
    if (atomic && (((uintptr_t)op->laddr | op->raddr) & 7u) != 0) {
        return -EINVAL;
    }
    return 0;
}

int duct_read(duct_ep *ep, duct_mr *local, size_t loff, const struct duct_mr_desc *remote, uint64_t roff,
              size_t len, int timeout_ms, uint64_t *seq) {
    struct duct_rma op = {.op = DUCT_RMA_READ};
    int rc = rma_prepare(ep, local, loff, remote, roff, len, DUCT_ACCESS_REMOTE_READ, 0, &op);
    return rc != 0 ? rc : ep->ops->rma(ep, &op, timeout_ms, seq);
}

int duct_write(duct_ep *ep, const duct_mr *local, size_t loff, const struct duct_mr_desc *remote,
               uint64_t roff, size_t len, int timeout_ms, uint64_t *seq) {
    struct duct_rma op = {.op = DUCT_RMA_WRITE};
    int rc = rma_prepare(ep, local, loff, remote, roff, len, DUCT_ACCESS_REMOTE_WRITE, 0, &op);
    return rc != 0 ? rc : ep->ops->rma(ep, &op, timeout_ms, seq);
}

int duct_atomic_fadd(duct_ep *ep, duct_mr *local, size_t loff, const struct duct_mr_desc *remote,
                     uint64_t roff, uint64_t add, int timeout_ms, uint64_t *seq) {
    struct duct_rma op = {.op = DUCT_RMA_FADD, .a = add};
    int rc = rma_prepare(ep, local, loff, remote, roff, 8, DUCT_ACCESS_REMOTE_ATOMIC, 1, &op);
    return rc != 0 ? rc : ep->ops->rma(ep, &op, timeout_ms, seq);
}

int duct_atomic_cswap(duct_ep *ep, duct_mr *local, size_t loff, const struct duct_mr_desc *remote,
                      uint64_t roff, uint64_t compare, uint64_t swap, int timeout_ms, uint64_t *seq) {
    struct duct_rma op = {.op = DUCT_RMA_CSWAP, .a = compare, .b = swap};
    int rc = rma_prepare(ep, local, loff, remote, roff, 8, DUCT_ACCESS_REMOTE_ATOMIC, 1, &op);
    return rc != 0 ? rc : ep->ops->rma(ep, &op, timeout_ms, seq);
}

uint64_t duct_ep_completed(duct_ep *ep) {
    return ep->ops->completed(ep);
}

int duct_wait(duct_ep *ep, uint64_t seq, int timeout_ms) {
    return ep->ops->wait(ep, seq, timeout_ms);
}
