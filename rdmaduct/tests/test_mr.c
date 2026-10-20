/*
 * test_mr.c - registered memory, zero-copy send, one-sided operations and atomics.
 *
 * A target thread registers regions, sends their descriptors and keeps calling duct_recv() (which
 * services emulated operations on TCP) until the initiator closes. The initiator checks:
 *   - write then read back at unaligned offsets and lengths up to 256 KiB;
 *   - 64 pipelined writes completed by waiting for the last sequence number only;
 *   - fetch-and-add and compare-and-swap results, including a failing compare;
 *   - a zero-copy send that arrives as an ordinary message;
 *   - local validation (range, alignment, rights in the descriptor) returning -EINVAL without
 *     damaging the endpoint;
 *   - remote violations on fresh connections: a forged descriptor whose range exceeds the region, and
 *     a region deregistered by its owner, both failing with DUCT_EACCESS.
 *
 * Runs over TCP and, when DUCT_TEST_ADDR is set and RDMA support is compiled in, over RDMA in both wait
 * modes.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

#define REGION (512u * 1024u)
#define MAX_MSG 4096u

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

/* Message tags exchanged between initiator and target. */
enum {
    TAG_DESCS = 'D', /* target -> initiator: descriptors of rw, ro and atomic regions */
    TAG_ECHO = 'E',  /* initiator -> target -> initiator: echoed message */
    TAG_DEREG = 'X', /* initiator -> target: deregister the rw region; reply 'x' */
};

struct target {
    duct_ctx *ctx;
    duct_listener *l;
    int conns; /* connections to serve */
    int error;
};

static void *target_main(void *arg) {
    struct target *t = arg;
    for (int c = 0; c < t->conns; c++) {
        duct_ep *ep;
        int rc = duct_accept(t->l, 10000, &ep);
        if (rc != 0) {
            t->error = rc;
            return NULL;
        }
        uint8_t *rw = aligned_alloc(64, REGION);
        uint8_t *ro = aligned_alloc(64, 4096);
        uint64_t *at = aligned_alloc(64, 64);
        memset(rw, 0, REGION);
        for (int i = 0; i < 4096; i++) {
            ro[i] = (uint8_t)(i * 13);
        }
        memset(at, 0, 64);
        duct_mr *mrw;
        duct_mr *mro;
        duct_mr *mat;
        rc = duct_mr_reg(t->ctx, rw, REGION, DUCT_ACCESS_REMOTE_READ | DUCT_ACCESS_REMOTE_WRITE, &mrw);
        rc |= duct_mr_reg(t->ctx, ro, 4096, DUCT_ACCESS_REMOTE_READ, &mro);
        rc |= duct_mr_reg(t->ctx, at, 64, DUCT_ACCESS_REMOTE_ATOMIC | DUCT_ACCESS_REMOTE_READ, &mat);
        if (rc != 0) {
            t->error = rc;
            duct_ep_close(ep);
            return NULL;
        }
        uint8_t msg[1 + 3 * DUCT_MR_DESC_SIZE];
        msg[0] = TAG_DESCS;
        duct_mr_export(mrw, (struct duct_mr_desc *)(msg + 1));
        duct_mr_export(mro, (struct duct_mr_desc *)(msg + 1 + DUCT_MR_DESC_SIZE));
        duct_mr_export(mat, (struct duct_mr_desc *)(msg + 1 + 2 * DUCT_MR_DESC_SIZE));
        duct_send(ep, msg, sizeof(msg), 5000);
        struct duct_rx rx;
        while ((rc = duct_recv(ep, &rx, 15000)) == 0) {
            const uint8_t *p = rx.data;
            if (rx.len > 0 && p[0] == TAG_ECHO) {
                duct_send(ep, rx.data, rx.len, 5000);
            } else if (rx.len > 0 && p[0] == TAG_DEREG) {
                duct_mr_dereg(mrw);
                mrw = NULL;
                duct_send(ep, "x", 1, 5000);
            }
            duct_rx_release(ep, &rx);
        }
        duct_ep_close(ep);
        duct_mr_dereg(mrw);
        duct_mr_dereg(mro);
        duct_mr_dereg(mat);
        free(rw);
        free(ro);
        free(at);
    }
    return NULL;
}

/* Connects and receives the three descriptors. */
static int open_conn(duct_ctx *ctx, const char *addr, const char *port, duct_ep **ep, struct duct_mr_desc d[3]) {
    int rc = duct_connect(ctx, addr, port, ep);
    if (rc != 0) {
        return rc;
    }
    struct duct_rx rx;
    rc = duct_recv(*ep, &rx, 5000);
    if (rc != 0) {
        return rc;
    }
    if (rx.len != 1 + 3 * DUCT_MR_DESC_SIZE || ((const uint8_t *)rx.data)[0] != TAG_DESCS) {
        duct_rx_release(*ep, &rx);
        return -EPROTO;
    }
    memcpy(d, (const uint8_t *)rx.data + 1, 3 * DUCT_MR_DESC_SIZE);
    duct_rx_release(*ep, &rx);
    return 0;
}

static void fill(uint8_t *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed + i * 31u + (i >> 8));
    }
}

static void run(enum duct_transport tr, enum duct_wait w, const char *addr, const char *name) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = tr;
    o.wait = w;
    o.max_msg = MAX_MSG;
    o.recv_depth = 8;
    o.send_depth = 16;
    struct target t = {.conns = 3};
    int rc = duct_ctx_create(&o, &t.ctx);
    EXPECT(rc == 0, "%s: ctx: %s", name, duct_strerror(rc));
    if (rc != 0) {
        return;
    }
    rc = duct_listen(t.ctx, addr, "0", &t.l);
    EXPECT(rc == 0, "%s: listen: %s", name, duct_strerror(rc));
    if (rc != 0) {
        duct_ctx_destroy(t.ctx);
        return;
    }
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(t.l));
    pthread_t th;
    pthread_create(&th, NULL, target_main, &t);

    uint8_t *lbuf = aligned_alloc(64, REGION);
    memset(lbuf, 0, REGION);
    duct_mr *lmr;
    rc = duct_mr_reg(t.ctx, lbuf, REGION, DUCT_ACCESS_LOCAL, &lmr);
    EXPECT(rc == 0, "%s: local registration: %s", name, duct_strerror(rc));

    duct_ep *ep;
    struct duct_mr_desc d[3];
    rc = open_conn(t.ctx, addr, port, &ep, d);
    EXPECT(rc == 0, "%s: open: %s", name, duct_strerror(rc));
    if (rc == 0 && lmr != NULL) {
        /* write, then read back, at unaligned offsets */
        static const size_t lens[] = {1, 7, 4096, 65537, 262144};
        for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
            size_t len = lens[i];
            size_t roff = 3 + i * 101;
            fill(lbuf, len, i);
            uint64_t s1;
            rc = duct_write(ep, lmr, 0, &d[0], roff, len, 5000, &s1);
            EXPECT(rc == 0, "%s: write %zu: %s", name, len, duct_strerror(rc));
            rc = duct_wait(ep, s1, 5000);
            EXPECT(rc == 0, "%s: wait write %zu: %s", name, len, duct_strerror(rc));
            memset(lbuf + REGION / 2, 0, len);
            uint64_t s2;
            rc = duct_read(ep, lmr, REGION / 2, &d[0], roff, len, 5000, &s2);
            EXPECT(rc == 0, "%s: read %zu: %s", name, len, duct_strerror(rc));
            rc = duct_wait(ep, s2, 5000);
            EXPECT(rc == 0 && s2 == s1 + 1, "%s: wait read %zu: %s", name, len, duct_strerror(rc));
            EXPECT(memcmp(lbuf, lbuf + REGION / 2, len) == 0, "%s: read back %zu differs", name, len);
        }

        /* read-only region: contents and rights */
        rc = duct_read(ep, lmr, 0, &d[1], 0, 4096, 5000, NULL);
        EXPECT(rc == 0, "%s: read ro: %s", name, duct_strerror(rc));
        rc = duct_wait(ep, duct_ep_completed(ep) + 1, 5000);
        int ro_ok = 1;
        for (int i = 0; i < 4096; i++) {
            ro_ok &= lbuf[i] == (uint8_t)(i * 13);
        }
        EXPECT(rc == 0 && ro_ok, "%s: read-only region content", name);
        EXPECT(duct_write(ep, lmr, 0, &d[1], 0, 8, 5000, NULL) == -EINVAL, "%s: write without right", name);

        /* pipelined writes, then one wait */
        uint64_t last = 0;
        for (int i = 0; i < 64; i++) {
            fill(lbuf + (size_t)i * 512, 512, 100 + (unsigned)i);
            rc = duct_write(ep, lmr, (size_t)i * 512, &d[0], (uint64_t)i * 512, 512, 5000, &last);
            EXPECT(rc == 0, "%s: pipelined write %d: %s", name, i, duct_strerror(rc));
        }
        EXPECT(duct_wait(ep, last, 5000) == 0 && duct_ep_completed(ep) >= last, "%s: pipeline completion", name);
        memset(lbuf + REGION / 2, 0, 64 * 512);
        uint64_t s;
        duct_read(ep, lmr, REGION / 2, &d[0], 0, 64 * 512, 5000, &s);
        EXPECT(duct_wait(ep, s, 5000) == 0 && memcmp(lbuf, lbuf + REGION / 2, 64 * 512) == 0,
               "%s: pipelined data", name);

        /* atomics */
        uint64_t *res = (uint64_t *)(lbuf + 1024 * 64);
        for (uint64_t i = 0; i < 10; i++) {
            rc = duct_atomic_fadd(ep, lmr, 1024 * 64, &d[2], 8, 3, 5000, &s);
            EXPECT(rc == 0 && duct_wait(ep, s, 5000) == 0 && *res == 3 * i, "%s: fadd %llu returned %llu", name,
                   (unsigned long long)i, (unsigned long long)*res);
        }
        rc = duct_atomic_cswap(ep, lmr, 1024 * 64, &d[2], 8, 30, 77, 5000, &s);
        EXPECT(rc == 0 && duct_wait(ep, s, 5000) == 0 && *res == 30, "%s: cswap success returned %llu", name,
               (unsigned long long)*res);
        rc = duct_atomic_cswap(ep, lmr, 1024 * 64, &d[2], 8, 30, 99, 5000, &s);
        EXPECT(rc == 0 && duct_wait(ep, s, 5000) == 0 && *res == 77, "%s: cswap failure returned %llu", name,
               (unsigned long long)*res);
        duct_read(ep, lmr, 0, &d[2], 8, 8, 5000, &s);
        uint64_t word;
        memcpy(&word, lbuf, 8);
        EXPECT(duct_wait(ep, s, 5000) == 0 && (memcpy(&word, lbuf, 8), word == 77), "%s: atomic word is %llu",
               name, (unsigned long long)word);

        /* local validation leaves the endpoint usable */
        EXPECT(duct_read(ep, lmr, 0, &d[0], REGION - 4, 8, 0, NULL) == -EINVAL, "%s: remote range", name);
        EXPECT(duct_read(ep, lmr, REGION - 4, &d[0], 0, 8, 0, NULL) == -EINVAL, "%s: local range", name);
        EXPECT(duct_atomic_fadd(ep, lmr, 4, &d[2], 0, 1, 0, NULL) == -EINVAL, "%s: local alignment", name);
        EXPECT(duct_atomic_fadd(ep, lmr, 0, &d[2], 4, 1, 0, NULL) == -EINVAL, "%s: remote alignment", name);
        EXPECT(duct_atomic_fadd(ep, lmr, 0, &d[0], 0, 1, 0, NULL) == -EINVAL, "%s: atomic right", name);
        struct duct_mr_desc bad = d[0];
        bad.bytes[0] ^= 0xff;
        EXPECT(duct_read(ep, lmr, 0, &bad, 0, 8, 0, NULL) == -EINVAL, "%s: bad descriptor", name);
        EXPECT(duct_read(ep, lmr, 0, &d[0], 0, 0, 0, NULL) == -EINVAL, "%s: zero length", name);

        /* zero-copy send */
        lbuf[0] = TAG_ECHO;
        fill(lbuf + 1, 3000, 9);
        rc = duct_send_mr(ep, lmr, 0, 3001, 5000, &s);
        EXPECT(rc == 0 && duct_wait(ep, s, 5000) == 0, "%s: send_mr: %s", name, duct_strerror(rc));
        EXPECT(duct_send_mr(ep, lmr, 0, MAX_MSG + 1, 0, NULL) == -EMSGSIZE, "%s: send_mr size limit", name);
        struct duct_rx rx;
        rc = duct_recv(ep, &rx, 5000);
        EXPECT(rc == 0 && rx.len == 3001 && memcmp(rx.data, lbuf, 3001) == 0, "%s: zero-copy echo", name);
        if (rc == 0) {
            duct_rx_release(ep, &rx);
        }
        duct_ep_close(ep);
    }

    /* forged descriptor: range beyond the region */
    rc = open_conn(t.ctx, addr, port, &ep, d);
    EXPECT(rc == 0, "%s: open 2: %s", name, duct_strerror(rc));
    if (rc == 0) {
        struct duct_desc dd;
        duct_desc_decode(&d[1], &dd);
        dd.len = 1u << 20;
        struct duct_mr_desc forged;
        duct_desc_encode(&dd, &forged);
        uint64_t s;
        rc = duct_read(ep, lmr, 0, &forged, 8192, 64, 5000, &s);
        EXPECT(rc == 0, "%s: post forged read: %s", name, duct_strerror(rc));
        rc = duct_wait(ep, s, 5000);
        EXPECT(rc == DUCT_EACCESS, "%s: forged read failed with %s", name, duct_strerror(rc));
        EXPECT(duct_send(ep, "E", 1, 1000) != 0, "%s: endpoint unusable after an access violation", name);
        duct_ep_close(ep);
    }

    /* deregistered region */
    rc = open_conn(t.ctx, addr, port, &ep, d);
    EXPECT(rc == 0, "%s: open 3: %s", name, duct_strerror(rc));
    if (rc == 0) {
        duct_send(ep, "X", 1, 5000);
        struct duct_rx rx;
        rc = duct_recv(ep, &rx, 5000);
        EXPECT(rc == 0 && rx.len == 1, "%s: deregistration acknowledged", name);
        if (rc == 0) {
            duct_rx_release(ep, &rx);
        }
        uint64_t s;
        rc = duct_write(ep, lmr, 0, &d[0], 0, 16, 5000, &s);
        if (rc == 0) {
            rc = duct_wait(ep, s, 5000);
        }
        EXPECT(rc == DUCT_EACCESS, "%s: write to deregistered region failed with %s", name, duct_strerror(rc));
        duct_ep_close(ep);
    }

    pthread_join(th, NULL);
    EXPECT(t.error == 0, "%s: target error: %s", name, duct_strerror(t.error));
    duct_mr_dereg(lmr);
    free(lbuf);
    duct_listener_close(t.l);
    duct_ctx_destroy(t.ctx);
    printf("%s: one-sided operations ok\n", name);
}

int main(void) {
    run(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1", "tcp");
#ifdef DUCT_HAVE_RDMA
    const char *addr = getenv("DUCT_TEST_ADDR");
    if (addr != NULL && addr[0] != '\0') {
        run(DUCT_TRANSPORT_RDMA, DUCT_WAIT_POLL, addr, "rdma-poll");
        run(DUCT_TRANSPORT_RDMA, DUCT_WAIT_EVENT, addr, "rdma-event");
    } else {
        puts("test_mr: RDMA cases skipped (DUCT_TEST_ADDR not set)");
    }
#endif
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_mr: ok");
    return 0;
}
