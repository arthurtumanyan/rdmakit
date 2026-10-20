/*
 * duct_bench.c - transport benchmark: round-trip latency and one-way streaming throughput.
 *
 * Usage:
 *   duct_bench server   [common options]
 *   duct_bench client   [common options] --sizes LIST [--pingpong N] [--stream N] [--out FILE]
 *   duct_bench loopback [common options] --sizes LIST [--pingpong N] [--stream N] [--out FILE]
 *
 * Common options: --transport tcp|rdma  --wait poll|event  --addr ADDRESS  --port PORT  --max-msg BYTES
 *
 * The server answers three message types, selected by the first payload byte:
 *   'E'  echo: the message is sent back unchanged
 *   'S'  stream data: counted, no reply
 *   'F'  flush: answered with a 1-byte acknowledgement after all earlier messages were received
 *
 * For each message size the client runs
 *   ping-pong  N single round trips (send 'E', wait for the echo), each timed; reports mean and
 *              percentiles of the round-trip time and the payload rate in both directions
 *   stream     N 'S' messages sent in batches of 16 with duct_send_batch(), then one 'F'; reports messages
 *              per second and payload rate for the time until the acknowledgement arrives
 * "loopback" runs the server in a thread of the client process (both use the same address).
 *
 * Output: JSON, {"tool": "duct_bench", "transport": ..., "wait": ..., "results": [...]}.
 */
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdmaduct.h"

struct options {
    enum duct_transport transport;
    enum duct_wait wait;
    const char *addr;
    const char *port;
    uint32_t max_msg;
    int sizes[32];
    int nsizes;
    unsigned pingpong;
    unsigned stream;
    const char *out;
};

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static duct_ctx *make_ctx(const struct options *o) {
    struct duct_opts ro;
    duct_opts_default(&ro);
    ro.transport = o->transport;
    ro.wait = o->wait;
    ro.max_msg = o->max_msg;
    ro.recv_depth = 64;
    ro.send_depth = 64;
    duct_ctx *ctx = NULL;
    int rc = duct_ctx_create(&ro, &ctx);
    if (rc != 0) {
        fprintf(stderr, "context: %s\n", duct_strerror(rc));
        exit(1);
    }
    return ctx;
}

/* ---- server ---------------------------------------------------------------------------------------- */

static void serve_connection(duct_ep *ep) {
    struct duct_rx rx;
    while (duct_recv(ep, &rx, -1) == 0) {
        char type = rx.len > 0 ? ((const char *)rx.data)[0] : 0;
        int rc = 0;
        if (type == 'E') {
            rc = duct_send(ep, rx.data, rx.len, -1);
        } else if (type == 'F') {
            rc = duct_send(ep, "A", 1, -1);
        }
        duct_rx_release(ep, &rx);
        if (rc != 0) {
            break;
        }
    }
}

struct server_arg {
    const struct options *o;
    duct_ctx *ctx;
    duct_listener *listener;
    int once;
};

static void *server_main(void *p) {
    struct server_arg *a = p;
    do {
        duct_ep *ep = NULL;
        if (duct_accept(a->listener, -1, &ep) != 0) {
            break;
        }
        serve_connection(ep);
        duct_ep_close(ep);
    } while (!a->once);
    return NULL;
}

/* ---- client ---------------------------------------------------------------------------------------- */

static int run_client(const struct options *o, duct_ctx *ctx, FILE *out) {
    duct_ep *ep = NULL;
    int rc = duct_connect(ctx, o->addr, o->port, &ep);
    if (rc != 0) {
        fprintf(stderr, "connect: %s\n", duct_strerror(rc));
        return 1;
    }
    uint8_t *buf = malloc(o->max_msg);
    memset(buf, 0xa5, o->max_msg);
    double *rtt = malloc(sizeof(double) * o->pingpong);
    struct duct_msg batch[16];
    fprintf(out,
            "{\"tool\": \"duct_bench\", \"transport\": \"%s\", \"wait\": \"%s\", \"pingpong_max\": %u, "
            "\"stream_max\": %u, \"results\": [\n",
            duct_ctx_transport(ctx) == DUCT_TRANSPORT_RDMA ? "rdma" : "tcp",
            o->wait == DUCT_WAIT_EVENT ? "event" : "poll", o->pingpong, o->stream);
    for (int si = 0; si < o->nsizes; si++) {
        size_t size = (size_t)o->sizes[si];
        /* Large messages: bound the transferred volume (64 MiB of ping-pong, 256 MiB of streaming per size). */
        unsigned pp = o->pingpong;
        unsigned pp_cap = (unsigned)((64u << 20) / (size ? size : 1));
        if (pp > pp_cap) {
            pp = pp_cap < 200 ? 200 : pp_cap;
        }
        unsigned st = o->stream;
        unsigned st_cap = (unsigned)((256u << 20) / (size ? size : 1));
        if (st > st_cap) {
            st = st_cap < 256 ? 256 : st_cap;
        }
        struct duct_rx rx;
        buf[0] = 'E';
        for (unsigned i = 0; i < 200; i++) { /* warm-up */
            if (duct_send(ep, buf, size, 10000) != 0 || duct_recv(ep, &rx, 10000) != 0) {
                fprintf(stderr, "warm-up failed\n");
                return 1;
            }
            duct_rx_release(ep, &rx);
        }
        double t_all = now_us();
        for (unsigned i = 0; i < pp; i++) {
            double t0 = now_us();
            if (duct_send(ep, buf, size, 10000) != 0 || duct_recv(ep, &rx, 10000) != 0) {
                fprintf(stderr, "ping-pong failed\n");
                return 1;
            }
            rtt[i] = now_us() - t0;
            duct_rx_release(ep, &rx);
        }
        t_all = now_us() - t_all;
        double sum = 0;
        for (unsigned i = 0; i < pp; i++) {
            sum += rtt[i];
        }
        qsort(rtt, pp, sizeof(double), cmp_double);
        double p50 = rtt[pp / 2];
        double p99 = rtt[(size_t)(pp * 0.99)];
        double p999 = rtt[(size_t)(pp * 0.999)];

        buf[0] = 'S';
        for (int i = 0; i < 16; i++) {
            batch[i] = (struct duct_msg){.data = buf, .len = size};
        }
        double ts = now_us();
        for (unsigned sent = 0; sent < st;) {
            unsigned n = st - sent < 16 ? st - sent : 16;
            if (duct_send_batch(ep, batch, n, 10000) != 0) {
                fprintf(stderr, "stream send failed\n");
                return 1;
            }
            sent += n;
        }
        buf[0] = 'F';
        if (duct_send(ep, buf, size, 10000) != 0 || duct_recv(ep, &rx, 30000) != 0) {
            fprintf(stderr, "stream flush failed\n");
            return 1;
        }
        duct_rx_release(ep, &rx);
        ts = now_us() - ts;

        fprintf(
            out,
            "%s  {\"size\": %zu, \"pingpong_count\": %u, \"stream_count\": %u, \"rtt_us\": {\"mean\": %.2f, "
            "\"p50\": %.2f, \"p99\": %.2f, \"p999\": %.2f, "
            "\"min\": %.2f, "
            "\"max\": %.2f}, \"pingpong_mb_s\": %.2f, \"stream_msgs_per_s\": %.0f, \"stream_mb_s\": %.2f}",
            si ? ",\n" : "", size, pp, st, sum / pp, p50, p99, p999, rtt[0], rtt[pp - 1],
            2.0 * (double)size * pp / t_all, st / (ts / 1e6), (double)size * st / ts);
        fflush(out);
    }
    fprintf(out, "\n]}\n");
    free(buf);
    free(rtt);
    duct_ep_close(ep);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0 &&
                     strcmp(argv[1], "loopback") != 0)) {
        fprintf(stderr, "usage: %s server|client|loopback [options]\n", argv[0]);
        return 2;
    }
    struct options o = {.transport = DUCT_TRANSPORT_TCP,
                        .wait = DUCT_WAIT_POLL,
                        .addr = NULL,
                        .port = "7700",
                        .max_msg = 1u << 20,
                        .pingpong = 5000,
                        .stream = 20000};
    o.nsizes = 0;
    static const struct option lo[] = {
        {"transport", required_argument, NULL, 't'}, {"wait", required_argument, NULL, 'w'},
        {"addr", required_argument, NULL, 'a'},      {"port", required_argument, NULL, 'p'},
        {"max-msg", required_argument, NULL, 'm'},   {"sizes", required_argument, NULL, 's'},
        {"pingpong", required_argument, NULL, 'n'},  {"stream", required_argument, NULL, 'x'},
        {"out", required_argument, NULL, 'o'},       {NULL, 0, NULL, 0}};
    optind = 2;
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 't':
            o.transport = strcmp(optarg, "rdma") == 0 ? DUCT_TRANSPORT_RDMA : DUCT_TRANSPORT_TCP;
            break;
        case 'w':
            o.wait = strcmp(optarg, "event") == 0 ? DUCT_WAIT_EVENT : DUCT_WAIT_POLL;
            break;
        case 'a':
            o.addr = optarg;
            break;
        case 'p':
            o.port = optarg;
            break;
        case 'm':
            o.max_msg = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 's': {
            char *copy = strdup(optarg);
            for (char *t = strtok(copy, ","); t != NULL && o.nsizes < 32; t = strtok(NULL, ",")) {
                o.sizes[o.nsizes++] = atoi(t);
            }
            free(copy);
            break;
        }
        case 'n':
            o.pingpong = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'x':
            o.stream = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'o':
            o.out = optarg;
            break;
        default:
            return 2;
        }
    }
    duct_ctx *ctx = make_ctx(&o);
    if (strcmp(argv[1], "server") == 0) {
        struct server_arg a = {.o = &o, .ctx = ctx};
        int rc = duct_listen(ctx, o.addr, o.port, &a.listener);
        if (rc != 0) {
            fprintf(stderr, "listen: %s\n", duct_strerror(rc));
            return 1;
        }
        printf("duct_bench server: port %d\n", duct_listener_port(a.listener));
        fflush(stdout);
        server_main(&a);
        return 0;
    }
    if (o.nsizes == 0 || o.addr == NULL) {
        fprintf(stderr, "client and loopback need --sizes and --addr\n");
        return 2;
    }
    FILE *out = o.out ? fopen(o.out, "w") : stdout;
    if (out == NULL) {
        perror(o.out);
        return 1;
    }
    int rc;
    if (strcmp(argv[1], "loopback") == 0) {
        struct server_arg a = {.o = &o, .ctx = ctx, .once = 1};
        rc = duct_listen(ctx, o.addr, "0", &a.listener);
        if (rc != 0) {
            fprintf(stderr, "listen: %s\n", duct_strerror(rc));
            return 1;
        }
        char port[16];
        snprintf(port, sizeof(port), "%d", duct_listener_port(a.listener));
        o.port = port;
        pthread_t th;
        pthread_create(&th, NULL, server_main, &a);
        rc = run_client(&o, ctx, out);
        pthread_join(th, NULL);
        duct_listener_close(a.listener);
    } else {
        rc = run_client(&o, ctx, out);
    }
    if (o.out) {
        fclose(out);
    }
    duct_ctx_destroy(ctx);
    return rc;
}
