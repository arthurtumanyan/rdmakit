/*
 * STEP 10 - Zero-copy sends from registered memory
 * ================================================
 * duct_send() copies the message into one of the endpoint's registered send buffers, so the caller's buffer is
 * free again when the call returns. For large messages that copy costs CPU time and memory bandwidth on top of
 * what the adapter moves. duct_send_mr() sends straight from memory the application registered itself (step 2's
 * ibv_reg_mr, here duct_mr_reg): the adapter reads the bytes from the application's buffer.
 *
 * The price is a rule the copy hid: the buffer must not change until the adapter has read it. Each posted
 * operation gets a sequence number; duct_ep_completed() is the latest finished one, and duct_wait(ep, seq, ...)
 * waits for one. The client below fills a ring of SLOTS buffers inside one registered region and, before it
 * writes a slot again, waits for the send that last used it. Writing a slot whose send has not completed would
 * send partly new bytes; the server checks every message, so such a bug shows up as corrupt messages.
 *
 * On TCP duct_send_mr() writes from the region to the socket, and copies into the send queue only what the
 * socket does not take; the completion then follows at once.
 *
 * The client prints throughput and its own CPU time per GiB for both ways of sending, for messages from 4 KiB to
 * 1 MiB. The CPU time is the comparison that matters: the copy runs on the sending core. On TCP and on Soft-RoCE
 * the kernel still copies every message into network buffers (rxe builds its packets in software), so only the
 * user-space copy is saved and the difference is small; with an RDMA adapter, which reads the buffer by DMA, the
 * zero-copy send leaves no copy on the sending CPU at all.
 *
 * Usage:
 *   step10_duct_zero_copy server [--transport tcp|rdma] [--port P]
 *   step10_duct_zero_copy client HOST [--transport tcp|rdma] [--port P] [--mib N]
 * --mib: data per measurement (default 256 MiB).
 */
#include <errno.h>
#include <sys/resource.h>

#include "duct_common.h"

#define DEFAULT_PORT "7493"
#define MAX_MSG (1u << 20)
#define SLOTS 8u
#define END 0xffffffffffffffffull /* message number of the end marker */

static duct_ctx *make_ctx(const struct lab_opts *lo) {
    struct duct_opts o;
    lab_duct_opts(lo, &o);
    o.max_msg = MAX_MSG;
    o.recv_depth = 16;
    o.send_depth = 16;
    duct_ctx *ctx;
    lab_check(duct_ctx_create(&o, &ctx), "duct_ctx_create");
    return ctx;
}

/* Message content: the message number in the first 8 bytes, and the byte (number & 0xff) at every 4 KiB and at
 * the end, so that a message sent while its slot was rewritten fails the check. */
static void fill(uint8_t *p, size_t len, uint64_t n) {
    memcpy(p, &n, sizeof(n));
    for (size_t off = 4096; off < len; off += 4096) {
        p[off] = (uint8_t)n;
    }
    p[len - 1] = (uint8_t)n;
}

static int valid(const uint8_t *p, size_t len, uint64_t *n) {
    memcpy(n, p, sizeof(*n));
    for (size_t off = 4096; off < len; off += 4096) {
        if (p[off] != (uint8_t)*n) {
            return 0;
        }
    }
    return p[len - 1] == (uint8_t)*n;
}

/* Receives until the end marker and answers with {messages, corrupt messages}. */
static void serve(duct_ep *ep) {
    uint64_t result[2] = {0, 0};
    struct duct_rx rx;
    while (duct_recv(ep, &rx, -1) == 0) {
        uint64_t n = 0;
        int ok = rx.len >= 8 && valid(rx.data, rx.len, &n);
        duct_rx_release(ep, &rx);
        if (ok && n == END) {
            duct_send(ep, result, sizeof(result), 5000);
            return;
        }
        result[0]++;
        result[1] += !ok;
    }
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

static double cpu_seconds(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6 + (double)ru.ru_stime.tv_sec +
           (double)ru.ru_stime.tv_usec * 1e-6;
}

/* Sends n messages of len bytes, copied (zero_copy = 0) or from the registered ring. Returns 0 when the server
 * received all of them intact. */
static int measure(duct_ctx *ctx, const char *host, const char *port, duct_mr *mr, size_t len, unsigned n,
                   int zero_copy) {
    uint8_t *ring = duct_mr_addr(mr);
    uint64_t slot_seq[SLOTS] = {0}; /* sequence number of the send that last used each slot */
    duct_ep *ep;
    lab_check(duct_connect(ctx, host, port, &ep), "duct_connect");
    double t0 = lab_now();
    double c0 = cpu_seconds();
    for (uint64_t i = 0; i < n; i++) {
        unsigned s = (unsigned)(i % SLOTS);
        uint8_t *p = ring + (size_t)s * MAX_MSG;
        if (zero_copy && slot_seq[s] != 0) {
            lab_check(duct_wait(ep, slot_seq[s], 10000), "duct_wait"); /* the adapter has read the slot */
        }
        fill(p, len, i);
        if (zero_copy) {
            lab_check(duct_send_mr(ep, mr, (size_t)s * MAX_MSG, len, 10000, &slot_seq[s]), "duct_send_mr");
        } else {
            lab_check(duct_send(ep, p, len, 10000), "duct_send"); /* copied: the slot is free at once */
        }
    }
    uint8_t end[8];
    fill(end, sizeof(end), END);
    lab_check(duct_send(ep, end, sizeof(end), 10000), "duct_send");
    struct duct_rx rx;
    lab_check(duct_recv(ep, &rx, 60000), "duct_recv");
    uint64_t result[2] = {0, 1};
    if (rx.len == sizeof(result)) {
        memcpy(result, rx.data, sizeof(result));
    }
    duct_rx_release(ep, &rx);
    double s = lab_now() - t0;
    double cpu = cpu_seconds() - c0;
    double gib = (double)n * (double)len / (1u << 30);
    printf("  %7zu B  %-9s %8.0f MiB/s  client CPU %.3f s per GiB", len, zero_copy ? "zero-copy" : "copy",
           gib * 1024 / s, cpu / gib);
    int ok = result[0] == n && result[1] == 0;
    if (!ok) {
        printf("  server: %llu messages, %llu corrupt", (unsigned long long)result[0],
               (unsigned long long)result[1]);
    }
    printf("\n");
    duct_ep_close(ep);
    return ok ? 0 : 1;
}

static int run_client(const char *host, const struct lab_opts *lo, unsigned mib) {
    duct_ctx *ctx = make_ctx(lo);
    static uint8_t ring[SLOTS * MAX_MSG];
    duct_mr *mr;
    /* Local access only: the peer never reads or writes this memory, the local adapter reads it for sends. */
    lab_check(duct_mr_reg(ctx, ring, sizeof(ring), DUCT_ACCESS_LOCAL, &mr), "duct_mr_reg");
    printf("%s, %u MiB per measurement:\n", lab_transport_name(duct_ctx_transport(ctx)), mib);
    static const size_t sizes[] = {4096, 65536, 262144, MAX_MSG};
    int bad = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        double want = (double)mib * 1048576.0 / (double)sizes[i];
        unsigned n = want < 64 ? 64 : (unsigned)want;
        bad |= measure(ctx, host, lo->port, mr, sizes[i], n, 0);
        bad |= measure(ctx, host, lo->port, mr, sizes[i], n, 1);
    }
    duct_mr_dereg(mr);
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
    unsigned mib = 256;
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
