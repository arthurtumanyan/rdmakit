/*
 * STEP 8 - Flow control, back-pressure and batching
 * =================================================
 * A receiver can hold only as many messages as it has receive buffers. On RDMA a SEND that finds no posted
 * receive fails at the receiver's adapter (receiver not ready, step 3's exercise 2); rdmaduct therefore gives the
 * sender one credit per receive buffer of the peer (recv_depth) and takes one per message. The receiver returns
 * credits as the application releases messages (duct_rx_release): it writes its release count into a word of
 * the sender's memory with an RDMA WRITE. A sender without credits waits; with timeout 0 it gets -ETIMEDOUT at
 * once, and nothing is sent. On TCP the kernel's socket buffers and rdmaduct's send queue (send_depth full
 * messages) play the same role: the sender waits when both are full.
 *
 *   duct_ep_send_space(ep)   messages duct_send() accepts now without waiting
 *   duct_send_batch(ep, ...) sends several messages in order; on RDMA they are posted as one chain with one
 *                            doorbell (one write to the adapter) per 16 messages, on TCP written with one
 *                            sendmsg() per group, so the per-message cost of small messages falls
 *
 * The client runs two parts over separate connections. A message's first byte tells the server what to do:
 *   1. back-pressure: the server waits one second before it reads anything. The client sends 64 KiB messages
 *      with timeout 0 and prints the send space until the first -ETIMEDOUT: on RDMA at most the server's
 *      recv_depth (32; a credit whose return the server has not yet written back is missing), on TCP what the
 *      socket buffers and the send queue hold. Then it sends 10 more with
 *      blocking sends, which proceed as the server drains, and checks that the server counted every message.
 *   2. throughput: for each message size, a stream of messages sent one by one and in batches of 16; the
 *      server counts them and answers the end marker, so the time covers delivery, not only posting. On
 *      Soft-RoCE batching gains little: the kernel processes every message, and that costs more than the posts
 *      it saves. On TCP the gain comes from fewer system calls.
 *
 * Usage:
 *   step8_duct_flow server [--transport tcp|rdma] [--port P]
 *   step8_duct_flow client HOST [--transport tcp|rdma] [--port P] [--mib N]
 * --mib: data per measurement (default 64 MiB; at least 2,000 messages each).
 */
#include <errno.h>
#include <unistd.h>

#include "duct_common.h"

#define DEFAULT_PORT "7491"
#define MAX_MSG 65536u
#define DEPTH 32u
#define MODE_PRESSURE 'P'
#define MODE_STREAM 'S'
#define END 'E' /* end marker; the server answers with the number of messages before it */

static duct_ctx *make_ctx(const struct lab_opts *lo) {
    struct duct_opts o;
    lab_duct_opts(lo, &o);
    o.max_msg = MAX_MSG;
    o.recv_depth = DEPTH;
    o.send_depth = DEPTH;
    duct_ctx *ctx;
    lab_check(duct_ctx_create(&o, &ctx), "duct_ctx_create");
    return ctx;
}

/* Counts messages until the end marker and answers with the count (8 bytes). */
static void serve(duct_ep *ep) {
    struct duct_rx rx;
    if (duct_recv(ep, &rx, -1) != 0) {
        return;
    }
    char mode = rx.len > 0 ? ((const char *)rx.data)[0] : 0;
    duct_rx_release(ep, &rx);
    if (mode == MODE_PRESSURE) {
        sleep(1); /* nothing is released meanwhile: the client runs out of credits or buffer space */
    }
    uint64_t count = 0;
    for (;;) {
        if (duct_recv(ep, &rx, -1) != 0) {
            return;
        }
        int end = rx.len == 1 && ((const char *)rx.data)[0] == END;
        duct_rx_release(ep, &rx);
        if (end) {
            break;
        }
        count++;
    }
    duct_send(ep, &count, sizeof(count), 5000);
}

static int run_server(const struct lab_opts *lo) {
    duct_ctx *ctx = make_ctx(lo);
    duct_listener *l;
    lab_check(duct_listen(ctx, NULL, lo->port, &l), "duct_listen");
    printf("listening on port %d (%s)\n", duct_listener_port(l), lab_transport_name(duct_ctx_transport(ctx)));
    fflush(stdout);
    duct_ep *ep;
    int rc;
    while ((rc = duct_accept(l, -1, &ep)) == 0) {
        serve(ep);
        duct_ep_close(ep);
    }
    lab_check(rc, "duct_accept");
    return 1;
}

/* Sends the end marker and returns the server's count. */
static uint64_t finish(duct_ep *ep) {
    char end = END;
    lab_check(duct_send(ep, &end, 1, 10000), "duct_send");
    struct duct_rx rx;
    lab_check(duct_recv(ep, &rx, 30000), "duct_recv");
    uint64_t count = 0;
    memcpy(&count, rx.data, rx.len == sizeof(count) ? sizeof(count) : 0);
    duct_rx_release(ep, &rx);
    return count;
}

static duct_ep *connect_mode(duct_ctx *ctx, const char *host, const char *port, char mode) {
    duct_ep *ep;
    lab_check(duct_connect(ctx, host, port, &ep), "duct_connect");
    lab_check(duct_send(ep, &mode, 1, 5000), "duct_send");
    return ep;
}

static int pressure(duct_ctx *ctx, const char *host, const char *port) {
    static char msg[MAX_MSG];
    duct_ep *ep = connect_mode(ctx, host, port, MODE_PRESSURE);
    printf("back-pressure (the server reads nothing for 1 s):\n");
    unsigned accepted = 0;
    for (;;) {
        unsigned space = duct_ep_send_space(ep);
        int rc = duct_send(ep, msg, sizeof(msg), 0);
        if (rc == -ETIMEDOUT) {
            printf("  message %u: send space %u, non-blocking send: %s\n", accepted + 1, space,
                   duct_strerror(rc));
            break;
        }
        lab_check(rc, "duct_send");
        if (accepted < 3 || accepted % 8 == 7) {
            printf("  message %u: send space %u before, accepted\n", accepted + 1, space);
        }
        accepted++;
    }
    printf("  %u messages of 64 KiB accepted without the server reading\n", accepted);
    double t0 = lab_now();
    for (int i = 0; i < 10; i++) {
        lab_check(duct_send(ep, msg, sizeof(msg), 10000), "duct_send");
    }
    printf("  10 blocking sends completed after %.0f ms, when the server started reading\n",
           (lab_now() - t0) * 1e3);
    uint64_t count = finish(ep);
    printf("  server received %llu of %u messages\n", (unsigned long long)count, accepted + 10);
    duct_ep_close(ep);
    return count == accepted + 10 ? 0 : 1;
}

/* One measurement: n messages of size bytes, in batches of `batch`. Returns 0 when all arrived. */
static int stream(duct_ctx *ctx, const char *host, const char *port, size_t size, unsigned n,
                  unsigned batch) {
    static char buf[MAX_MSG];
    struct duct_msg msgs[16];
    for (unsigned i = 0; i < 16; i++) {
        msgs[i] = (struct duct_msg){.data = buf, .len = size};
    }
    duct_ep *ep = connect_mode(ctx, host, port, MODE_STREAM);
    double t0 = lab_now();
    for (unsigned sent = 0; sent < n;) {
        unsigned k = n - sent < batch ? n - sent : batch;
        if (k == 1) {
            lab_check(duct_send(ep, buf, size, 10000), "duct_send");
        } else {
            lab_check(duct_send_batch(ep, msgs, k, 10000), "duct_send_batch");
        }
        sent += k;
    }
    uint64_t count = finish(ep);
    double s = lab_now() - t0;
    printf("  %6zu B  batch %2u  %9.0f msg/s  %8.1f MiB/s%s\n", size, batch, n / s,
           n * (double)size / s / 1048576.0, count == n ? "" : "  (messages lost)");
    duct_ep_close(ep);
    return count == n ? 0 : 1;
}

static int run_client(const char *host, const struct lab_opts *lo, unsigned mib) {
    duct_ctx *ctx = make_ctx(lo);
    int bad = pressure(ctx, host, lo->port);
    printf("throughput (%s, delivery to the server application):\n",
           lab_transport_name(duct_ctx_transport(ctx)));
    static const size_t sizes[] = {64, 1024, 16384, 65536};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        double want = (double)mib * 1048576.0 / (double)sizes[i];
        unsigned n = want < 2000 ? 2000 : want > 2e6 ? 2000000 : (unsigned)want;
        bad |= stream(ctx, host, lo->port, sizes[i], n, 1);
        bad |= stream(ctx, host, lo->port, sizes[i], n, 16);
    }
    duct_ctx_destroy(ctx);
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0) ||
        (strcmp(argv[1], "client") == 0 && argc < 3)) {
        fprintf(stderr, "usage: %s server [options] | client HOST [options] [--mib N]\n", argv[0]);
        return 2;
    }
    struct lab_opts lo = {.transport = DUCT_TRANSPORT_TCP, .wait = DUCT_WAIT_POLL, .port = DEFAULT_PORT};
    int client = strcmp(argv[1], "client") == 0;
    int i = duct_lab_parse(argc, argv, client ? 3 : 2, &lo);
    unsigned mib = 64;
    if (i + 1 < argc && strcmp(argv[i], "--mib") == 0) {
        mib = (unsigned)strtoul(argv[i + 1], NULL, 10);
        i += 2;
    }
    if (i != argc || mib == 0) {
        fprintf(stderr, "unexpected argument: %s\n", i < argc ? argv[i] : "--mib 0");
        return 2;
    }
    return client ? run_client(argv[2], &lo, mib) : run_server(&lo);
}
