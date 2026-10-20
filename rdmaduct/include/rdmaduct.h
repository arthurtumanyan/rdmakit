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
 * Errors: functions returning int return 0 or a non-negative count on success
 * and a negative value on failure. Negative errno values are returned as -errno.
 * Library-specific codes are the DUCT_E* constants below. After a transport
 * error other than -ETIMEDOUT an endpoint is unusable and must be closed.
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

typedef struct duct_ctx duct_ctx;
typedef struct duct_listener duct_listener;
typedef struct duct_ep duct_ep;

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
 * Receives one message. Returns 0 and fills rx, -ETIMEDOUT, or a transport error. TCP endpoints
 * hold at most one unreleased message (-EBUSY otherwise); RDMA endpoints up to recv_depth.
 */
int duct_recv(duct_ep *ep, struct duct_rx *rx, int timeout_ms);

/* Returns the receive buffer to the transport. rx must not be used afterwards. */
int duct_rx_release(duct_ep *ep, struct duct_rx *rx);

#ifdef __cplusplus
}
#endif

#endif
