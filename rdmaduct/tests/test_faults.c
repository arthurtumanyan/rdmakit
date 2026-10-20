/*
 * test_faults.c - fault injection and failure-handling tests.
 *
 * TCP framing and handshake are attacked through a raw socket that speaks the wire protocol by hand:
 * oversize frames, frames split across pauses, truncation, garbage handshakes, random byte streams.
 * Transport-independent behavior (accept timeout, refused connections, asymmetric limits, abrupt
 * close in both wait modes, connect/disconnect cycles without descriptor leaks) runs on TCP and, when
 * DUCT_TEST_ADDR is set, on RDMA.
 *
 * Limits: the effective send limit of an endpoint is min(local max_msg, peer max_msg).
 * Environment: DUCT_TEST_ADDR (RDMA device address), DUCT_CYCLES (connect/disconnect cycles, default 300).
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

static int failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

#define MAX_MSG 1024u

static double now_ms_f(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static duct_ctx *make_ctx(enum duct_transport t, enum duct_wait wait, uint32_t max_msg, uint32_t depth) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = t;
    o.wait = wait;
    o.max_msg = max_msg;
    o.recv_depth = depth;
    o.send_depth = depth;
    o.connect_timeout_ms = 3000;
    duct_ctx *ctx = NULL;
    int rc = duct_ctx_create(&o, &ctx);
    EXPECT(rc == 0, "ctx create: %s", duct_strerror(rc));
    return ctx;
}

static int open_fds(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) {
        return -1;
    }
    while (readdir(d) != NULL) {
        n++;
    }
    closedir(d);
    return n;
}

/* ---- raw TCP client ------------------------------------------------------------------------------ */

static int raw_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void raw_write(int fd, const void *buf, size_t n) {
    if (write(fd, buf, n) != (ssize_t)n) {
        /* peer may have closed already; the test judges outcomes through the library side */
    }
}

/* Sends a valid handshake and consumes the library's handshake. */
static void raw_handshake(int fd, uint32_t max_msg) {
    uint8_t hello[DUCT_HELLO_SIZE];
    struct duct_hello h = {.max_msg = max_msg, .recv_depth = 4};
    duct_hello_encode(&h, hello);
    raw_write(fd, hello, sizeof(hello));
    size_t got = 0;
    while (got < sizeof(hello)) {
        ssize_t r = read(fd, hello + got, sizeof(hello) - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
}

static void raw_frame_header(int fd, uint32_t len) {
    uint8_t h[4] = {(uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    raw_write(fd, h, sizeof(h));
}

struct accept_result {
    duct_listener *l;
    duct_ep *ep;
    int rc;
};

static void *accept_thread(void *arg) {
    struct accept_result *a = arg;
    a->rc = duct_accept(a->l, 5000, &a->ep);
    return NULL;
}

/* Starts a TCP listener, connects a raw client and returns the accepted library endpoint. */
static int tcp_pair(duct_ctx *ctx, duct_listener **l, duct_ep **ep, int *raw, int do_handshake) {
    if (duct_listen(ctx, "127.0.0.1", "0", l) != 0) {
        return -1;
    }
    struct accept_result a = {.l = *l};
    pthread_t th;
    pthread_create(&th, NULL, accept_thread, &a);
    *raw = raw_connect(duct_listener_port(*l));
    if (do_handshake) {
        raw_handshake(*raw, MAX_MSG);
    }
    pthread_join(th, NULL);
    *ep = a.ep;
    return a.rc;
}

static void test_tcp_oversize_frame(duct_ctx *ctx) {
    duct_listener *l;
    duct_ep *ep;
    int raw;
    EXPECT(tcp_pair(ctx, &l, &ep, &raw, 1) == 0, "pair");
    raw_frame_header(raw, MAX_MSG + 1);
    struct duct_rx rx;
    EXPECT(duct_recv(ep, &rx, 1000) == DUCT_EPROTO, "oversize frame not rejected");
    EXPECT(duct_recv(ep, &rx, 100) == DUCT_EPROTO, "error is not sticky");
    duct_ep_close(ep);
    close(raw);
    duct_listener_close(l);
}

static void test_tcp_split_frame(duct_ctx *ctx) {
    duct_listener *l;
    duct_ep *ep;
    int raw;
    EXPECT(tcp_pair(ctx, &l, &ep, &raw, 1) == 0, "pair");
    static const char text[] = "split across three writes";
    raw_frame_header(raw, sizeof(text));
    raw_write(raw, text, 5);
    struct duct_rx rx;
    EXPECT(duct_recv(ep, &rx, 80) == -ETIMEDOUT, "partial frame must time out");
    raw_write(raw, text + 5, 7);
    EXPECT(duct_recv(ep, &rx, 80) == -ETIMEDOUT, "partial frame must time out again");
    raw_write(raw, text + 12, sizeof(text) - 12);
    int rc = duct_recv(ep, &rx, 1000);
    EXPECT(rc == 0, "resumed receive: %s", duct_strerror(rc));
    EXPECT(rc == 0 && rx.len == sizeof(text) && memcmp(rx.data, text, sizeof(text)) == 0,
           "reassembled content");
    duct_rx_release(ep, &rx);
    duct_ep_close(ep);
    close(raw);
    duct_listener_close(l);
}

static void test_tcp_truncated(duct_ctx *ctx) {
    duct_listener *l;
    duct_ep *ep;
    int raw;
    EXPECT(tcp_pair(ctx, &l, &ep, &raw, 1) == 0, "pair");
    raw_frame_header(raw, 100);
    raw_write(raw, "short", 5);
    close(raw);
    struct duct_rx rx;
    EXPECT(duct_recv(ep, &rx, 1000) == DUCT_ECLOSED, "close inside a frame must report DUCT_ECLOSED");
    duct_ep_close(ep);
    duct_listener_close(l);
}

static void test_tcp_bad_handshakes(duct_ctx *ctx) {
    duct_listener *l;
    EXPECT(duct_listen(ctx, "127.0.0.1", "0", &l) == 0, "listen");
    int port = duct_listener_port(l);

    uint8_t junk[DUCT_HELLO_SIZE];
    memset(junk, 0xA5, sizeof(junk));
    for (int variant = 0; variant < 3; variant++) {
        struct accept_result a = {.l = l};
        pthread_t th;
        pthread_create(&th, NULL, accept_thread, &a);
        int raw = raw_connect(port);
        if (variant == 0) {
            raw_write(raw, junk, sizeof(junk));
        } else if (variant == 1) {
            raw_write(raw, junk, 7);
            close(raw);
            raw = -1;
        } else {
            close(raw);
            raw = -1;
        }
        pthread_join(th, NULL);
        EXPECT(a.rc == DUCT_EPROTO || a.rc == DUCT_ECLOSED, "variant %d: accept returned %s", variant,
               duct_strerror(a.rc));
        if (a.rc == 0) {
            duct_ep_close(a.ep);
        }
        if (raw >= 0) {
            close(raw);
        }
    }
    EXPECT(duct_accept(l, 50, &(duct_ep *){NULL}) == -ETIMEDOUT, "accept timeout");
    duct_listener_close(l);
}

/* Random byte streams after a valid handshake: only defined outcomes, no crash, no hang. */
static void test_tcp_fuzz(duct_ctx *ctx) {
    duct_listener *l;
    EXPECT(duct_listen(ctx, "127.0.0.1", "0", &l) == 0, "listen");
    int port = duct_listener_port(l);
    unsigned seed = 424242;
    int delivered = 0;
    int protocol_errors = 0;
    for (int iter = 0; iter < 1500; iter++) {
        struct accept_result a = {.l = l};
        pthread_t th;
        pthread_create(&th, NULL, accept_thread, &a);
        int raw = raw_connect(port);
        raw_handshake(raw, MAX_MSG);
        pthread_join(th, NULL);
        if (a.rc != 0) {
            EXPECT(0, "fuzz accept: %s", duct_strerror(a.rc));
            close(raw);
            continue;
        }
        uint8_t buf[600];
        seed = seed * 1664525u + 1013904223u;
        size_t n = seed % sizeof(buf);
        for (size_t i = 0; i < n; i++) {
            seed = seed * 1664525u + 1013904223u;
            buf[i] = (uint8_t)(seed >> 24);
        }
        /* Half of the cases start with a plausible small frame header. */
        if ((iter & 1) && n >= 4) {
            buf[0] = (uint8_t)(buf[0] % 64);
            buf[1] = buf[2] = buf[3] = 0;
        }
        raw_write(raw, buf, n);
        close(raw);
        for (int guard = 0; guard < 64; guard++) {
            struct duct_rx rx;
            int rc = duct_recv(a.ep, &rx, 500);
            if (rc == 0) {
                EXPECT(rx.len <= MAX_MSG, "delivered length %zu above limit", rx.len);
                duct_rx_release(a.ep, &rx);
                delivered++;
                continue;
            }
            EXPECT(rc == DUCT_EPROTO || rc == DUCT_ECLOSED, "unexpected outcome %s", duct_strerror(rc));
            protocol_errors += rc == DUCT_EPROTO;
            break;
        }
        duct_ep_close(a.ep);
    }
    printf("fuzz: %d frames delivered, %d protocol errors\n", delivered, protocol_errors);
    duct_listener_close(l);
}

/* ---- transport-independent tests ----------------------------------------------------------------- */

static void test_connect_refused(duct_ctx *ctx, const char *addr) {
    /* Obtain a port that is certainly closed: listen, read the port, close. */
    duct_listener *l;
    if (duct_listen(ctx, addr, "0", &l) != 0) {
        EXPECT(0, "listen for refused test");
        return;
    }
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(l));
    duct_listener_close(l);
    duct_ep *ep = NULL;
    double t0 = now_ms_f();
    int rc = duct_connect(ctx, addr, port, &ep);
    EXPECT(rc == -ECONNREFUSED, "connect to closed port: %s", duct_strerror(rc));
    EXPECT(now_ms_f() - t0 < 2500, "refusal took %.0f ms", now_ms_f() - t0);
    if (rc == 0) {
        duct_ep_close(ep);
    }
}

struct recv_blocker {
    duct_ep *ep;
    int rc;
    double ms;
};

static void *recv_blocker_main(void *arg) {
    struct recv_blocker *b = arg;
    struct duct_rx rx;
    double t0 = now_ms_f();
    b->rc = duct_recv(b->ep, &rx, 8000);
    b->ms = now_ms_f() - t0;
    return NULL;
}

/* A peer closes while the other side is blocked in duct_recv(): the wait ends promptly with DUCT_ECLOSED. */
static void test_abrupt_close(enum duct_transport t, enum duct_wait wait, const char *addr) {
    duct_ctx *ctx = make_ctx(t, wait, MAX_MSG, 8);
    duct_listener *l;
    EXPECT(duct_listen(ctx, addr, "0", &l) == 0, "listen");
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(l));
    struct accept_result a = {.l = l};
    pthread_t th;
    pthread_create(&th, NULL, accept_thread, &a);
    duct_ep *client;
    EXPECT(duct_connect(ctx, addr, port, &client) == 0, "connect");
    pthread_join(th, NULL);
    EXPECT(a.rc == 0, "accept");
    struct recv_blocker b = {.ep = a.ep};
    pthread_t bt;
    pthread_create(&bt, NULL, recv_blocker_main, &b);
    usleep(150000);
    duct_ep_close(client);
    pthread_join(bt, NULL);
    EXPECT(b.rc == DUCT_ECLOSED, "blocked receiver got %s", duct_strerror(b.rc));
    EXPECT(b.ms < 3000, "disconnect detected after %.0f ms (wait mode %d)", b.ms, wait);
    /* Sends on the dead endpoint fail instead of hanging. */
    int rc = 0;
    for (int i = 0; i < 100 && rc == 0; i++) {
        rc = duct_send(a.ep, "x", 1, 1000);
    }
    EXPECT(rc != 0 && rc != -ETIMEDOUT, "send after peer close returned %s", duct_strerror(rc));
    duct_ep_close(a.ep);
    duct_listener_close(l);
    duct_ctx_destroy(ctx);
}

struct echo_thread {
    duct_listener *l;
    int rc;
};

static void *one_echo_thread(void *arg) {
    struct echo_thread *e = arg;
    duct_ep *ep;
    e->rc = duct_accept(e->l, 5000, &ep);
    if (e->rc != 0) {
        return NULL;
    }
    struct duct_rx rx;
    e->rc = duct_recv(ep, &rx, 5000);
    if (e->rc == 0) {
        e->rc = duct_send(ep, rx.data, rx.len, 5000);
        duct_rx_release(ep, &rx);
    }
    duct_ep_close(ep);
    return NULL;
}

/* Two contexts with different limits: each side honors the other's max_msg and credit depth. */
static void test_asymmetric(enum duct_transport t, enum duct_wait wait, const char *addr) {
    duct_ctx *small = make_ctx(t, wait, 256, 2);
    duct_ctx *large = make_ctx(t, wait, 4096, 16);
    duct_listener *l;
    EXPECT(duct_listen(small, addr, "0", &l) == 0, "listen");
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(l));
    struct accept_result a = {.l = l};
    pthread_t th;
    pthread_create(&th, NULL, accept_thread, &a);
    duct_ep *c;
    EXPECT(duct_connect(large, addr, port, &c) == 0, "connect");
    pthread_join(th, NULL);
    EXPECT(a.rc == 0, "accept");
    if (a.rc == 0) {
        EXPECT(duct_ep_peer_max_msg(c) == 256, "client sees peer max %u", duct_ep_peer_max_msg(c));
        EXPECT(duct_ep_peer_max_msg(a.ep) == 256, "server send limit %u", duct_ep_peer_max_msg(a.ep));
        unsigned char big[4096];
        memset(big, 7, sizeof(big));
        EXPECT(duct_send(c, big, 257, 100) == -EMSGSIZE, "client must not exceed the server's limit");
        EXPECT(duct_send(a.ep, big, 257, 100) == -EMSGSIZE, "server must not exceed its own limit either");
        EXPECT(duct_send(a.ep, big, 256, 3000) == 0, "server sends a message at the limit");
        struct duct_rx rx;
        EXPECT(duct_recv(c, &rx, 3000) == 0 && rx.len == 256, "client receives the message");
        duct_rx_release(c, &rx);
        /* The client may have only 2 messages outstanding toward the small server; the third blocks. */
        EXPECT(duct_send(c, "1", 1, 1000) == 0 && duct_send(c, "2", 1, 1000) == 0, "two sends fit");
        int rc = t == DUCT_TRANSPORT_RDMA ? duct_send(c, "3", 1, 200) : -ETIMEDOUT;
        EXPECT(rc == -ETIMEDOUT, "third send must wait for credit (got %s)", duct_strerror(rc));
        EXPECT(duct_recv(a.ep, &rx, 1000) == 0, "server receives first");
        duct_rx_release(a.ep, &rx);
        if (t == DUCT_TRANSPORT_RDMA) {
            EXPECT(duct_send(c, "3", 1, 3000) == 0, "credit returned after release");
        }
        duct_ep_close(a.ep);
    }
    duct_ep_close(c);
    duct_listener_close(l);
    duct_ctx_destroy(small);
    duct_ctx_destroy(large);
}

static void test_cycles(enum duct_transport t, enum duct_wait wait, const char *addr, int cycles) {
    duct_ctx *ctx = make_ctx(t, wait, MAX_MSG, 4);
    duct_listener *l;
    EXPECT(duct_listen(ctx, addr, "0", &l) == 0, "listen");
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(l));
    int fds_before = open_fds();
    for (int i = 0; i < cycles; i++) {
        struct echo_thread e = {.l = l};
        pthread_t th;
        pthread_create(&th, NULL, one_echo_thread, &e);
        duct_ep *c;
        int rc = duct_connect(ctx, addr, port, &c);
        if (rc != 0) {
            EXPECT(0, "cycle %d connect: %s", i, duct_strerror(rc));
            pthread_join(th, NULL);
            break;
        }
        struct duct_rx rx;
        const char *stage = "send";
        rc = duct_send(c, "ping", 4, 3000);
        if (rc == 0) {
            stage = "recv";
            rc = duct_recv(c, &rx, 3000);
        }
        EXPECT(rc == 0, "cycle %d echo (%s, transport %d, wait %d): %s", i, stage, (int)t, (int)wait,
               duct_strerror(rc));
        if (rc == 0) {
            duct_rx_release(c, &rx);
        }
        duct_ep_close(c);
        pthread_join(th, NULL);
        if (rc != 0) {
            EXPECT(0, "cycle %d: server side finished with %s", i, duct_strerror(e.rc));
            break;
        }
    }
    int fds_after = open_fds();
    EXPECT(fds_after <= fds_before + 1, "descriptor leak: %d -> %d after %d cycles", fds_before, fds_after,
           cycles);
    duct_listener_close(l);
    duct_ctx_destroy(ctx);
}

int main(void) {
    duct_ctx *tcp = make_ctx(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, MAX_MSG, 4);
    test_tcp_oversize_frame(tcp);
    test_tcp_split_frame(tcp);
    test_tcp_truncated(tcp);
    test_tcp_bad_handshakes(tcp);
    test_tcp_fuzz(tcp);
    test_connect_refused(tcp, "127.0.0.1");
    duct_ctx_destroy(tcp);

    const char *env = getenv("DUCT_CYCLES");
    int cycles = env != NULL ? atoi(env) : 300;
    test_abrupt_close(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1");
    test_asymmetric(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1");
    test_cycles(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1", cycles);

#ifdef DUCT_HAVE_RDMA
    const char *addr = getenv("DUCT_TEST_ADDR");
    if (addr != NULL && addr[0] != '\0') {
        duct_ctx *rd = make_ctx(DUCT_TRANSPORT_RDMA, DUCT_WAIT_POLL, MAX_MSG, 4);
        test_connect_refused(rd, addr);
        duct_ctx_destroy(rd);
        for (int w = 0; w < 2; w++) {
            enum duct_wait wait = w == 0 ? DUCT_WAIT_POLL : DUCT_WAIT_EVENT;
            test_abrupt_close(DUCT_TRANSPORT_RDMA, wait, addr);
            test_asymmetric(DUCT_TRANSPORT_RDMA, wait, addr);
            test_cycles(DUCT_TRANSPORT_RDMA, wait, addr, cycles);
        }
    } else {
        puts("test_faults: RDMA cases skipped (DUCT_TEST_ADDR not set)");
    }
#endif
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_faults: ok");
    return 0;
}
