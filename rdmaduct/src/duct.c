/* duct.c - context management and transport dispatch. */
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
    if (o->transport == DUCT_TRANSPORT_RDMA && o->recv_depth < 2) {
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
            free(ctx);
            return rc;
        }
    }
#else
    if (ctx->transport == DUCT_TRANSPORT_AUTO) {
        ctx->transport = DUCT_TRANSPORT_TCP;
    }
    if (ctx->transport == DUCT_TRANSPORT_RDMA) {
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

int duct_recv(duct_ep *ep, struct duct_rx *rx, int timeout_ms) {
    return ep->ops->recv(ep, rx, timeout_ms);
}

int duct_rx_release(duct_ep *ep, struct duct_rx *rx) {
    return ep->ops->release(ep, rx);
}
