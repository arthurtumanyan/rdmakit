/*
 * hello_client.c - minimal rdmaduct client: sends its arguments as messages and prints the replies of
 * hello_server.
 *
 * Usage: hello_client tcp|rdma|auto HOST PORT MESSAGE...
 */
#include <stdio.h>
#include <string.h>

#include "rdmaduct.h"

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s tcp|rdma|auto HOST PORT MESSAGE...\n", argv[0]);
        return 2;
    }
    struct duct_opts opts;
    duct_opts_default(&opts);
    opts.transport = strcmp(argv[1], "tcp") == 0    ? DUCT_TRANSPORT_TCP
                     : strcmp(argv[1], "rdma") == 0 ? DUCT_TRANSPORT_RDMA
                                                    : DUCT_TRANSPORT_AUTO;
    duct_ctx *ctx;
    int rc = duct_ctx_create(&opts, &ctx);
    if (rc != 0) {
        fprintf(stderr, "context: %s\n", duct_strerror(rc));
        return 1;
    }
    duct_ep *ep;
    rc = duct_connect(ctx, argv[2], argv[3], &ep);
    if (rc != 0) {
        fprintf(stderr, "connect: %s\n", duct_strerror(rc));
        return 1;
    }
    for (int i = 4; i < argc; i++) {
        struct duct_rx rx;
        rc = duct_send(ep, argv[i], strlen(argv[i]), 5000);
        if (rc == 0) {
            rc = duct_recv(ep, &rx, 5000);
        }
        if (rc != 0) {
            fprintf(stderr, "exchange failed: %s\n", duct_strerror(rc));
            return 1;
        }
        printf("%s -> %.*s\n", argv[i], (int)rx.len, (const char *)rx.data);
        duct_rx_release(ep, &rx);
    }
    duct_ep_close(ep);
    duct_ctx_destroy(ctx);
    return 0;
}
