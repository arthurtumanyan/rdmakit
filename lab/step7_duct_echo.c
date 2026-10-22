/*
 * STEP 7 - The same echo over TCP and RDMA with rdmaduct
 * ======================================================
 * Steps 1 to 6 used libibverbs and librdmacm directly: device, protection domain, completion queues, queue pairs,
 * memory registration, posting receives and polling completions all appear in the program. rdmaduct
 * (../rdmaduct) wraps these in a message API with the same calls for both transports:
 *
 *   duct_ctx_create      options: transport, largest message (max_msg), buffers per endpoint (recv_depth,
 *                        send_depth), connection timeout, waiting mode; on RDMA the context owns the device,
 *                        protection domain and registered buffers
 *   duct_listen          a listener; duct_accept waits for one connection and returns its endpoint
 *   duct_connect         an endpoint connected to a listener
 *   duct_send            copies one message (at most duct_ep_peer_max_msg() bytes) into a send buffer and posts it
 *   duct_recv            returns the next message in a receive buffer owned by the library
 *   duct_rx_release      returns that buffer; on RDMA it is posted again as a receive, and the peer is told
 *                        that one more message may be sent (credit-based flow control, see step 8)
 *
 * Message boundaries are kept on both transports: one duct_send produces one duct_recv. On TCP rdmaduct frames
 * messages in the byte stream; on RDMA a message is one SEND into one posted receive buffer.
 *
 * Errors are negative return values: -errno codes (-ETIMEDOUT, -EMSGSIZE, -ECONNREFUSED) and library codes
 * (DUCT_ECLOSED: the peer closed; DUCT_EPROTO: protocol violation). -ETIMEDOUT and -EMSGSIZE leave the endpoint
 * usable; every other error is final for the endpoint. The client below provokes both recoverable errors.
 *
 * With a library built with TLS=1, --tls encrypts every message (TLS 1.3 handshake, then AES-256-GCM per
 * message); the program is otherwise unchanged. Encryption costs 17 bytes of the message limit.
 *
 * Usage:
 *   step7_duct_echo server [--transport tcp|rdma] [--port P] [--once] [--tls --cert FILE --key FILE]
 *   step7_duct_echo client HOST [--transport tcp|rdma] [--port P] [--tls [--cacert FILE]] MESSAGE...
 * --once: the server exits after its first client has disconnected.
 */
#include <errno.h>

#include "duct_common.h"

#define DEFAULT_PORT "7490"

/* TLS options; NULL when not given. */
struct tls_args {
    int on;
    const char *cert;
    const char *key;
    const char *ca;
};

static int parse_tls(int argc, char **argv, int i, struct tls_args *t, int *once) {
    while (i < argc) {
        if (strcmp(argv[i], "--tls") == 0) {
            t->on = 1;
            i++;
        } else if (strcmp(argv[i], "--once") == 0) {
            *once = 1;
            i++;
        } else if (i + 1 < argc && strcmp(argv[i], "--cert") == 0) {
            t->cert = argv[i + 1];
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "--key") == 0) {
            t->key = argv[i + 1];
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "--cacert") == 0) {
            t->ca = argv[i + 1];
            i += 2;
        } else {
            break;
        }
    }
    return i;
}

static duct_ctx *make_ctx(const struct lab_opts *lo, const struct tls_args *t) {
    struct duct_opts o;
    lab_duct_opts(lo, &o);
    o.max_msg = 4096; /* small, so that the oversize send below is cheap */
    o.tls = t->on;
    o.tls_cert = t->cert;
    o.tls_key = t->key;
    o.tls_ca = t->ca;
    duct_ctx *ctx;
    int rc = duct_ctx_create(&o, &ctx);
    if (rc == -ENOTSUP) {
        fprintf(stderr, "duct_ctx_create: encryption needs rdmaduct built with TLS=1 (make TLS=1)\n");
        exit(1);
    }
    lab_check(rc, "duct_ctx_create");
    return ctx;
}

static int run_server(const struct lab_opts *lo, const struct tls_args *t, int once) {
    duct_ctx *ctx = make_ctx(lo, t);
    duct_listener *l;
    lab_check(duct_listen(ctx, NULL, lo->port, &l), "duct_listen");
    printf("listening on port %d (%s%s)\n", duct_listener_port(l),
           lab_transport_name(duct_ctx_transport(ctx)), t->on ? ", encrypted" : "");
    fflush(stdout);
    for (;;) {
        duct_ep *ep;
        lab_check(duct_accept(l, -1, &ep), "duct_accept");
        printf("client connected\n");
        fflush(stdout);
        struct duct_rx rx;
        int rc;
        /* Each received message is sent back unchanged. rx.data stays valid until duct_rx_release(). */
        while ((rc = duct_recv(ep, &rx, -1)) == 0) {
            int src = duct_send(ep, rx.data, rx.len, 5000);
            duct_rx_release(ep, &rx);
            if (src != 0) {
                rc = src;
                break;
            }
        }
        /* DUCT_ECLOSED is the normal end; anything else is reported with its name. */
        printf("client gone: %s\n", duct_strerror(rc));
        fflush(stdout);
        duct_ep_close(ep);
        if (once) {
            break;
        }
    }
    duct_listener_close(l);
    duct_ctx_destroy(ctx);
    return 0;
}

/* Sends one message and prints the echo. Returns 0 when the echo equals the message. */
static int echo(duct_ep *ep, const char *msg) {
    size_t len = strlen(msg);
    lab_check(duct_send(ep, msg, len, 5000), "duct_send");
    struct duct_rx rx;
    lab_check(duct_recv(ep, &rx, 5000), "duct_recv");
    int same = rx.len == len && memcmp(rx.data, msg, len) == 0;
    printf("echo: %.*s\n", (int)rx.len, (const char *)rx.data);
    duct_rx_release(ep, &rx);
    return same ? 0 : 1;
}

static int run_client(const char *host, const struct lab_opts *lo, const struct tls_args *t, int nmsg,
                      char **msgs) {
    duct_ctx *ctx = make_ctx(lo, t);
    duct_ep *ep;
    lab_check(duct_connect(ctx, host, lo->port, &ep), "duct_connect");
    int bad = 0;
    for (int i = 0; i < nmsg; i++) {
        bad |= echo(ep, msgs[i]);
    }

    /* The limit is the smaller max_msg of both sides, less 17 bytes on encrypted endpoints. */
    uint32_t limit = duct_ep_peer_max_msg(ep);
    printf("message limit: %u bytes\n", limit);
    static char big[8192];
    int rc = duct_send(ep, big, (size_t)limit + 1, 1000);
    printf("send of %u bytes: %s\n", limit + 1, duct_strerror(rc));

    /* Nothing was requested, so nothing arrives: the receive ends at its timeout. */
    struct duct_rx rx;
    rc = duct_recv(ep, &rx, 200);
    printf("receive with a 200 ms timeout: %s\n", duct_strerror(rc));

    /* Both errors left the endpoint usable. */
    bad |= echo(ep, "still connected");
    duct_ep_close(ep);
    duct_ctx_destroy(ctx);
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0) ||
        (strcmp(argv[1], "client") == 0 && argc < 3)) {
        fprintf(stderr, "usage: %s server [options] | client HOST [options] MESSAGE...\n", argv[0]);
        return 2;
    }
    struct lab_opts lo = {.transport = DUCT_TRANSPORT_TCP, .wait = DUCT_WAIT_POLL, .port = DEFAULT_PORT};
    struct tls_args t = {0};
    int once = 0;
    int client = strcmp(argv[1], "client") == 0;
    int i = client ? 3 : 2;
    for (int prev = -1; prev != i;) {
        prev = i;
        i = duct_lab_parse(argc, argv, i, &lo);
        i = parse_tls(argc, argv, i, &t, &once);
    }
    if (!client) {
        return run_server(&lo, &t, once);
    }
    return run_client(argv[2], &lo, &t, argc - i, argv + i);
}
