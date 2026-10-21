/*
 * tcp.c - TCP transport.
 *
 * Sockets are non-blocking; all waits use poll() with the caller's deadline.
 *
 * Receive path. A state machine reads frames as far as the socket allows and resumes at the same byte on
 * the next call, so a timeout inside a frame does not corrupt the stream. Socket reads go through a
 * per-endpoint input buffer of IBUF_SIZE bytes, so that one recv() delivers many small pipelined frames;
 * a payload of at least IBUF_DIRECT bytes is read straight into its destination while the buffer is empty.
 * Buffered bytes are consumed before the socket is read again, so they need no readiness event: a call
 * that returns for lack of data has emptied the buffer. Data frames are read into
 * receive slots: 2 * recv_depth buffers of max_msg bytes, allocated on first use. At most recv_depth
 * delivered messages may be held by the application (as on RDMA, where the peer is limited by credits);
 * the other slots buffer frames that arrive while the application waits for a one-sided operation, so
 * that the response behind them can be read. When every slot is in use, reading stops until a release.
 * duct_recv() delivers a data frame that lies complete in the input buffer in place, without copying it
 * into a slot, when no earlier frame waits in a slot. While such a message is held, the buffer is not
 * refilled: the endpoint switches to a second input buffer (allocated on first use), and in-place
 * delivery from it waits until the messages of the first buffer are released, so that at most one buffer
 * is referenced by held messages and a refill is always possible.
 *
 * Send path. Frames are written directly while the socket accepts data; the rest of a frame is copied
 * into a send queue that later calls flush. duct_send() waits while the queue holds more than
 * send_depth full-size frames and returns -ETIMEDOUT without writing anything when the deadline passes,
 * so a timeout never leaves a partial frame. Control responses are queued without that limit, because
 * the servicing side must not block.
 *
 * Control frames (bit 31 of the frame word set) emulate one-sided operations. Payload layout
 * (little-endian), common prefix: type (1), atomic op (1), reserved (2), operation id (8):
 *
 *   type 1 READ_REQ     prefix, addr (8), key (4), len (4)                          28 bytes
 *   type 2 READ_RESP    prefix, status (4), reserved (4), then len data bytes if ok 20 (+ len)
 *   type 3 WRITE_REQ    prefix, addr (8), key (4), len (4), then len data bytes     28 + len
 *   type 4 WRITE_RESP   prefix, status (4), reserved (4)                            20
 *   type 5 ATOMIC_REQ   prefix, addr (8), key (4), reserved (4), a (8), b (8)       44
 *   type 6 ATOMIC_RESP  prefix, status (4), reserved (4), previous value (8)        28
 *
 * Status 0 is success, 1 an access violation. The owner of a region executes requests in arrival order
 * while its application calls into the library; responses therefore arrive in request order, and a
 * response must match the oldest outstanding request (type and id), otherwise the stream is rejected.
 * An access violation fails the requesting endpoint with DUCT_EACCESS, as an RDMA queue pair would.
 *
 * Event loop integration: each endpoint owns an epoll descriptor that contains the socket (POLLIN, and
 * POLLOUT while the send queue is not empty).
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "internal.h"

#define FRAME_HEADER 4u
#define CTRL_FLAG 0x80000000u
#define CTRL_PREFIX 12u
#define CTRL_MAX_FIXED 44u
#define CLOSE_FLUSH_MS 1000
#define IBUF_SIZE 65536u
#define IBUF_DIRECT 4096u
/* iovec entries per sendmsg() of a batch; at most IOV_MAX (1024, sysconf(_SC_IOV_MAX) on Linux). */
#define IOV_BATCH 64u
/*
 * Batch staging: frames with a payload of at most COPY_MAX bytes are copied, header and payload, into a
 * contiguous stack buffer of SBUF_SIZE bytes, so that a run of small frames is one iovec. Copying a small
 * payload costs a few cycles per byte; every extra iovec costs a segment walk in the kernel's
 * copy_from_iter, and every extra sendmsg() a full transmit path.
 */
#define COPY_MAX 512u
#define SBUF_SIZE 16384u

enum ctrl_type {
    CT_READ_REQ = 1,
    CT_READ_RESP = 2,
    CT_WRITE_REQ = 3,
    CT_WRITE_RESP = 4,
    CT_ATOMIC_REQ = 5,
    CT_ATOMIC_RESP = 6,
};

enum rx_state {
    RX_HDR,       /* reading the 4-byte frame word */
    RX_DATA,      /* reading a data frame into a slot */
    RX_CTRL,      /* reading the fixed part of a control frame */
    RX_CTRL_BODY, /* reading the data part of READ_RESP or WRITE_REQ */
};

enum slot_state {
    SLOT_FREE = 0,
    SLOT_READY = 1,
    SLOT_HELD = 2,
};

/*
 * Outstanding one-sided operation issued by this endpoint. seq is the wire id; done_seq is the sequence
 * number that completes with it (larger when zero-copy sends were posted behind it).
 */
struct pending_op {
    uint64_t seq;
    uint64_t done_seq;
    enum duct_rma_op op;
    uint8_t *laddr;
    size_t len;
};

struct tcp_ep {
    struct duct_ep base;
    int fd;
    int epfd;
    uint32_t epmask; /* events to wait for: registered for fd in epfd, or reported in direct mode */
    int direct;      /* duct_ep_direct_fd() was called: fd is not in epfd, the caller waits on it */
    uint32_t max_msg;
    uint32_t recv_depth;
    uint32_t send_depth;
    int broken; /* sticky error (negative code) */

    /* input buffer: bytes read from the socket and not yet consumed by the state machine */
    uint8_t *ib;        /* ibuf[ibcur] */
    uint8_t *ibuf[2];   /* the second buffer is allocated when first needed */
    uint32_t ibrefs[2]; /* held messages delivered in place from each buffer */
    unsigned ibcur;
    size_t ib_pos;
    size_t ib_len;
    int lazy; /* pump() called by duct_recv(): leave a complete data frame in the buffer for in-place delivery */
    /*
     * Socket emptiness hint, to save recv() calls that would return EAGAIN:
     * HINT_UNKNOWN: read the socket; HINT_SEEN_EMPTY: the last recv() returned less than requested, so the next
     * read reports "no data" without a system call; HINT_REPORTED_EMPTY: "no data" was reported since the last
     * arm, so tcp_arm() does not read again. The descriptors are level-triggered, so bytes that arrive after
     * the hint was taken raise an event.
     */
    enum { HINT_UNKNOWN, HINT_SEEN_EMPTY, HINT_REPORTED_EMPTY } hint;

    /* receive state machine */
    enum rx_state rxs;
    uint8_t hdr[FRAME_HEADER];
    size_t hdr_off;
    uint32_t flen;   /* payload length of the current frame */
    uint32_t cur;    /* slot of the current data frame */
    size_t data_off; /* bytes of the current data payload read */
    uint8_t ctl[CTRL_MAX_FIXED];
    size_t ctl_need;
    size_t ctl_off;
    uint8_t *body; /* destination of the control body, NULL to discard */
    size_t body_len;
    size_t body_off;
    int body_ok; /* WRITE_REQ: destination valid */

    /* receive slots */
    uint32_t nslots;
    uint8_t **slot_buf;
    uint32_t *slot_len;
    uint8_t *slot_state;
    uint32_t *ready; /* FIFO of READY slots */
    uint32_t rhead;
    uint32_t rcount;
    uint32_t held;

    /* send queue */
    uint8_t *q;
    size_t qlen; /* bytes stored */
    size_t qoff; /* bytes already written */
    size_t qcap;
    size_t qlimit;

    /* one-sided operations issued by this side */
    struct pending_op *ops; /* FIFO of send_depth entries */
    uint32_t ohead;
    uint32_t ocount;
    uint64_t posted;
    uint64_t done;
};

struct tcp_listener {
    struct duct_listener base;
    int fd;
    int port;
};

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

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
        int rc = poll(&p, events != 0 ? 1 : 0, duct_remaining_ms(deadline));
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

/* Reads exactly n bytes into buf starting at *off (blocking until the deadline; handshake only). */
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

/* Writes n bytes (blocking until the deadline; handshake only). */
static int write_full(int fd, const uint8_t *buf, size_t n, int64_t deadline) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (w >= 0) {
            sent += (size_t)w;
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

/*
 * Reads up to n bytes from the socket without blocking and maintains the emptiness hint. Returns the byte
 * count (> 0), 0 when no data is available, or a negative error (DUCT_ECLOSED for an orderly or abortive
 * close).
 */
static ssize_t sock_read(struct tcp_ep *e, uint8_t *buf, size_t n) {
    if (e->hint == HINT_SEEN_EMPTY) {
        e->hint = HINT_REPORTED_EMPTY;
        return 0;
    }
    for (;;) {
        ssize_t r = recv(e->fd, buf, n, 0);
        if (r > 0) {
            e->hint = (size_t)r < n ? HINT_SEEN_EMPTY : HINT_UNKNOWN;
            return r;
        }
        if (r == 0) {
            return DUCT_ECLOSED;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            e->hint = HINT_REPORTED_EMPTY;
            return 0;
        }
        if (errno == ECONNRESET) {
            return DUCT_ECLOSED;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
}

/*
 * Refills the consumed input buffer from the socket; switches to the other buffer while held messages
 * refer to the current one. Returns as sock_read().
 */
static ssize_t ib_fill(struct tcp_ep *e) {
    if (e->ibrefs[e->ibcur] > 0) {
        unsigned o = e->ibcur ^ 1u; /* not referenced: in-place delivery requires the other buffer free */
        if (e->ibuf[o] == NULL && (e->ibuf[o] = malloc(IBUF_SIZE)) == NULL) {
            return -ENOMEM;
        }
        e->ibcur = o;
        e->ib = e->ibuf[o];
    }
    e->ib_pos = 0;
    e->ib_len = 0;
    ssize_t r = sock_read(e, e->ib, IBUF_SIZE);
    if (r > 0) {
        e->ib_len = (size_t)r;
    }
    return r;
}

/* Returns 1 when a complete data frame starts at the read position and may be delivered in place. */
static int ib_frame_ready(const struct tcp_ep *e) {
    size_t avail = e->ib_len - e->ib_pos;
    if (avail < FRAME_HEADER || e->ibrefs[e->ibcur ^ 1u] != 0) {
        return 0;
    }
    uint32_t word = get_le32(e->ib + e->ib_pos);
    return (word & CTRL_FLAG) == 0 && word <= e->max_msg && avail - FRAME_HEADER >= word;
}

/*
 * Reads up to n bytes without blocking, from the input buffer first, otherwise from the socket. Returns the
 * byte count (> 0), 0 when no data is available, or a negative error (DUCT_ECLOSED for an orderly or
 * abortive close).
 */
static ssize_t read_some(struct tcp_ep *e, uint8_t *buf, size_t n) {
    if (e->ib_pos == e->ib_len) {
        if (n >= IBUF_DIRECT) {
            return sock_read(e, buf, n);
        }
        ssize_t r = ib_fill(e);
        if (r <= 0) {
            return r;
        }
    }
    size_t take = e->ib_len - e->ib_pos < n ? e->ib_len - e->ib_pos : n;
    memcpy(buf, e->ib + e->ib_pos, take);
    e->ib_pos += take;
    return (ssize_t)take;
}

/* ---- send queue ------------------------------------------------------------------------------------ */

static size_t queued(const struct tcp_ep *e) {
    return e->qlen - e->qoff;
}

/* Appends bytes to the send queue. Returns 0 or -ENOMEM. */
static int q_append(struct tcp_ep *e, const void *data, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (e->qoff > 0 && e->qoff == e->qlen) {
        e->qoff = 0;
        e->qlen = 0;
    }
    if (e->qlen + n > e->qcap) {
        if (e->qoff > 0) {
            memmove(e->q, e->q + e->qoff, e->qlen - e->qoff);
            e->qlen -= e->qoff;
            e->qoff = 0;
        }
        if (e->qlen + n > e->qcap) {
            size_t cap = e->qcap > 0 ? e->qcap : 4096;
            while (cap < e->qlen + n) {
                cap *= 2;
            }
            uint8_t *q = realloc(e->q, cap);
            if (q == NULL) {
                return -ENOMEM;
            }
            e->q = q;
            e->qcap = cap;
        }
    }
    memcpy(e->q + e->qlen, data, n);
    e->qlen += n;
    return 0;
}

/* Writes queued bytes without blocking. Returns 0 or a sticky error. */
static int q_flush(struct tcp_ep *e) {
    while (queued(e) > 0) {
        ssize_t w = send(e->fd, e->q + e->qoff, queued(e), MSG_NOSIGNAL);
        if (w > 0) {
            e->qoff += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        e->broken = (w < 0 && errno != EPIPE && errno != ECONNRESET) ? -errno : DUCT_ECLOSED;
        return e->broken;
    }
    e->qoff = 0;
    e->qlen = 0;
    return 0;
}

/*
 * Writes the bytes of iov[0..cnt) (cnt at most IOV_BATCH). Bytes the socket does not accept immediately
 * are queued, so the data is always either written or queued after the call and frames stay whole.
 */
static int emitv(struct tcp_ep *e, struct iovec *iov, int cnt) {
    size_t total = 0;
    for (int i = 0; i < cnt; i++) {
        total += iov[i].iov_len;
    }
    size_t written = 0;
    if (queued(e) == 0) {
        for (;;) {
            /* One contiguous run (a staged batch) uses send(): sendmsg() also copies the msghdr and the
             * iovec array from user space (net/socket.c: ___sys_sendmsg, about 300 cycles more here). */
            struct msghdr mh = {.msg_iov = iov, .msg_iovlen = (size_t)cnt};
            ssize_t w = cnt == 1 ? send(e->fd, iov[0].iov_base, iov[0].iov_len, MSG_NOSIGNAL)
                                 : sendmsg(e->fd, &mh, MSG_NOSIGNAL);
            if (w >= 0) {
                written = (size_t)w;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno != EINTR) {
                e->broken = (errno != EPIPE && errno != ECONNRESET) ? -errno : DUCT_ECLOSED;
                return e->broken;
            }
        }
    }
    if (written == total) {
        return 0;
    }
    for (int i = 0; i < cnt; i++) {
        size_t n = iov[i].iov_len;
        if (written >= n) {
            written -= n;
            continue;
        }
        int rc = q_append(e, (const uint8_t *)iov[i].iov_base + written, n - written);
        if (rc != 0) {
            e->broken = rc;
            return rc;
        }
        written = 0;
    }
    return q_flush(e);
}

/* Writes a frame made of up to three parts (see emitv). */
static int emit(struct tcp_ep *e, const void *p0, size_t n0, const void *p1, size_t n1, const void *p2,
                size_t n2) {
    struct iovec iov[3] = {{(void *)p0, n0}, {(void *)p1, n1}, {(void *)p2, n2}};
    return emitv(e, iov, 3);
}

/* ---- receive slots --------------------------------------------------------------------------------- */

/* Returns a free slot with an allocated buffer, or UINT32_MAX. */
static uint32_t slot_take(struct tcp_ep *e) {
    for (uint32_t i = 0; i < e->nslots; i++) {
        if (e->slot_state[i] != SLOT_FREE) {
            continue;
        }
        if (e->slot_buf[i] == NULL) {
            e->slot_buf[i] = malloc(e->max_msg > 0 ? e->max_msg : 1);
            if (e->slot_buf[i] == NULL) {
                return UINT32_MAX;
            }
        }
        return i;
    }
    return UINT32_MAX;
}

/* ---- control frames -------------------------------------------------------------------------------- */

/* Fixed payload size of a control frame type, or 0 for an unknown type. */
static size_t ctrl_fixed(uint8_t type) {
    switch (type) {
    case CT_READ_REQ:
    case CT_WRITE_REQ:
        return 28;
    case CT_READ_RESP:
    case CT_WRITE_RESP:
        return 20;
    case CT_ATOMIC_REQ:
        return 44;
    case CT_ATOMIC_RESP:
        return 28;
    default:
        return 0;
    }
}

/* Builds the frame word and common prefix of a control frame into out (FRAME_HEADER + CTRL_PREFIX bytes). */
static void ctrl_prefix(uint8_t *out, uint32_t payload, uint8_t type, uint8_t aop, uint64_t id) {
    put_le32(out, CTRL_FLAG | payload);
    out[4] = type;
    out[5] = aop;
    out[6] = 0;
    out[7] = 0;
    put_le64(out + 8, id);
}

/* Sends a response with status and optional trailing data or value. */
static int send_resp(struct tcp_ep *e, uint8_t type, uint64_t id, uint32_t status, const void *data, size_t n,
                     const uint64_t *value) {
    uint8_t f[FRAME_HEADER + 28];
    size_t fixed = ctrl_fixed(type);
    ctrl_prefix(f, (uint32_t)(fixed + n), type, 0, id);
    put_le32(f + FRAME_HEADER + 12, status);
    put_le32(f + FRAME_HEADER + 16, 0);
    if (value != NULL) {
        put_le64(f + FRAME_HEADER + 20, *value);
    }
    return emit(e, f, FRAME_HEADER + fixed, data, n, NULL, 0);
}

/* Head of the outstanding-operation FIFO, or NULL. */
static struct pending_op *op_head(struct tcp_ep *e) {
    return e->ocount > 0 ? &e->ops[e->ohead] : NULL;
}

static void op_pop(struct tcp_ep *e) {
    e->done = e->ops[e->ohead].done_seq;
    e->ohead = (e->ohead + 1) % e->send_depth;
    e->ocount--;
}

/*
 * Handles the fixed part of a control frame (in e->ctl). Sets up the body phase for READ_RESP and
 * WRITE_REQ. Returns 0 or a sticky error.
 */
static int ctrl_fixed_done(struct tcp_ep *e) {
    uint8_t type = e->ctl[0];
    uint64_t id = get_le64(e->ctl + 4);
    const uint8_t *f = e->ctl + CTRL_PREFIX;
    size_t extra = e->flen - ctrl_fixed(type);
    switch (type) {
    case CT_READ_REQ: {
        uint64_t addr = get_le64(f);
        uint32_t key = get_le32(f + 8);
        uint32_t len = get_le32(f + 12);
        if (extra != 0) {
            return DUCT_EPROTO;
        }
        uint8_t *src = duct_mr_resolve(e->base.ctx, key, addr, len, DUCT_ACCESS_REMOTE_READ);
        int rc = send_resp(e, CT_READ_RESP, id, src != NULL ? 0 : 1, src, src != NULL ? len : 0, NULL);
        e->rxs = RX_HDR;
        return rc;
    }
    case CT_WRITE_REQ: {
        uint64_t addr = get_le64(f);
        uint32_t key = get_le32(f + 8);
        uint32_t len = get_le32(f + 12);
        if (extra != len) {
            return DUCT_EPROTO;
        }
        e->body = duct_mr_resolve(e->base.ctx, key, addr, len, DUCT_ACCESS_REMOTE_WRITE);
        e->body_ok = e->body != NULL;
        e->body_len = len;
        e->body_off = 0;
        e->rxs = RX_CTRL_BODY;
        return 0;
    }
    case CT_ATOMIC_REQ: {
        uint64_t addr = get_le64(f);
        uint32_t key = get_le32(f + 8);
        uint64_t a = get_le64(f + 16);
        uint64_t b = get_le64(f + 24);
        if (extra != 0) {
            return DUCT_EPROTO;
        }
        uint8_t *p = duct_mr_resolve(e->base.ctx, key, addr, 8, DUCT_ACCESS_REMOTE_ATOMIC);
        uint64_t old = 0;
        uint32_t status = 1;
        if (p != NULL && ((uintptr_t)p & 7u) == 0) {
            uint64_t *w = (uint64_t *)p;
            if (e->ctl[1] == DUCT_RMA_FADD) {
                old = __atomic_fetch_add(w, a, __ATOMIC_SEQ_CST);
                status = 0;
            } else if (e->ctl[1] == DUCT_RMA_CSWAP) {
                old = a;
                __atomic_compare_exchange_n(w, &old, b, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                status = 0;
            } else {
                return DUCT_EPROTO;
            }
        }
        e->rxs = RX_HDR;
        return send_resp(e, CT_ATOMIC_RESP, id, status, NULL, 0, &old);
    }
    case CT_READ_RESP:
    case CT_WRITE_RESP:
    case CT_ATOMIC_RESP: {
        struct pending_op *op = op_head(e);
        enum duct_rma_op want = type == CT_READ_RESP    ? DUCT_RMA_READ
                                : type == CT_WRITE_RESP ? DUCT_RMA_WRITE
                                                        : 0;
        if (op == NULL || op->seq != id || (want != 0 && op->op != want) ||
            (want == 0 && op->op != DUCT_RMA_FADD && op->op != DUCT_RMA_CSWAP)) {
            return DUCT_EPROTO;
        }
        uint32_t status = get_le32(f);
        if (status != 0) {
            return extra == 0 ? DUCT_EACCESS : DUCT_EPROTO;
        }
        if (type == CT_READ_RESP) {
            if (extra != op->len) {
                return DUCT_EPROTO;
            }
            e->body = op->laddr;
            e->body_ok = 1;
            e->body_len = op->len;
            e->body_off = 0;
            e->rxs = RX_CTRL_BODY;
            return 0;
        }
        if (extra != 0) {
            return DUCT_EPROTO;
        }
        if (type == CT_ATOMIC_RESP) {
            uint64_t v = get_le64(f + 8);
            memcpy(op->laddr, &v, sizeof(v));
        }
        op_pop(e);
        e->rxs = RX_HDR;
        return 0;
    }
    default:
        return DUCT_EPROTO;
    }
}

/* Handles the end of a control body. */
static int ctrl_body_done(struct tcp_ep *e) {
    e->rxs = RX_HDR;
    if (e->ctl[0] == CT_WRITE_REQ) {
        return send_resp(e, CT_WRITE_RESP, get_le64(e->ctl + 4), e->body_ok ? 0 : 1, NULL, 0, NULL);
    }
    op_pop(e); /* READ_RESP */
    return 0;
}

/*
 * Reads and processes frames as far as the socket allows without blocking. Returns 1 if anything was
 * read, 0 if nothing was available (or a data frame waits for a free slot), or a sticky error.
 */
static int pump(struct tcp_ep *e) {
    int any = 0;
    uint8_t discard[512];
    for (;;) {
        if (e->broken != 0) {
            return e->broken;
        }
        ssize_t r = 0;
        int rc = 0;
        switch (e->rxs) {
        case RX_HDR:
            if (e->lazy && e->hdr_off == 0 && e->rcount == 0 && e->held < e->recv_depth) {
                if (e->ib_pos == e->ib_len) {
                    r = ib_fill(e);
                    if (r <= 0) {
                        break;
                    }
                    any = 1;
                }
                if (ib_frame_ready(e)) {
                    return 1; /* tcp_recv() delivers it in place */
                }
            }
            r = read_some(e, e->hdr + e->hdr_off, FRAME_HEADER - e->hdr_off);
            if (r <= 0) {
                break;
            }
            e->hdr_off += (size_t)r;
            any = 1;
            if (e->hdr_off < FRAME_HEADER) {
                continue;
            }
            e->hdr_off = 0;
            uint32_t word = get_le32(e->hdr);
            if (word & CTRL_FLAG) {
                e->flen = word & ~CTRL_FLAG;
                e->ctl_off = 0;
                e->ctl_need = 0;
                if (e->flen < CTRL_PREFIX) {
                    rc = DUCT_EPROTO;
                    break;
                }
                e->rxs = RX_CTRL;
            } else {
                if (word > e->max_msg) {
                    rc = DUCT_EPROTO;
                    break;
                }
                e->flen = word;
                e->cur = UINT32_MAX;
                e->data_off = 0;
                e->rxs = RX_DATA;
            }
            r = 1;
            break;
        case RX_DATA:
            if (e->cur == UINT32_MAX) {
                e->cur = slot_take(e);
                if (e->cur == UINT32_MAX) {
                    return any; /* all slots in use: wait for a release */
                }
                e->slot_state[e->cur] = SLOT_HELD; /* reserved while being filled */
            }
            r = e->flen > e->data_off ? read_some(e, e->slot_buf[e->cur] + e->data_off, e->flen - e->data_off)
                                      : 1;
            if (r <= 0) {
                break;
            }
            if (e->flen > e->data_off) {
                e->data_off += (size_t)r;
            }
            if (e->data_off == e->flen) {
                e->slot_state[e->cur] = SLOT_READY;
                e->slot_len[e->cur] = e->flen;
                e->ready[(e->rhead + e->rcount) % e->nslots] = e->cur;
                e->rcount++;
                e->rxs = RX_HDR;
            }
            break;
        case RX_CTRL:
            if (e->ctl_need == 0) {
                /* the type byte decides the fixed size */
                r = read_some(e, e->ctl, 1);
                if (r <= 0) {
                    break;
                }
                any = 1;
                e->ctl_off = 1;
                e->ctl_need = ctrl_fixed(e->ctl[0]);
                if (e->ctl_need == 0 || e->flen < e->ctl_need) {
                    rc = DUCT_EPROTO;
                    break;
                }
            }
            r = e->ctl_off < e->ctl_need ? read_some(e, e->ctl + e->ctl_off, e->ctl_need - e->ctl_off) : 1;
            if (r <= 0) {
                break;
            }
            if (e->ctl_off < e->ctl_need) {
                e->ctl_off += (size_t)r;
            }
            if (e->ctl_off == e->ctl_need) {
                rc = ctrl_fixed_done(e);
            }
            break;
        case RX_CTRL_BODY:
            if (e->body_off == e->body_len) {
                rc = ctrl_body_done(e);
                r = 1;
                break;
            }
            if (e->body_ok) {
                r = read_some(e, e->body + e->body_off, e->body_len - e->body_off);
            } else {
                size_t n = e->body_len - e->body_off;
                r = read_some(e, discard, n < sizeof(discard) ? n : sizeof(discard));
            }
            if (r > 0) {
                e->body_off += (size_t)r;
                if (e->body_off == e->body_len) {
                    rc = ctrl_body_done(e);
                }
            }
            break;
        }
        if (rc == 0 && r < 0) {
            rc = (int)r;
        }
        if (rc != 0) {
            e->broken = rc;
            return rc;
        }
        if (r == 0) {
            return any;
        }
        any = 1;
    }
}

/* One non-blocking round of work. Returns 1 if anything happened, 0 if not, or a sticky error. */
static int work(struct tcp_ep *e) {
    size_t before = queued(e);
    int rc = q_flush(e);
    if (rc != 0) {
        return rc;
    }
    rc = pump(e);
    if (rc < 0) {
        return rc;
    }
    if (queued(e) > 0) {
        int frc = q_flush(e);
        if (frc != 0) {
            return frc;
        }
    }
    return rc > 0 || queued(e) < before;
}

/* Blocks until the socket is readable (if reading can progress), writable (if data is queued), or the
 * deadline passes. */
static int wait_io(struct tcp_ep *e, int64_t deadline) {
    short ev = 0;
    if (!(e->rxs == RX_DATA && e->cur == UINT32_MAX)) {
        ev |= POLLIN;
    }
    if (queued(e) > 0) {
        ev |= POLLOUT;
    }
    if (ev == 0) {
        /* Nothing can progress until the application releases a message; sleep to the deadline. */
        int left = duct_remaining_ms(deadline);
        if (left != 0) {
            struct pollfd none;
            poll(&none, 0, left < 0 || left > 10 ? 10 : left);
        }
        return deadline >= 0 && duct_remaining_ms(deadline) == 0 ? -ETIMEDOUT : 0;
    }
    e->hint = HINT_UNKNOWN;
    int rc = wait_fd(e->fd, ev, deadline);
    return rc == -ETIMEDOUT ? -ETIMEDOUT : rc;
}

/* ---- endpoint operations --------------------------------------------------------------------------- */

static void tcp_ep_free(struct tcp_ep *e) {
    if (e->fd >= 0) {
        close(e->fd);
    }
    if (e->epfd >= 0) {
        close(e->epfd);
    }
    if (e->slot_buf != NULL) {
        for (uint32_t i = 0; i < e->nslots; i++) {
            free(e->slot_buf[i]);
        }
    }
    free(e->slot_buf);
    free(e->slot_len);
    free(e->slot_state);
    free(e->ready);
    free(e->ops);
    free(e->q);
    free(e->ibuf[0]);
    free(e->ibuf[1]);
    free(e);
}

/* Waits until a frame of `need` bytes fits into the send queue limit. */
static int wait_send_space(struct tcp_ep *e, size_t need, int64_t deadline) {
    while (queued(e) > 0 && queued(e) + need > e->qlimit) {
        int rc = work(e);
        if (rc < 0) {
            return rc;
        }
        if (queued(e) == 0 || queued(e) + need <= e->qlimit) {
            break;
        }
        if (deadline >= 0 && duct_remaining_ms(deadline) == 0) {
            return -ETIMEDOUT;
        }
        rc = wait_io(e, deadline);
        if (rc != 0 && rc != -ETIMEDOUT) {
            return rc;
        }
    }
    return 0;
}

static int tcp_send(duct_ep *base, const void *data, size_t len, int timeout_ms) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (e->broken != 0) {
        return e->broken;
    }
    int rc = wait_send_space(e, FRAME_HEADER + len, duct_deadline(timeout_ms));
    if (rc != 0) {
        return rc;
    }
    uint8_t h[FRAME_HEADER];
    put_le32(h, (uint32_t)len);
    return emit(e, h, sizeof(h), data, len, NULL, 0);
}

/*
 * Sends the messages in groups, each with one sendmsg(). Small payloads are staged with their headers (see
 * COPY_MAX); a larger payload is referenced in place behind its staged header. A group ends when the
 * staging buffer or the iovec array is full or the group reaches the queue limit. Each group waits for
 * send space like duct_send(); on -ETIMEDOUT the earlier groups have been sent. *sent receives the number
 * of messages written or queued.
 */
static int send_groups(struct tcp_ep *e, const struct duct_msg *msgs, unsigned n, int64_t deadline,
                       unsigned *sent) {
    uint8_t sb[SBUF_SIZE];
    struct iovec iov[IOV_BATCH];
    unsigned i = 0;
    *sent = 0;
    while (i < n) {
        if (e->broken != 0) {
            return e->broken;
        }
        int cnt = 0;
        size_t used = 0;  /* staged bytes */
        size_t bytes = 0; /* frame bytes of the group */
        unsigned first = i;
        while (i < n) {
            size_t len = msgs[i].len;
            int copy = len <= COPY_MAX;
            size_t stage = FRAME_HEADER + (copy ? len : 0);
            /* worst case two new iovecs: the staged run and an in-place payload */
            if (cnt + 2 > (int)IOV_BATCH || used + stage > SBUF_SIZE) {
                break;
            }
            /* a group never needs more than the queue limit, so that wait_send_space can succeed */
            if (i > first && bytes + FRAME_HEADER + len > e->qlimit) {
                break;
            }
            uint8_t *dst = sb + used;
            put_le32(dst, (uint32_t)len);
            if (copy && len > 0) {
                memcpy(dst + FRAME_HEADER, msgs[i].data, len);
            }
            if (cnt > 0 && (uint8_t *)iov[cnt - 1].iov_base + iov[cnt - 1].iov_len == dst) {
                iov[cnt - 1].iov_len += stage; /* extends the staged run */
            } else {
                iov[cnt++] = (struct iovec){dst, stage};
            }
            if (!copy) {
                iov[cnt++] = (struct iovec){(void *)msgs[i].data, len};
            }
            used += stage;
            bytes += FRAME_HEADER + len;
            i++;
        }
        int rc = wait_send_space(e, bytes, deadline);
        if (rc == 0) {
            rc = emitv(e, iov, cnt); /* unsent bytes are copied to the queue: sb is free afterwards */
        }
        if (rc != 0) {
            return rc;
        }
        *sent = i;
    }
    return 0;
}

static int tcp_send_batch(duct_ep *base, const struct duct_msg *msgs, unsigned n, int timeout_ms) {
    unsigned sent;
    return send_groups((struct tcp_ep *)base, msgs, n, duct_deadline(timeout_ms), &sent);
}

static int tcp_send_some(duct_ep *base, const struct duct_msg *msgs, unsigned n) {
    unsigned sent;
    int rc = send_groups((struct tcp_ep *)base, msgs, n, duct_deadline(0), &sent);
    if (rc == -ETIMEDOUT) {
        return (int)sent;
    }
    return rc != 0 ? rc : (int)n;
}

static int tcp_send_mr(duct_ep *base, const duct_mr *mr, size_t off, size_t len, int timeout_ms,
                       uint64_t *seq) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    int rc = tcp_send(base, mr->addr + off, len, timeout_ms);
    if (rc != 0) {
        return rc;
    }
    /* The frame was written or copied into the send queue: the region is reusable at once. */
    e->posted++;
    if (e->ocount == 0) {
        e->done = e->posted;
    } else {
        /* Completion order is posting order: complete together with the operations ahead of it. */
        e->ops[(e->ohead + e->ocount - 1) % e->send_depth].done_seq = e->posted;
    }
    if (seq != NULL) {
        *seq = e->posted;
    }
    return 0;
}

static int tcp_rma(duct_ep *base, const struct duct_rma *op, int timeout_ms, uint64_t *seq) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (e->broken != 0) {
        return e->broken;
    }
    int64_t deadline = duct_deadline(timeout_ms);
    while (e->ocount == e->send_depth) {
        int rc = work(e);
        if (rc < 0) {
            return rc;
        }
        if (e->ocount < e->send_depth) {
            break;
        }
        if (deadline >= 0 && duct_remaining_ms(deadline) == 0) {
            return -ETIMEDOUT;
        }
        rc = wait_io(e, deadline);
        if (rc != 0 && rc != -ETIMEDOUT) {
            return rc;
        }
    }
    size_t body = op->op == DUCT_RMA_WRITE ? op->len : 0;
    int rc = wait_send_space(e, FRAME_HEADER + CTRL_MAX_FIXED + body, deadline);
    if (rc != 0) {
        return rc;
    }
    uint64_t id = e->posted + 1;
    uint8_t f[FRAME_HEADER + CTRL_MAX_FIXED];
    uint8_t *p = f + FRAME_HEADER + CTRL_PREFIX;
    size_t fixed;
    if (op->op == DUCT_RMA_READ || op->op == DUCT_RMA_WRITE) {
        uint8_t type = op->op == DUCT_RMA_READ ? CT_READ_REQ : CT_WRITE_REQ;
        fixed = ctrl_fixed(type);
        ctrl_prefix(f, (uint32_t)(fixed + body), type, 0, id);
        put_le64(p, op->raddr);
        put_le32(p + 8, op->rkey);
        put_le32(p + 12, (uint32_t)op->len);
    } else {
        fixed = ctrl_fixed(CT_ATOMIC_REQ);
        ctrl_prefix(f, (uint32_t)fixed, CT_ATOMIC_REQ, (uint8_t)op->op, id);
        put_le64(p, op->raddr);
        put_le32(p + 8, op->rkey);
        put_le32(p + 12, 0);
        put_le64(p + 16, op->a);
        put_le64(p + 24, op->b);
    }
    rc = emit(e, f, FRAME_HEADER + fixed, op->op == DUCT_RMA_WRITE ? op->laddr : NULL, body, NULL, 0);
    if (rc != 0) {
        return rc;
    }
    e->posted = id;
    e->ops[(e->ohead + e->ocount) % e->send_depth] =
        (struct pending_op){.seq = id, .done_seq = id, .op = op->op, .laddr = op->laddr, .len = op->len};
    e->ocount++;
    if (seq != NULL) {
        *seq = id;
    }
    return 0;
}

static uint64_t tcp_completed(duct_ep *base) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (e->broken == 0) {
        work(e);
    }
    return e->done;
}

static int tcp_wait(duct_ep *base, uint64_t seq, int timeout_ms) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    int64_t deadline = duct_deadline(timeout_ms);
    for (;;) {
        if (e->done >= seq) {
            return 0;
        }
        int rc = work(e);
        if (rc < 0) {
            return rc;
        }
        if (e->done >= seq) {
            return 0;
        }
        if (rc > 0) {
            continue;
        }
        if (deadline >= 0 && duct_remaining_ms(deadline) == 0) {
            return -ETIMEDOUT;
        }
        rc = wait_io(e, deadline);
        if (rc != 0 && rc != -ETIMEDOUT) {
            return rc;
        }
    }
}

static int tcp_recv(duct_ep *base, struct duct_rx *rx, int timeout_ms) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    int64_t deadline = duct_deadline(timeout_ms);
    for (;;) {
        if (e->rcount > 0 && e->held < e->recv_depth) {
            uint32_t s = e->ready[e->rhead];
            e->rhead = (e->rhead + 1) % e->nslots;
            e->rcount--;
            e->slot_state[s] = SLOT_HELD;
            e->held++;
            rx->data = e->slot_buf[s];
            rx->len = e->slot_len[s];
            rx->token = s;
            return 0;
        }
        if (e->rcount == 0 && e->held < e->recv_depth && e->rxs == RX_HDR && e->hdr_off == 0 &&
            ib_frame_ready(e)) {
            uint32_t len = get_le32(e->ib + e->ib_pos);
            rx->data = e->ib + e->ib_pos + FRAME_HEADER;
            rx->len = len;
            rx->token = e->nslots + e->ibcur;
            e->ib_pos += FRAME_HEADER + (size_t)len;
            e->ibrefs[e->ibcur]++;
            e->held++;
            return 0;
        }
        if (e->broken != 0) {
            return e->broken; /* messages received before the failure were delivered first */
        }
        e->lazy = 1;
        int rc = work(e);
        e->lazy = 0;
        if (rc < 0) {
            if (e->rcount > 0 && e->held < e->recv_depth) {
                continue;
            }
            return rc;
        }
        if (rc > 0) {
            continue;
        }
        if (deadline >= 0 && duct_remaining_ms(deadline) == 0) {
            return -ETIMEDOUT;
        }
        rc = wait_io(e, deadline);
        if (rc != 0 && rc != -ETIMEDOUT) {
            e->broken = rc;
            return rc;
        }
    }
}

static int tcp_release(duct_ep *base, struct duct_rx *rx) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (rx->data != NULL && rx->token >= e->nslots && rx->token < e->nslots + 2) {
        unsigned b = rx->token - e->nslots; /* delivered in place from input buffer b */
        const uint8_t *p = rx->data;
        if (e->ibrefs[b] == 0 || e->ibuf[b] == NULL || p < e->ibuf[b] || p > e->ibuf[b] + IBUF_SIZE) {
            return -EINVAL;
        }
        e->ibrefs[b]--;
        e->held--;
        rx->data = NULL;
        rx->len = 0;
        return 0;
    }
    if (rx->data == NULL || rx->token >= e->nslots || e->slot_state[rx->token] != SLOT_HELD ||
        rx->data != e->slot_buf[rx->token]) {
        return -EINVAL;
    }
    e->slot_state[rx->token] = SLOT_FREE;
    e->held--;
    rx->data = NULL;
    rx->len = 0;
    return 0;
}

static int tcp_progress(duct_ep *base) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    int rc = work(e);
    return rc < 0 ? rc : 0;
}

static int tcp_fd(const duct_ep *base) {
    return ((const struct tcp_ep *)base)->epfd;
}

static int tcp_arm(duct_ep *base) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    /* After "no data" was reported, a further read would return EAGAIN; only flush the send queue. */
    int rc = e->hint == HINT_REPORTED_EMPTY ? q_flush(e) : work(e);
    e->hint = HINT_UNKNOWN;
    if (rc < 0) {
        return -EAGAIN; /* the error is reported by the next call */
    }
    if (rc > 0 || (e->rcount > 0 && e->held < e->recv_depth)) {
        return -EAGAIN;
    }
    /* While every receive slot is in use the socket is not watched: only a release can make progress. */
    int slot_blocked = e->rxs == RX_DATA && e->cur == UINT32_MAX;
    uint32_t mask = (slot_blocked ? 0 : EPOLLIN) | (queued(e) > 0 ? EPOLLOUT : 0);
    if (e->direct) {
        e->epmask = mask; /* the caller registers it (duct_ep_wait_events) */
    } else if (mask != e->epmask) {
        struct epoll_event ev = {.events = mask, .data.fd = e->fd};
        if (epoll_ctl(e->epfd, EPOLL_CTL_MOD, e->fd, &ev) != 0) {
            return -errno;
        }
        e->epmask = mask;
    }
    return 0;
}

static int tcp_direct_fd(duct_ep *base) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (!e->direct) {
        epoll_ctl(e->epfd, EPOLL_CTL_DEL, e->fd, NULL);
        e->direct = 1;
    }
    return e->fd;
}

static uint32_t tcp_wait_events(const duct_ep *base) {
    return ((const struct tcp_ep *)base)->epmask;
}

static unsigned tcp_send_space(duct_ep *base) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    if (e->broken != 0) {
        return 0;
    }
    q_flush(e);
    if (queued(e) == 0) {
        return e->send_depth;
    }
    size_t frame = FRAME_HEADER + (size_t)e->base.peer_max_msg;
    return queued(e) >= e->qlimit ? 0 : (unsigned)((e->qlimit - queued(e)) / frame);
}

static void tcp_close(duct_ep *base) {
    struct tcp_ep *e = (struct tcp_ep *)base;
    /* Deliver queued data before closing, as the RDMA transport waits for sends in flight. */
    int64_t deadline = duct_deadline(CLOSE_FLUSH_MS);
    while (e->broken == 0 && queued(e) > 0 && duct_remaining_ms(deadline) > 0) {
        if (q_flush(e) != 0 || queued(e) == 0) {
            break;
        }
        wait_fd(e->fd, POLLOUT, deadline);
    }
    tcp_ep_free(e);
}

static const struct duct_ep_ops tcp_ep_ops = {
    .send = tcp_send,
    .send_batch = tcp_send_batch,
    .send_some = tcp_send_some,
    .recv = tcp_recv,
    .release = tcp_release,
    .close = tcp_close,
    .fd = tcp_fd,
    .arm = tcp_arm,
    .progress = tcp_progress,
    .send_space = tcp_send_space,
    .direct_fd = tcp_direct_fd,
    .wait_events = tcp_wait_events,
    .send_mr = tcp_send_mr,
    .rma = tcp_rma,
    .completed = tcp_completed,
    .wait = tcp_wait,
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
    rc = write_full(fd, hello, sizeof(hello), deadline);
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
    e->epfd = -1;
    e->max_msg = ctx->opts.max_msg;
    e->recv_depth = ctx->opts.recv_depth;
    e->send_depth = ctx->opts.send_depth;
    e->nslots = 2 * e->recv_depth;
    e->qlimit = (size_t)e->send_depth * (FRAME_HEADER + (size_t)e->base.peer_max_msg);
    e->slot_buf = calloc(e->nslots, sizeof(*e->slot_buf));
    e->slot_len = calloc(e->nslots, sizeof(*e->slot_len));
    e->slot_state = calloc(e->nslots, sizeof(*e->slot_state));
    e->ready = calloc(e->nslots, sizeof(*e->ready));
    e->ops = calloc(e->send_depth, sizeof(*e->ops));
    e->ibuf[0] = malloc(IBUF_SIZE);
    e->ib = e->ibuf[0];
    e->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (e->slot_buf == NULL || e->slot_len == NULL || e->slot_state == NULL || e->ready == NULL ||
        e->ops == NULL || e->ib == NULL || e->epfd < 0) {
        rc = e->epfd < 0 ? -errno : -ENOMEM;
        tcp_ep_free(e);
        return rc;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    if (epoll_ctl(e->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        rc = -errno;
        tcp_ep_free(e);
        return rc;
    }
    e->epmask = EPOLLIN;
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

static int tcp_listener_fd(const duct_listener *base) {
    return ((const struct tcp_listener *)base)->fd;
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
    .fd = tcp_listener_fd,
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
