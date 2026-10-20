/*
 * hello_server.c - minimal rdmaduct server: accepts connections one at a time and answers every message
 * with the message reversed.
 *
 * Usage: hello_server [tcp|rdma|auto] [port]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdmaduct.h"

int main(int argc, char **argv) {
    struct duct_opts opts;
    duct_opts_default(&opts);
    if (argc > 1 && strcmp(argv[1], "tcp") == 0) {
        opts.transport = DUCT_TRANSPORT_TCP;
    } else if (argc > 1 && strcmp(argv[1], "rdma") == 0) {
        opts.transport = DUCT_TRANSPORT_RDMA;
    }
    const char *port = argc > 2 ? argv[2] : "7800";

    duct_ctx *ctx;
    int rc = duct_ctx_create(&opts, &ctx);
    if (rc != 0) {
        fprintf(stderr, "context: %s\n", duct_strerror(rc));
        return 1;
    }
    duct_listener *listener;
    rc = duct_listen(ctx, NULL, port, &listener);
    if (rc != 0) {
        fprintf(stderr, "listen: %s\n", duct_strerror(rc));
        return 1;
    }
    printf("listening on port %d\n", duct_listener_port(listener));
    fflush(stdout);

    for (;;) {
        duct_ep *ep;
        if (duct_accept(listener, -1, &ep) != 0) {
            continue;
        }
        struct duct_rx rx;
        while (duct_recv(ep, &rx, -1) == 0) {
            char reply[256];
            size_t n = rx.len < sizeof(reply) ? rx.len : sizeof(reply);
            for (size_t i = 0; i < n; i++) {
                reply[i] = ((const char *)rx.data)[n - 1 - i];
            }
            duct_rx_release(ep, &rx);
            if (duct_send(ep, reply, n, 5000) != 0) {
                break;
            }
        }
        duct_ep_close(ep);
    }
}
