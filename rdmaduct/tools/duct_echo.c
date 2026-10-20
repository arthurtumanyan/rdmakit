/*
 * duct_echo.c - echo server and round-trip client for manual and cross-host tests.
 *
 * Usage:
 *   duct_echo server [--transport tcp|rdma] [--addr A] [--port P]
 *   duct_echo client [--transport tcp|rdma] --addr A [--port P] [--count N] [--size S]
 *
 * The server echoes every message and serves connections one after another until
 * interrupted. The client sends N messages of S bytes, verifies each echo, and
 * prints the mean round-trip time and payload throughput.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdmaduct.h"

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int run_server(duct_ctx *ctx, const char *addr, const char *port) {
    duct_listener *l;
    int rc = duct_listen(ctx, addr, port, &l);
    if (rc != 0) {
        fprintf(stderr, "listen: %s\n", duct_strerror(rc));
        return 1;
    }
    printf("listening on port %d (%s)\n", duct_listener_port(l),
           duct_ctx_transport(ctx) == DUCT_TRANSPORT_RDMA ? "rdma" : "tcp");
    fflush(stdout);
    for (;;) {
        duct_ep *ep;
        rc = duct_accept(l, -1, &ep);
        if (rc != 0) {
            fprintf(stderr, "accept: %s\n", duct_strerror(rc));
            continue;
        }
        unsigned long n = 0;
        struct duct_rx rx;
        while ((rc = duct_recv(ep, &rx, -1)) == 0) {
            int src = duct_send(ep, rx.data, rx.len, -1);
            duct_rx_release(ep, &rx);
            if (src != 0) {
                rc = src;
                break;
            }
            n++;
        }
        printf("connection ended after %lu messages: %s\n", n, duct_strerror(rc));
        fflush(stdout);
        duct_ep_close(ep);
    }
}

static int run_client(duct_ctx *ctx, const char *addr, const char *port, unsigned count, unsigned size) {
    duct_ep *ep;
    int rc = duct_connect(ctx, addr, port, &ep);
    if (rc != 0) {
        fprintf(stderr, "connect: %s\n", duct_strerror(rc));
        return 1;
    }
    unsigned char *msg = malloc(size ? size : 1);
    for (unsigned i = 0; i < size; i++) {
        msg[i] = (unsigned char)(i * 131u + 7u);
    }
    double t0 = now_us();
    for (unsigned i = 0; i < count; i++) {
        struct duct_rx rx;
        rc = duct_send(ep, msg, size, 5000);
        if (rc == 0) {
            rc = duct_recv(ep, &rx, 5000);
        }
        if (rc != 0) {
            fprintf(stderr, "message %u: %s\n", i, duct_strerror(rc));
            return 1;
        }
        if (rx.len != size || memcmp(rx.data, msg, size) != 0) {
            fprintf(stderr, "message %u: echo mismatch\n", i);
            return 1;
        }
        duct_rx_release(ep, &rx);
    }
    double dt = now_us() - t0;
    printf(
        "%u messages of %u bytes: mean round trip %.1f us, payload throughput %.1f MB/s (both directions)\n",
        count, size, dt / count, 2.0 * size * count / dt);
    free(msg);
    duct_ep_close(ep);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0)) {
        fprintf(
            stderr,
            "usage: %s server|client [--transport tcp|rdma] [--addr A] [--port P] [--count N] [--size S]\n",
            argv[0]);
        return 2;
    }
    int server = strcmp(argv[1], "server") == 0;
    struct duct_opts opts;
    duct_opts_default(&opts);
    const char *addr = NULL;
    const char *port = "7500";
    unsigned count = 10000;
    unsigned size = 64;
    static const struct option lo[] = {
        {"transport", required_argument, NULL, 't'}, {"addr", required_argument, NULL, 'a'},
        {"port", required_argument, NULL, 'p'},      {"count", required_argument, NULL, 'n'},
        {"size", required_argument, NULL, 's'},      {NULL, 0, NULL, 0}};
    optind = 2;
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 't':
            opts.transport = strcmp(optarg, "rdma") == 0 ? DUCT_TRANSPORT_RDMA : DUCT_TRANSPORT_TCP;
            break;
        case 'a':
            addr = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'n':
            count = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 's':
            size = (unsigned)strtoul(optarg, NULL, 10);
            break;
        default:
            return 2;
        }
    }
    duct_ctx *ctx;
    int rc = duct_ctx_create(&opts, &ctx);
    if (rc != 0) {
        fprintf(stderr, "context: %s\n", duct_strerror(rc));
        return 1;
    }
    if (!server && addr == NULL) {
        fprintf(stderr, "client requires --addr\n");
        return 2;
    }
    rc = server ? run_server(ctx, addr, port) : run_client(ctx, addr, port, count, size);
    duct_ctx_destroy(ctx);
    return rc;
}
