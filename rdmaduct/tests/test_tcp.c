/* test_tcp.c - echo, stream and in-place delivery tests over TCP on the loopback interface. */
#include "suite.h"

#define HOLD_COUNT 20000u
#define HOLD_MAX 6u

struct hold_sender {
    duct_listener *listener;
    int rc;
};

/* Message i: 4-byte sequence number followed by a pattern; 8 to 300 bytes. */
static size_t hold_len(uint32_t i) {
    return 8 + (i * 37u) % 293u;
}

static void hold_fill(unsigned char *p, uint32_t i) {
    memcpy(p, &i, 4);
    for (size_t k = 4; k < hold_len(i); k++) {
        p[k] = (unsigned char)(i * 13u + k);
    }
}

static int hold_check(const struct duct_rx *rx, uint32_t i) {
    unsigned char want[512];
    hold_fill(want, i);
    return rx->len == hold_len(i) && memcmp(rx->data, want, rx->len) == 0;
}

/* Sends HOLD_COUNT messages in batches, so that the receiver's input buffer is refilled many times. */
static void *hold_sender_main(void *arg) {
    struct hold_sender *s = arg;
    duct_ep *ep;
    s->rc = duct_accept(s->listener, 10000, &ep);
    if (s->rc != 0) {
        return NULL;
    }
    static unsigned char pool[64][512];
    struct duct_msg batch[64];
    for (uint32_t i = 0; i < HOLD_COUNT && s->rc == 0;) {
        unsigned n = 0;
        while (n < 64 && i + n < HOLD_COUNT) {
            hold_fill(pool[n], i + n);
            batch[n] = (struct duct_msg){.data = pool[n], .len = hold_len(i + n)};
            n++;
        }
        s->rc = duct_send_batch(ep, batch, n, 10000);
        i += n;
    }
    struct duct_rx rx;
    duct_recv(ep, &rx, 10000); /* waits for the receiver to close */
    duct_ep_close(ep);
    return NULL;
}

/*
 * Messages delivered in place from the input buffer stay valid while held: the receiver keeps up to
 * HOLD_MAX messages unreleased (released oldest first, at varying distances) while further frames arrive
 * and the buffer is refilled, and checks every held message again before releasing it.
 */
static int run_hold_inplace(void) {
    struct duct_opts opts;
    duct_opts_default(&opts);
    opts.transport = DUCT_TRANSPORT_TCP;
    opts.max_msg = 512;
    opts.recv_depth = HOLD_MAX + 1;
    opts.send_depth = 8;
    duct_ctx *ctx;
    int rc = duct_ctx_create(&opts, &ctx);
    EXPECT(rc == 0, "hold ctx: %s", duct_strerror(rc));
    if (rc != 0) {
        return 1;
    }
    struct hold_sender s = {0};
    rc = duct_listen(ctx, "127.0.0.1", "0", &s.listener);
    EXPECT(rc == 0, "hold listen: %s", duct_strerror(rc));
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(s.listener));
    pthread_t th;
    pthread_create(&th, NULL, hold_sender_main, &s);
    duct_ep *ep;
    rc = duct_connect(ctx, "127.0.0.1", port, &ep);
    EXPECT(rc == 0, "hold connect: %s", duct_strerror(rc));
    struct duct_rx held[HOLD_MAX];
    uint32_t seq[HOLD_MAX];
    unsigned nheld = 0;
    uint32_t next = 0;
    unsigned bad = 0;
    while (rc == 0 && next < HOLD_COUNT) {
        struct duct_rx rx;
        rc = duct_recv(ep, &rx, 10000);
        EXPECT(rc == 0, "hold recv %u: %s", next, duct_strerror(rc));
        if (rc != 0) {
            break;
        }
        bad += !hold_check(&rx, next);
        held[nheld] = rx;
        seq[nheld] = next;
        nheld++;
        next++;
        /* Keep between 1 and HOLD_MAX messages, varying with the sequence number. */
        unsigned keep = 1 + next % HOLD_MAX;
        while (nheld > keep || nheld == HOLD_MAX) {
            bad += !hold_check(&held[0], seq[0]);
            EXPECT(duct_rx_release(ep, &held[0]) == 0, "hold release %u", seq[0]);
            memmove(&held[0], &held[1], sizeof(held[0]) * (nheld - 1));
            memmove(&seq[0], &seq[1], sizeof(seq[0]) * (nheld - 1));
            nheld--;
        }
    }
    for (unsigned i = 0; i < nheld; i++) {
        bad += !hold_check(&held[i], seq[i]);
        duct_rx_release(ep, &held[i]);
    }
    EXPECT(bad == 0, "%u held messages changed while held", bad);
    EXPECT(next == HOLD_COUNT, "received %u of %u", next, HOLD_COUNT);
    duct_ep_close(ep);
    pthread_join(th, NULL);
    EXPECT(s.rc == 0, "hold sender: %s", duct_strerror(s.rc));
    duct_listener_close(s.listener);
    duct_ctx_destroy(ctx);
    return suite_failures != 0;
}

int main(void) {
    int rc = run_echo_suite(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1");
    rc |= run_stream_stress(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1", 50000);
    rc |= run_hold_inplace();
    if (rc == 0) {
        puts("test_tcp: ok");
    }
    return rc;
}
