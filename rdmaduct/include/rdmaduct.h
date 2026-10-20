/*
 * rdmaduct.h - message transport over RDMA (RC send/receive) or TCP.
 *
 * The library moves discrete messages of at most `max_msg` bytes between two
 * endpoints. Both transports expose the same API and the same semantics:
 * reliable, ordered, message-preserving delivery.
 *
 * Threading: a context may be shared by threads. An endpoint or listener must
 * be used by one thread at a time; the library takes no locks on the data path.
 *
 * Besides two-sided messages the library offers registered memory regions, zero-copy sends from them,
 * one-sided reads, writes and 64-bit atomics on a peer's region, and pollable descriptors for event
 * loops. On RDMA these map to verbs operations executed by the network adapter. On TCP they are
 * emulated by the peer's library, which services them while its application calls any function on
 * the endpoint (duct_recv, duct_send, duct_progress, ...).
 *
 * Errors: functions returning int return 0 or a non-negative count on success
 * and a negative value on failure. Negative errno values are returned as -errno.
 * Library-specific codes are the DUCT_E* constants below. After a transport
 * error other than -ETIMEDOUT, -EAGAIN and -EINVAL an endpoint is unusable and must be closed.
 */
#ifndef RDMADUCT_H
#define RDMADUCT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Peer closed the connection (orderly shutdown or reset). */
#define DUCT_ECLOSED (-1001)
/* Peer sent data that violates the protocol (bad handshake, bad frame length). */
#define DUCT_EPROTO (-1002)
/* An RDMA work completion finished with an error status. */
#define DUCT_EWC (-1003)
/* No usable RDMA device, or RDMA support is not compiled in. */
#define DUCT_ENODEV (-1004)
/*
 * A one-sided operation addressed memory outside the remote region or without the required access
 * right. The endpoint is unusable afterwards on both transports (on RDMA the queue pair enters the
 * error state).
 */
#define DUCT_EACCESS (-1005)

typedef struct duct_ctx duct_ctx;
typedef struct duct_listener duct_listener;
typedef struct duct_ep duct_ep;
typedef struct duct_mr duct_mr;

enum duct_transport {
    /* RDMA when the library is built with RDMA support and a device exists, otherwise TCP. */
    DUCT_TRANSPORT_AUTO = 0,
    DUCT_TRANSPORT_TCP = 1,
    DUCT_TRANSPORT_RDMA = 2,
};

/* How blocking calls wait for the transport. */
enum duct_wait {
    /* Spin on the completion queue: lowest latency, one CPU core busy while waiting. */
    DUCT_WAIT_POLL = 0,
    /*
     * Sleep in poll() on the completion channel and the connection-manager channel: no CPU use while
     * idle, wake-up latency of a few microseconds more, immediate detection of a peer disconnect.
     * A send blocked on flow control re-checks credits every 200 microseconds, because credit
     * updates arrive as remote memory writes that do not raise a completion event.
     * TCP endpoints always wait in poll(); the setting has no effect there.
     */
    DUCT_WAIT_EVENT = 1,
};

/* Context options. Obtain defaults with duct_opts_default() and modify fields. */
struct duct_opts {
    enum duct_transport transport;
    /* Largest message this side accepts, in bytes. Receive buffers are allocated with this size. */
    uint32_t max_msg;
    /*
     * Receive buffers posted per endpoint (RDMA) = messages that may be received before release.
     * The peer is limited to this many unreleased messages by credit-based flow control.
     */
    uint32_t recv_depth;
    /* Send buffers per endpoint (RDMA) = sends that may be in flight. */
    uint32_t send_depth;
    /* Connection setup timeout in milliseconds; -1 waits indefinitely. */
    int connect_timeout_ms;
    /* Waiting strategy of RDMA endpoints. */
    enum duct_wait wait;
};

/* One message of a batch for duct_send_batch(). */
struct duct_msg {
    const void *data;
    size_t len;
};

/* Received message. data stays valid until duct_rx_release(). */
struct duct_rx {
    const void *data;
    size_t len;
    /* Library-private. */
    uint64_t token;
};

/* Fills o with defaults: AUTO transport, 64 KiB messages, 32 receive and 32 send buffers, 10 s connect timeout, polling wait. */
void duct_opts_default(struct duct_opts *o);

/* Returns a static description of an error code returned by this library. */
const char *duct_strerror(int err);

/* Creates a context. Returns 0 or a negative error. */
int duct_ctx_create(const struct duct_opts *o, duct_ctx **out);

/* Destroys a context. All endpoints and listeners created from it must be closed first. */
void duct_ctx_destroy(duct_ctx *ctx);

/* Returns the transport selected for this context (never AUTO). */
enum duct_transport duct_ctx_transport(const duct_ctx *ctx);

/*
 * Starts listening. addr is a host name or address; NULL listens on all IPv4 addresses.
 * port is a decimal port; "0" selects a free port (see duct_listener_port()).
 */
int duct_listen(duct_ctx *ctx, const char *addr, const char *port, duct_listener **out);

/* Returns the bound port of a listener. */
int duct_listener_port(const duct_listener *l);

/* Waits for and completes one incoming connection. timeout_ms: -1 infinite, 0 poll. Returns -ETIMEDOUT on timeout. */
int duct_accept(duct_listener *l, int timeout_ms, duct_ep **out);

/* Closes a listener. */
void duct_listener_close(duct_listener *l);

/* Connects to a listener. */
int duct_connect(duct_ctx *ctx, const char *addr, const char *port, duct_ep **out);

/* Closes an endpoint; the peer observes DUCT_ECLOSED. Safe to call with NULL. */
void duct_ep_close(duct_ep *ep);

/*
 * Largest message that can be sent on this endpoint: the smaller of the local max_msg and the max_msg
 * announced by the peer during connection setup.
 */
uint32_t duct_ep_peer_max_msg(const duct_ep *ep);

/*
 * Sends one message (len <= duct_ep_peer_max_msg()). The data is copied; the caller's buffer is
 * reusable on return. Blocks while all send buffers are in use. Returns 0, -EMSGSIZE if the
 * message is too large, -ETIMEDOUT, or a transport error. timeout_ms: -1 infinite, 0 non-blocking.
 */
int duct_send(duct_ep *ep, const void *data, size_t len, int timeout_ms);

/*
 * Sends n messages in order. On RDMA endpoints the messages are posted with one doorbell per chunk of
 * up to 16 messages. Semantics otherwise as duct_send(); every message must satisfy the size limit,
 * otherwise nothing is sent and -EMSGSIZE is returned. A timeout may leave a prefix of the batch sent.
 */
int duct_send_batch(duct_ep *ep, const struct duct_msg *msgs, unsigned n, int timeout_ms);

/*
 * Receives one message. Returns 0 and fills rx, -ETIMEDOUT, or a transport error. An endpoint holds at
 * most recv_depth unreleased messages; while all are held, further messages wait in the transport.
 */
int duct_recv(duct_ep *ep, struct duct_rx *rx, int timeout_ms);

/* Returns the receive buffer to the transport. rx must not be used afterwards. */
int duct_rx_release(duct_ep *ep, struct duct_rx *rx);

/* ---- event-loop integration ------------------------------------------------------------------------ */

/*
 * Returns a file descriptor that becomes readable when the endpoint may be able to make progress: a
 * message may be receivable, send space may have become available, a posted operation may have
 * completed, or the peer may have disconnected. The descriptor is owned by the endpoint; it may be added
 * to an epoll or poll set for POLLIN and must not be read or closed by the caller.
 *
 * Protocol: call duct_ep_arm() before waiting on the descriptor. When it returns -EAGAIN, progress is
 * already possible: call duct_recv(), duct_send() or duct_progress() with timeout 0 instead of waiting.
 * Wake-ups may be spurious. While a sender is blocked on flow control (RDMA), the descriptor becomes
 * readable at least every 200 microseconds, because credit updates arrive as remote memory writes that
 * raise no completion event.
 */
int duct_ep_fd(const duct_ep *ep);

/* Prepares the descriptor of duct_ep_fd() for waiting. Returns 0 (wait), -EAGAIN (progress possible
 * now), or a transport error. */
int duct_ep_arm(duct_ep *ep);

/*
 * Performs pending work without blocking: polls completions, returns flow-control credits, flushes queued
 * TCP data, and services one-sided operations that the peer emulates over TCP. Returns 0 or a transport
 * error. duct_recv() and duct_send() perform the same work; duct_progress() is needed only by an
 * endpoint that is the target of one-sided operations over TCP and otherwise idle.
 */
int duct_progress(duct_ep *ep);

/* Returns the number of messages that duct_send() can accept now without waiting. */
unsigned duct_ep_send_space(duct_ep *ep);

/* Returns a descriptor that becomes readable when duct_accept(l, 0, ...) may succeed. Owned by l. */
int duct_listener_fd(const duct_listener *l);

/* ---- registered memory ----------------------------------------------------------------------------- */

/* Access rights a region grants to peers. Local reads and writes are always allowed. */
enum duct_access {
    DUCT_ACCESS_LOCAL = 0,
    DUCT_ACCESS_REMOTE_READ = 1u << 0,
    DUCT_ACCESS_REMOTE_WRITE = 1u << 1,
    DUCT_ACCESS_REMOTE_ATOMIC = 1u << 2,
};

/* Size of a region descriptor. */
#define DUCT_MR_DESC_SIZE 32u

/*
 * Region descriptor: address, length, key and rights of a registered region, to be sent to a peer that
 * performs one-sided operations on it. The layout is the same on both transports and opaque to callers.
 */
struct duct_mr_desc {
    uint8_t bytes[DUCT_MR_DESC_SIZE];
};

/*
 * Registers [addr, addr + len) with the context. access is a combination of enum duct_access values.
 * The memory must stay valid until duct_mr_dereg(). On RDMA the context binds to an RDMA device on the
 * first registration if no connection has bound it yet (the first device known to librdmacm); later
 * connections must use the same device. Returns 0, -EINVAL, -ENOMEM, or an RDMA error.
 */
int duct_mr_reg(duct_ctx *ctx, void *addr, size_t len, unsigned access, duct_mr **out);

/*
 * Deregisters a region. Operations that use it must have completed. One-sided operations that peers
 * issue afterwards fail with DUCT_EACCESS at the peer. Safe to call with NULL.
 */
void duct_mr_dereg(duct_mr *mr);

/* Returns the start address of a region. */
void *duct_mr_addr(const duct_mr *mr);

/* Writes the descriptor of a region into out. */
void duct_mr_export(const duct_mr *mr, struct duct_mr_desc *out);

/* ---- posted operations ----------------------------------------------------------------------------- */

/*
 * The functions below post an operation and return without waiting for it to finish. Each posted
 * operation receives a sequence number (written to *seq when seq is not NULL); numbers increase by one
 * per operation and endpoint, starting at 1. Operations on one endpoint complete in posting order, so
 * duct_ep_completed() is the number of the latest completed operation and every smaller number has
 * completed too. timeout_ms bounds the wait for send space and flow-control credits before posting.
 * Buffers named by an operation must not be modified (sources) or read (destinations) before it has
 * completed. Range violations detected locally return -EINVAL and leave the endpoint usable.
 */

/*
 * Sends [off, off + len) of a registered region as one message without copying it (RDMA); on TCP the
 * bytes are written to the socket or, if the socket is full, copied into the send queue. The receiver
 * sees an ordinary message. len <= duct_ep_peer_max_msg().
 */
int duct_send_mr(duct_ep *ep, const duct_mr *mr, size_t off, size_t len, int timeout_ms, uint64_t *seq);

/* Reads len bytes at offset roff of the remote region into [loff, loff + len) of local. */
int duct_read(duct_ep *ep, duct_mr *local, size_t loff, const struct duct_mr_desc *remote, uint64_t roff,
              size_t len, int timeout_ms, uint64_t *seq);

/* Writes [loff, loff + len) of local to offset roff of the remote region. */
int duct_write(duct_ep *ep, const duct_mr *local, size_t loff, const struct duct_mr_desc *remote,
               uint64_t roff, size_t len, int timeout_ms, uint64_t *seq);

/*
 * Atomically adds `add` to the 64-bit word at offset roff of the remote region and stores the previous
 * value at [loff, loff + 8) of local. roff and loff must be multiples of 8. The remote word is a native
 * integer of the target host; peers must have the same byte order.
 */
int duct_atomic_fadd(duct_ep *ep, duct_mr *local, size_t loff, const struct duct_mr_desc *remote,
                     uint64_t roff, uint64_t add, int timeout_ms, uint64_t *seq);

/*
 * Atomically replaces the 64-bit word at offset roff of the remote region by `swap` if it equals
 * `compare`, and stores the previous value at [loff, loff + 8) of local. Alignment as duct_atomic_fadd().
 */
int duct_atomic_cswap(duct_ep *ep, duct_mr *local, size_t loff, const struct duct_mr_desc *remote,
                      uint64_t roff, uint64_t compare, uint64_t swap, int timeout_ms, uint64_t *seq);

/* Returns the sequence number of the latest completed posted operation (0 before the first). */
uint64_t duct_ep_completed(duct_ep *ep);

/*
 * Waits until the operation with sequence number seq has completed. Returns 0, -ETIMEDOUT, or a
 * transport error (DUCT_EACCESS for a rejected one-sided operation).
 */
int duct_wait(duct_ep *ep, uint64_t seq, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
