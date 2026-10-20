/*
 * tcp.c - TCP transport.
 *
 * Sockets are non-blocking; all waits use poll() with the caller's deadline.
 * Receive state persists across calls, so a duct_recv() that times out in the
 * middle of a frame resumes at the same byte on the next call. A send that
 * times out after writing part of a frame leaves the stream in an undefined
 * state and marks the endpoint broken.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"

#define FRAME_HEADER 4u

struct tcp_ep {
    struct duct_ep base;
    int fd;
    uint32_t max_msg;
    uint8_t *rx_buf; /* FRAME_HEADER + max_msg bytes */
    uint8_t *tx_buf; /* FRAME_HEADER + peer_max_msg bytes */
    size_t rx_off;   /* bytes of the current frame received so far */
    int rx_busy;     /* a delivered message has not been released */
    int broken;      /* sticky error (negative code) */
};

struct tcp_listener {
    struct duct_listener base;
    int fd;
    int port;
};

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        return -errno;
    }
    return 0;
}

/* Waits for events on fd until the deadline. Returns 0 when ready, -ETIMEDOUT, or -errno. */
static int wait_fd(int fd, short events, int64_t deadline) {
    for (;;) {
        struct pollfd p = {.fd = fd, .events = events};
        int rc = poll(&p, 1, duct_remaining_ms(deadline));
        if (rc > 0) {
            return 0;
        }
        if (rc == 0) {
            return -ETIMEDOUT;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
}

/* Reads exactly n bytes into buf starting at *off. Updates *off; returns 0, -ETIMEDOUT, DUCT_ECLOSED or -errno. */
static int read_full(int fd, uint8_t *buf, size_t *off, size_t n, int64_t deadline) {
    while (*off < n) {
        ssize_t r = recv(fd, buf + *off, n - *off, 0);
        if (r > 0) {
            *off += (size_t)r;
        } else if (r == 0) {
            return DUCT_ECLOSED;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int rc = wait_fd(fd, POLLIN, deadline);
            if (rc != 0) {
                return rc;
            }
        } else if (errno == ECONNRESET) {
            return DUCT_ECLOSED;
        } else if (errno != EINTR) {
            return -errno;
        }
    }
    return 0;
}

/* Writes n bytes. *sent receives the number of bytes written even on failure. */
static int write_full(int fd, const uint8_t *buf, size_t n, size_t *sent, int64_t deadline) {
    *sent = 0;
    while (*sent < n) {
        ssize_t w = send(fd, buf + *sent, n - *sent, MSG_NOSIGNAL);
        if (w >= 0) {
            *sent += (size_t)w;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int rc = wait_fd(fd, POLLOUT, deadline);
            if (rc != 0) {
                return rc;
            }
        } else if (errno == EPIPE || errno == ECONNRESET) {
            return DUCT_ECLOSED;
        } else if (errno != EINTR) {
            return -errno;
        }
    }
    return 0;
}

static void tcp_ep_free(struct tcp_ep *e) {
    if (e->fd >= 0) {
        close(e->fd);
    }
    free(e->rx_buf);
    free(e->tx_buf);
    free(e);
}

static int tcp_send(duct_ep *base, const void *data, size_t len, int timeout_ms) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (e->broken != 0) {
        return e->broken;
    }
    uint8_t *f = e->tx_buf;
    f[0] = (uint8_t)len;
    f[1] = (uint8_t)(len >> 8);
    f[2] = (uint8_t)(len >> 16);
    f[3] = (uint8_t)(len >> 24);
    if (len > 0) {
        memcpy(f + FRAME_HEADER, data, len);
    }
    size_t sent;
    int rc = write_full(e->fd, f, FRAME_HEADER + len, &sent, duct_deadline(timeout_ms));
    if (rc != 0 && sent > 0) {
        e->broken = rc == -ETIMEDOUT ? -ECONNABORTED : rc;
    }
    return rc;
}

static int tcp_send_batch(duct_ep *base, const struct duct_msg *msgs, unsigned n, int timeout_ms) {
    for (unsigned i = 0; i < n; i++) {
        int rc = tcp_send(base, msgs[i].data, msgs[i].len, timeout_ms);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int tcp_recv(duct_ep *base, struct duct_rx *rx, int timeout_ms) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (e->broken != 0) {
        return e->broken;
    }
    if (e->rx_busy) {
        return -EBUSY;
    }
    int64_t deadline = duct_deadline(timeout_ms);
    int rc = read_full(e->fd, e->rx_buf, &e->rx_off, FRAME_HEADER, deadline);
    if (rc != 0) {
        if (rc != -ETIMEDOUT) {
            e->broken = rc;
        }
        return rc;
    }
    uint32_t len = (uint32_t)e->rx_buf[0] | ((uint32_t)e->rx_buf[1] << 8) | ((uint32_t)e->rx_buf[2] << 16) |
                   ((uint32_t)e->rx_buf[3] << 24);
    if (len > e->max_msg) {
        e->broken = DUCT_EPROTO;
        return DUCT_EPROTO;
    }
    rc = read_full(e->fd, e->rx_buf, &e->rx_off, FRAME_HEADER + len, deadline);
    if (rc != 0) {
        if (rc != -ETIMEDOUT) {
            e->broken = rc;
        }
        return rc;
    }
    e->rx_busy = 1;
    rx->data = e->rx_buf + FRAME_HEADER;
    rx->len = len;
    rx->token = 0;
    return 0;
}

static int tcp_release(duct_ep *base, struct duct_rx *rx) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (!e->rx_busy) {
        return -EINVAL;
    }
    e->rx_busy = 0;
    e->rx_off = 0;
    rx->data = NULL;
    rx->len = 0;
    return 0;
}

static void tcp_close(duct_ep *base) {
    tcp_ep_free((struct tcp_ep *)base);
}

static const struct duct_ep_ops tcp_ep_ops = {
    .send = tcp_send,
    .send_batch = tcp_send_batch,
    .recv = tcp_recv,
    .release = tcp_release,
    .close = tcp_close,
};

/* Exchanges handshakes on a connected, non-blocking socket and builds the endpoint. */
static int tcp_ep_create(duct_ctx *ctx, int fd, int64_t deadline, duct_ep **out) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int rc = set_nonblock(fd);
    if (rc != 0) {
        close(fd);
        return rc;
    }
    uint8_t hello[DUCT_HELLO_SIZE];
    struct duct_hello mine = {.max_msg = ctx->opts.max_msg, .recv_depth = ctx->opts.recv_depth};
    duct_hello_encode(&mine, hello);
    size_t sent;
    rc = write_full(fd, hello, sizeof(hello), &sent, deadline);
    size_t got = 0;
    if (rc == 0) {
        rc = read_full(fd, hello, &got, sizeof(hello), deadline);
    }
    struct duct_hello theirs;
    if (rc == 0) {
        rc = duct_hello_decode(hello, got, &theirs);
    }
    if (rc != 0) {
        close(fd);
        return rc;
    }
    struct tcp_ep *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        close(fd);
        return -ENOMEM;
    }
    e->base.ops = &tcp_ep_ops;
    e->base.ctx = ctx;
    /* The effective send limit is the smaller of both sides' limits (same rule as the RDMA transport). */
    e->base.peer_max_msg = theirs.max_msg < ctx->opts.max_msg ? theirs.max_msg : ctx->opts.max_msg;
    e->fd = fd;
    e->max_msg = ctx->opts.max_msg;
    e->rx_buf = malloc(FRAME_HEADER + (size_t)e->max_msg);
    e->tx_buf = malloc(FRAME_HEADER + (size_t)e->base.peer_max_msg);
    if (e->rx_buf == NULL || e->tx_buf == NULL) {
        tcp_ep_free(e);
        return -ENOMEM;
    }
    *out = &e->base;
    return 0;
}

int duct_tcp_connect(duct_ctx *ctx, const char *addr, const char *port, duct_ep **out) {
    int64_t deadline = duct_deadline(ctx->opts.connect_timeout_ms);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(addr, port, &hints, &res) != 0) {
        return -EHOSTUNREACH;
    }
    int last = -ECONNREFUSED;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd < 0) {
            last = -errno;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            rc = wait_fd(fd, POLLOUT, deadline);
            if (rc == 0) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                rc = soerr == 0 ? 0 : -soerr;
            }
        } else if (rc < 0) {
            rc = -errno;
        }
        if (rc == 0) {
            freeaddrinfo(res);
            return tcp_ep_create(ctx, fd, deadline, out);
        }
        last = rc;
        close(fd);
    }
    freeaddrinfo(res);
    return last;
}

static int tcp_listener_accept(duct_listener *base, int timeout_ms, duct_ep **out) {
    struct tcp_listener *l = (struct tcp_listener *)base;
    int64_t deadline = duct_deadline(timeout_ms);
    int rc = wait_fd(l->fd, POLLIN, deadline);
    if (rc != 0) {
        return rc;
    }
    int fd = accept4(l->fd, NULL, NULL, SOCK_NONBLOCK);
    if (fd < 0) {
        return errno == EAGAIN ? -ETIMEDOUT : -errno;
    }
    int hs = base->ctx->opts.connect_timeout_ms;
    return tcp_ep_create(base->ctx, fd, duct_deadline(hs), out);
}

static int tcp_listener_port(const duct_listener *base) {
    return ((const struct tcp_listener *)base)->port;
}

static void tcp_listener_close(duct_listener *base) {
    struct tcp_listener *l = (struct tcp_listener *)base;
    close(l->fd);
    free(l);
}

static const struct duct_listener_ops tcp_listener_ops = {
    .accept = tcp_listener_accept,
    .port = tcp_listener_port,
    .close = tcp_listener_close,
};

int duct_tcp_listen(duct_ctx *ctx, const char *addr, const char *port, duct_listener **out) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
    struct addrinfo *res = NULL;
    if (getaddrinfo(addr, port, &hints, &res) != 0) {
        return -EINVAL;
    }
    int fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK, res->ai_protocol);
    int rc = 0;
    if (fd < 0) {
        rc = -errno;
    } else {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 128) != 0) {
            rc = -errno;
            close(fd);
        }
    }
    freeaddrinfo(res);
    if (rc != 0) {
        return rc;
    }
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    getsockname(fd, (struct sockaddr *)&sa, &sl);
    struct tcp_listener *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        close(fd);
        return -ENOMEM;
    }
    l->base.ops = &tcp_listener_ops;
    l->base.ctx = ctx;
    l->fd = fd;
    l->port = ntohs(sa.sin_port);
    *out = &l->base;
    return 0;
}
