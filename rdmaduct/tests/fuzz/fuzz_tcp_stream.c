/*
 * fuzz_tcp_stream.c - libFuzzer harness: the TCP transport's receive path. For each input a raw socket
 * connects to a listener, completes a valid handshake, sends the input as the byte stream and closes;
 * the library endpoint then receives until it reports an error. Only delivered messages within the
 * limit, DUCT_EPROTO and DUCT_ECLOSED are acceptable outcomes (others abort, which the fuzzer reports).
 * Control frames (emulated one-sided operations) are answered by the library; a registered region with
 * remote access exists so that valid requests reach the memory checks.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"

#define MAX_MSG 4096u

struct accepted {
    duct_listener *l;
    duct_ep *ep;
    int rc;
};

static void *accept_main(void *arg) {
    struct accepted *a = arg;
    a->rc = duct_accept(a->l, 2000, &a->ep);
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static duct_ctx *ctx;
    static duct_listener *l;
    static int port;
    if (ctx == NULL) {
        struct duct_opts o;
        duct_opts_default(&o);
        o.transport = DUCT_TRANSPORT_TCP;
        o.max_msg = MAX_MSG;
        o.recv_depth = 4;
        o.send_depth = 4;
        if (duct_ctx_create(&o, &ctx) != 0 || duct_listen(ctx, "127.0.0.1", "0", &l) != 0) {
            abort();
        }
        port = duct_listener_port(l);
        static uint8_t region[4096];
        duct_mr *mr;
        duct_mr_reg(ctx, region, sizeof(region), DUCT_ACCESS_REMOTE_READ | DUCT_ACCESS_REMOTE_WRITE, &mr);
    }
    struct accepted a = {.l = l};
    pthread_t th;
    pthread_create(&th, NULL, accept_main, &a);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        abort();
    }
    uint8_t hello[DUCT_HELLO_SIZE];
    struct duct_hello h = {.max_msg = MAX_MSG, .recv_depth = 4};
    duct_hello_encode(&h, hello);
    if (write(fd, hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        abort();
    }
    size_t got = 0;
    while (got < sizeof(hello)) {
        ssize_t r = read(fd, hello + got, sizeof(hello) - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    pthread_join(th, NULL);
    if (a.rc != 0) {
        abort();
    }
    for (size_t off = 0; off < n;) {
        ssize_t w = write(fd, data + off, n - off);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    shutdown(fd, SHUT_WR);
    for (int guard = 0; guard < 256; guard++) {
        struct duct_rx rx;
        int rc = duct_recv(a.ep, &rx, 1000);
        if (rc == 0) {
            if (rx.len > MAX_MSG) {
                abort();
            }
            duct_rx_release(a.ep, &rx);
            continue;
        }
        if (rc != DUCT_EPROTO && rc != DUCT_ECLOSED) {
            abort();
        }
        break;
    }
    duct_ep_close(a.ep);
    close(fd);
    return 0;
}
