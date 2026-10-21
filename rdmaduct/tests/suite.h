/*
 * suite.h - transport-independent echo test suite.
 *
 * run_echo_suite() runs the same checks over any transport: connection setup
 * and handshake, echo of message sizes from 0 to max_msg, bursts up to the
 * receive depth, oversize rejection, receive timeout, orderly close detection,
 * and a second connection on the same listener.
 */
#ifndef DUCT_TEST_SUITE_H
#define DUCT_TEST_SUITE_H

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rdmaduct.h"

#define SUITE_MAX_MSG 4096u
#define SUITE_DEPTH 8u

static int suite_failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            suite_failures++;                                                                                \
        }                                                                                                    \
    } while (0)

struct suite_server {
    duct_listener *listener;
    unsigned long handled[2];
    int end_code[2];
};

/* Accepts two connections in sequence and echoes until each peer closes. */
static void *suite_server_main(void *arg) {
    struct suite_server *s = arg;
    for (int conn = 0; conn < 2; conn++) {
        duct_ep *ep;
        int rc = duct_accept(s->listener, 10000, &ep);
        if (rc != 0) {
            s->end_code[conn] = rc;
            return NULL;
        }
        struct duct_rx rx;
        while ((rc = duct_recv(ep, &rx, 10000)) == 0) {
            int src = duct_send(ep, rx.data, rx.len, 10000);
            duct_rx_release(ep, &rx);
            if (src != 0) {
                rc = src;
                break;
            }
            s->handled[conn]++;
        }
        s->end_code[conn] = rc;
        duct_ep_close(ep);
    }
    return NULL;
}

static void fill_pattern(unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)(seed * 31u + i * 7u);
    }
}

static void echo_once(duct_ep *ep, size_t len, unsigned seed) {
    unsigned char out[SUITE_MAX_MSG];
    fill_pattern(out, len, seed);
    int rc = duct_send(ep, out, len, 5000);
    EXPECT(rc == 0, "send len=%zu: %s", len, duct_strerror(rc));
    struct duct_rx rx;
    rc = duct_recv(ep, &rx, 5000);
    EXPECT(rc == 0, "recv len=%zu: %s", len, duct_strerror(rc));
    if (rc == 0) {
        EXPECT(rx.len == len, "echo length %zu, expected %zu", rx.len, len);
        EXPECT(rx.len == len && memcmp(rx.data, out, len) == 0, "echo content mismatch for len=%zu", len);
        duct_rx_release(ep, &rx);
    }
}

[[maybe_unused]] static int run_echo_suite(enum duct_transport transport, enum duct_wait wait,
                                           const char *addr) {
    struct duct_opts opts;
    duct_opts_default(&opts);
    opts.transport = transport;
    opts.wait = wait;
    opts.max_msg = SUITE_MAX_MSG;
    opts.recv_depth = SUITE_DEPTH;
    opts.send_depth = SUITE_DEPTH;
    opts.connect_timeout_ms = 5000;
    duct_ctx *ctx;
    int rc = duct_ctx_create(&opts, &ctx);
    EXPECT(rc == 0, "ctx create: %s", duct_strerror(rc));
    if (rc != 0) {
        return 1;
    }
    EXPECT(duct_ctx_transport(ctx) == transport, "transport resolved");

    struct suite_server srv = {0};
    rc = duct_listen(ctx, addr, "0", &srv.listener);
    EXPECT(rc == 0, "listen: %s", duct_strerror(rc));
    if (rc != 0) {
        duct_ctx_destroy(ctx);
        return 1;
    }
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(srv.listener));
    pthread_t th;
    pthread_create(&th, NULL, suite_server_main, &srv);

    duct_ep *ep;
    rc = duct_connect(ctx, addr, port, &ep);
    EXPECT(rc == 0, "connect: %s", duct_strerror(rc));
    if (rc != 0) {
        pthread_cancel(th);
        pthread_join(th, NULL);
        duct_listener_close(srv.listener);
        duct_ctx_destroy(ctx);
        return 1;
    }
    EXPECT(duct_ep_peer_max_msg(ep) == SUITE_MAX_MSG, "peer max_msg = %u", duct_ep_peer_max_msg(ep));

    static const size_t sizes[] = {0, 1, 2, 63, 64, 65, 1000, 4095, SUITE_MAX_MSG};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        echo_once(ep, sizes[i], (unsigned)i);
    }
    unsigned seed = 12345;
    for (unsigned i = 0; i < 2000; i++) {
        seed = seed * 1103515245u + 12345u;
        echo_once(ep, (seed >> 8) % (SUITE_MAX_MSG + 1), i);
    }

    /* Burst: SUITE_DEPTH messages in flight before any is read. */
    for (unsigned i = 0; i < SUITE_DEPTH; i++) {
        unsigned char m[64];
        memset(m, (int)i, sizeof(m));
        rc = duct_send(ep, m, sizeof(m), 5000);
        EXPECT(rc == 0, "burst send %u: %s", i, duct_strerror(rc));
    }
    for (unsigned i = 0; i < SUITE_DEPTH; i++) {
        struct duct_rx rx;
        rc = duct_recv(ep, &rx, 5000);
        EXPECT(rc == 0, "burst recv %u: %s", i, duct_strerror(rc));
        if (rc == 0) {
            EXPECT(rx.len == 64 && ((const unsigned char *)rx.data)[0] == i, "burst order at %u", i);
            duct_rx_release(ep, &rx);
        }
    }

    unsigned char big[SUITE_MAX_MSG + 1] = {0};
    EXPECT(duct_send(ep, big, sizeof(big), 1000) == -EMSGSIZE, "oversize send rejected");

    struct duct_rx rx;
    EXPECT(duct_recv(ep, &rx, 50) == -ETIMEDOUT, "receive timeout");
    EXPECT(duct_recv(ep, &rx, 0) == -ETIMEDOUT, "non-blocking receive");

    /* Several messages may be held unreleased at once; each keeps its own buffer. */
    {
        struct duct_rx held[3];
        for (unsigned i = 0; i < 3; i++) {
            unsigned char b = (unsigned char)(0xa0 + i);
            EXPECT(duct_send(ep, &b, 1, 1000) == 0, "send for hold test %u", i);
        }
        int got = 0;
        for (unsigned i = 0; i < 3; i++) {
            rc = duct_recv(ep, &held[i], 5000);
            EXPECT(rc == 0, "hold recv %u: %s", i, duct_strerror(rc));
            got += rc == 0;
        }
        if (got == 3) {
            for (unsigned i = 0; i < 3; i++) {
                EXPECT(held[i].len == 1 && ((const unsigned char *)held[i].data)[0] == 0xa0 + i,
                       "held message %u intact", i);
            }
            for (unsigned i = 0; i < 3; i++) {
                EXPECT(duct_rx_release(ep, &held[i]) == 0, "release held %u", i);
            }
        }
        EXPECT(duct_rx_release(ep, &held[0]) == -EINVAL, "double release rejected");
    }

    duct_ep_close(ep);

    rc = duct_connect(ctx, addr, port, &ep);
    EXPECT(rc == 0, "second connect: %s", duct_strerror(rc));
    if (rc == 0) {
        echo_once(ep, 100, 7);
        duct_ep_close(ep);
    }
    pthread_join(th, NULL);
    EXPECT(srv.end_code[0] == DUCT_ECLOSED, "first connection ended with: %s",
           duct_strerror(srv.end_code[0]));
    EXPECT(srv.end_code[1] == DUCT_ECLOSED, "second connection ended with: %s",
           duct_strerror(srv.end_code[1]));
    EXPECT(srv.handled[0] > 2000, "server handled %lu messages", srv.handled[0]);

    duct_listener_close(srv.listener);
    duct_ctx_destroy(ctx);
    return suite_failures != 0;
}

/*
 * Streaming stress test. One producer sends `count` sequence-numbered messages of varying size in
 * batches of random length; the consumer verifies order and content, sleeps at intervals, and keeps up
 * to STRESS_HOLD received messages unreleased, so the producer repeatedly exhausts its send credits.
 * Batches of up to 80 messages span several RDMA doorbell chunks (16) and several TCP sendmsg groups (32).
 */
#define STRESS_MAX_BATCH 80
#define STRESS_MAX_LEN 512
#define STRESS_HOLD 3

struct stress_server {
    duct_listener *listener;
    uint64_t count;
    uint64_t received;
    unsigned hold; /* unreleased messages kept by the consumer; TCP endpoints allow none */
    int failed;
};

static unsigned stress_len(uint64_t seq) {
    return 12 + (unsigned)((seq * 2654435761u) >> 7) % (STRESS_MAX_LEN - 12 + 1);
}

static void stress_fill(unsigned char *buf, uint64_t seq) {
    unsigned len = stress_len(seq);
    memcpy(buf, &seq, 8);
    memcpy(buf + 8, &len, 4);
    for (unsigned i = 12; i < len; i++) {
        buf[i] = (unsigned char)(seq + i);
    }
}

static void *stress_consumer(void *arg) {
    struct stress_server *s = arg;
    duct_ep *ep;
    if (duct_accept(s->listener, 10000, &ep) != 0) {
        s->failed = 1;
        return NULL;
    }
    struct duct_rx held[STRESS_HOLD];
    unsigned nheld = 0;
    while (s->received < s->count && !s->failed) {
        struct duct_rx rx;
        int rc = duct_recv(ep, &rx, 10000);
        if (rc != 0) {
            fprintf(stderr, "consumer recv #%llu: %s\n", (unsigned long long)s->received, duct_strerror(rc));
            s->failed = 1;
            break;
        }
        uint64_t seq;
        unsigned len;
        memcpy(&seq, rx.data, 8);
        memcpy(&len, (const unsigned char *)rx.data + 8, 4);
        unsigned char expect[STRESS_MAX_LEN];
        stress_fill(expect, s->received);
        if (seq != s->received || rx.len != len || len != stress_len(seq) ||
            memcmp(rx.data, expect, len) != 0) {
            fprintf(stderr, "consumer: bad message at #%llu (seq %llu len %zu)\n",
                    (unsigned long long)s->received, (unsigned long long)seq, rx.len);
            s->failed = 1;
            break;
        }
        s->received++;
        if (s->hold == 0) {
            duct_rx_release(ep, &rx);
        } else if (nheld == s->hold) {
            duct_rx_release(ep, &held[0]);
            memmove(&held[0], &held[1], sizeof(held[0]) * (STRESS_HOLD - 1));
            nheld--;
        }
        if (s->hold != 0) {
            held[nheld++] = rx;
        }
        if (s->received % 97 == 0) {
            usleep(300);
        }
        if (s->received % 5000 == 0) {
            usleep(20000);
        }
    }
    for (unsigned i = 0; i < nheld; i++) {
        duct_rx_release(ep, &held[i]);
    }
    duct_ep_close(ep);
    return NULL;
}

[[maybe_unused]] static int run_stream_stress(enum duct_transport transport, enum duct_wait wait,
                                              const char *addr, uint64_t count) {
    struct duct_opts opts;
    duct_opts_default(&opts);
    opts.transport = transport;
    opts.wait = wait;
    opts.max_msg = STRESS_MAX_LEN;
    opts.recv_depth = 8;
    opts.send_depth = 8;
    opts.connect_timeout_ms = 5000;
    duct_ctx *ctx;
    int rc = duct_ctx_create(&opts, &ctx);
    EXPECT(rc == 0, "stress ctx: %s", duct_strerror(rc));
    if (rc != 0) {
        return 1;
    }
    struct stress_server srv = {.count = count, .hold = transport == DUCT_TRANSPORT_TCP ? 0 : STRESS_HOLD};
    rc = duct_listen(ctx, addr, "0", &srv.listener);
    EXPECT(rc == 0, "stress listen: %s", duct_strerror(rc));
    if (rc != 0) {
        duct_ctx_destroy(ctx);
        return 1;
    }
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(srv.listener));
    pthread_t th;
    pthread_create(&th, NULL, stress_consumer, &srv);

    duct_ep *ep;
    rc = duct_connect(ctx, addr, port, &ep);
    EXPECT(rc == 0, "stress connect: %s", duct_strerror(rc));
    if (rc == 0) {
        static unsigned char pool[STRESS_MAX_BATCH][STRESS_MAX_LEN];
        struct duct_msg batch[STRESS_MAX_BATCH];
        uint64_t seq = 0;
        unsigned rnd = 17;
        while (seq < count && !srv.failed) {
            rnd = rnd * 1103515245u + 12345u;
            unsigned n = 1 + (rnd >> 16) % STRESS_MAX_BATCH;
            if (n > count - seq) {
                n = (unsigned)(count - seq);
            }
            for (unsigned i = 0; i < n; i++) {
                stress_fill(pool[i], seq + i);
                batch[i] = (struct duct_msg){.data = pool[i], .len = stress_len(seq + i)};
            }
            rc = duct_send_batch(ep, batch, n, 10000);
            if (rc != 0) {
                EXPECT(0, "stress send at %llu: %s", (unsigned long long)seq, duct_strerror(rc));
                break;
            }
            seq += n;
        }
        pthread_join(th, NULL);
        EXPECT(!srv.failed, "consumer failed");
        EXPECT(srv.received == count, "consumer received %llu of %llu", (unsigned long long)srv.received,
               (unsigned long long)count);
        duct_ep_close(ep);
    } else {
        pthread_cancel(th);
        pthread_join(th, NULL);
    }
    duct_listener_close(srv.listener);
    duct_ctx_destroy(ctx);
    return suite_failures != 0;
}

#endif
