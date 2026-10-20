/*
 * internal.h - structures shared by the transport implementations.
 *
 * Wire handshake ("hello"): exchanged once per connection by both sides. TCP sends it as the first
 * DUCT_HELLO_SIZE bytes of the stream; RDMA sends it as connection-manager private data. All fields
 * are little-endian.
 *
 *   offset size field
 *   0      4    magic        0x31505452 ("RTP1")
 *   4      2    version      protocol version, currently 3 (3 added TCP control frames)
 *   6      2    flags        reserved, zero
 *   8      4    max_msg      largest message the sender accepts
 *   12     4    recv_depth   receive buffers the sender posts
 *   16     8    credit_addr  RDMA: address of the sender's credit word (zero for TCP)
 *   24     4    credit_rkey  RDMA: remote key of the credit word (zero for TCP)
 *   28     4    reserved     zero
 *
 * TCP framing after the handshake: a 4-byte little-endian word, then the payload. Bit 31 clear: data
 * frame, the word is the payload length (at most max_msg). Bit 31 set: control frame carrying an
 * emulated one-sided operation, the low 31 bits are the payload length (layout in tcp.c).
 *
 * Region descriptor (struct duct_mr_desc, DUCT_MR_DESC_SIZE bytes, little-endian):
 *
 *   offset size field
 *   0      4    magic        0x3152_4d44 ("DMR1")
 *   4      4    access       enum duct_access bits granted to peers
 *   8      8    addr         start address of the region in the owner's address space
 *   16     8    len          region length in bytes
 *   24     4    key          RDMA: remote key; TCP: registry key of the region in the owner's context
 *   28     4    reserved     zero
 */
#ifndef DUCT_INTERNAL_H
#define DUCT_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "rdmaduct.h"

#define DUCT_HELLO_MAGIC 0x31505452u
#define DUCT_HELLO_VERSION 3u
#define DUCT_HELLO_SIZE 32u

struct duct_hello {
    uint32_t max_msg;
    uint32_t recv_depth;
    uint64_t credit_addr;
    uint32_t credit_rkey;
};

/* Serializes h into out (DUCT_HELLO_SIZE bytes). */
void duct_hello_encode(const struct duct_hello *h, uint8_t *out);

/* Parses and validates a handshake. Returns 0, or DUCT_EPROTO for bad size, magic, version or limits. */
int duct_hello_decode(const uint8_t *in, size_t len, struct duct_hello *h);

/* Upper bound accepted for max_msg and depths when decoding a peer handshake. */
#define DUCT_LIMIT_MAX_MSG (1u << 30)
#define DUCT_LIMIT_DEPTH 65535u

#define DUCT_MR_MAGIC 0x31524d44u

struct duct_ctx {
    struct duct_opts opts;
    enum duct_transport transport;
    /* RDMA device state, bound lazily to the device of the first connection or registration. */
    void *rdma;
    /* Registered regions (looked up by key when TCP peers emulate one-sided operations). */
    pthread_mutex_t mr_lock;
    struct duct_mr *mrs;
    uint32_t next_key;
};

struct duct_mr {
    duct_ctx *ctx;
    uint8_t *addr;
    size_t len;
    unsigned access;
    /* Key published in descriptors: RDMA remote key, or registry key (TCP). */
    uint32_t key;
    /* RDMA local key and verbs region; unused on TCP. */
    uint32_t lkey;
    void *ibmr;
    struct duct_mr *next;
};

/* Decoded region descriptor. */
struct duct_desc {
    unsigned access;
    uint64_t addr;
    uint64_t len;
    uint32_t key;
};

/* Encodes and decodes struct duct_mr_desc. decode returns 0 or -EINVAL (bad magic). */
void duct_desc_encode(const struct duct_desc *d, struct duct_mr_desc *out);
int duct_desc_decode(const struct duct_mr_desc *in, struct duct_desc *d);

/*
 * Looks up a region of ctx by key and checks that [addr, addr + len) lies inside it and that it grants
 * `need`. Returns the region's local address for addr, or NULL. Used by the TCP emulation.
 */
uint8_t *duct_mr_resolve(duct_ctx *ctx, uint32_t key, uint64_t addr, uint64_t len, unsigned need);

/* One-sided operation kinds. */
enum duct_rma_op {
    DUCT_RMA_READ = 1,
    DUCT_RMA_WRITE = 2,
    DUCT_RMA_FADD = 3,
    DUCT_RMA_CSWAP = 4,
};

/* A validated one-sided operation, as handed to a transport. */
struct duct_rma {
    enum duct_rma_op op;
    uint8_t *laddr;
    uint32_t lkey;
    size_t len;
    uint64_t raddr;
    uint32_t rkey;
    /* FADD: a = addend. CSWAP: a = compare, b = swap. */
    uint64_t a;
    uint64_t b;
};

struct duct_ep_ops {
    int (*send)(duct_ep *ep, const void *data, size_t len, int timeout_ms);
    int (*send_batch)(duct_ep *ep, const struct duct_msg *msgs, unsigned n, int timeout_ms);
    int (*recv)(duct_ep *ep, struct duct_rx *rx, int timeout_ms);
    int (*release)(duct_ep *ep, struct duct_rx *rx);
    void (*close)(duct_ep *ep);
    int (*fd)(const duct_ep *ep);
    int (*arm)(duct_ep *ep);
    int (*progress)(duct_ep *ep);
    unsigned (*send_space)(duct_ep *ep);
    int (*send_mr)(duct_ep *ep, const duct_mr *mr, size_t off, size_t len, int timeout_ms, uint64_t *seq);
    int (*rma)(duct_ep *ep, const struct duct_rma *op, int timeout_ms, uint64_t *seq);
    uint64_t (*completed)(duct_ep *ep);
    int (*wait)(duct_ep *ep, uint64_t seq, int timeout_ms);
};

struct duct_ep {
    const struct duct_ep_ops *ops;
    duct_ctx *ctx;
    uint32_t peer_max_msg;
};

struct duct_listener_ops {
    int (*accept)(duct_listener *l, int timeout_ms, duct_ep **out);
    int (*port)(const duct_listener *l);
    void (*close)(duct_listener *l);
    int (*fd)(const duct_listener *l);
};

struct duct_listener {
    const struct duct_listener_ops *ops;
    duct_ctx *ctx;
};

/* Monotonic clock in milliseconds. */
static inline int64_t duct_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Converts a relative timeout (-1 = infinite) to an absolute deadline (-1 = none). */
static inline int64_t duct_deadline(int timeout_ms) {
    return timeout_ms < 0 ? -1 : duct_now_ms() + timeout_ms;
}

/* Milliseconds left until deadline for poll(): -1 infinite, otherwise >= 0. */
static inline int duct_remaining_ms(int64_t deadline) {
    if (deadline < 0) {
        return -1;
    }
    int64_t left = deadline - duct_now_ms();
    return left > 0 ? (int)left : 0;
}

/* Transport entry points. */
int duct_tcp_listen(duct_ctx *ctx, const char *addr, const char *port, duct_listener **out);
int duct_tcp_connect(duct_ctx *ctx, const char *addr, const char *port, duct_ep **out);

#ifdef DUCT_HAVE_RDMA
int duct_rdma_available(void);
int duct_rdma_ctx_init(duct_ctx *ctx);
void duct_rdma_ctx_fini(duct_ctx *ctx);
int duct_rdma_listen(duct_ctx *ctx, const char *addr, const char *port, duct_listener **out);
int duct_rdma_connect(duct_ctx *ctx, const char *addr, const char *port, duct_ep **out);
/* Registers mr->addr/len/access with the context's device; sets key, lkey and ibmr. */
int duct_rdma_mr_reg(duct_ctx *ctx, struct duct_mr *mr);
void duct_rdma_mr_dereg(struct duct_mr *mr);
#endif

#endif
