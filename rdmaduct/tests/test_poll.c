/*
 * test_poll.c - event-loop integration: many endpoints driven by one epoll set per side.
 *
 * A server thread accepts NCONN connections through the listener descriptor and echoes every message
 * from a single epoll loop. The client drives all its endpoints from another single epoll loop with
 * non-blocking sends and receives, so senders run out of send space and flow-control credits and must
 * be woken through the descriptors. Every echo is checked for content and order per connection.
 *
 * Runs over TCP on the loopback interface and, when DUCT_TEST_ADDR is set and RDMA support is compiled
 * in, over RDMA in both wait modes.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "rdmaduct.h"

#define NCONN 64
#define PER_CONN 400
#define MSG_MAX 2048u
#define DEPTH 8u

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

struct server {
    duct_ctx *ctx;
    duct_listener *l;
    int accepted;
    int closed;
    long echoed;
    int error;
};

/* Message i of connection c: length and content derived from (c, i). */
static size_t msg_len(int c, int i) {
    return 1 + (size_t)((c * 131 + i * 17) % (MSG_MAX - 1));
}

static void msg_fill(unsigned char *p, int c, int i) {
    size_t n = msg_len(c, i);
    for (size_t k = 0; k < n; k++) {
        p[k] = (unsigned char)(c * 7 + i * 3 + k);
    }
}

/* Waits on the epoll set for at most ms milliseconds; returns the number of ready entries. */
static int ep_wait(int epfd, struct epoll_event *evs, int max, int ms) {
    int n = epoll_wait(epfd, evs, max, ms);
    return n < 0 ? 0 : n;
}

/* Arms an endpoint; returns 1 if progress is possible now (-EAGAIN), 0 if armed, negative on error. */
static int arm(duct_ep *ep) {
    int rc = duct_ep_arm(ep);
    return rc == -EAGAIN ? 1 : rc;
}

/*
 * Server: one epoll set holds the listener and every endpoint. Echo replies are sent with timeout 0; a
 * reply that does not fit is kept as the pending message of that connection and retried when the
 * descriptor signals progress.
 */
struct sconn {
    duct_ep *ep;
    struct duct_rx pending;
    int has_pending;
    int dead;
};

static void *server_main(void *arg) {
    struct server *s = arg;
    struct sconn *c = calloc(NCONN, sizeof(*c));
    int epfd = epoll_create1(0);
    struct epoll_event lev = {.events = EPOLLIN, .data.u64 = UINT64_MAX};
    epoll_ctl(epfd, EPOLL_CTL_ADD, duct_listener_fd(s->l), &lev);
    struct epoll_event evs[NCONN + 1];
    while (s->closed < NCONN && s->error == 0) {
        int n = ep_wait(epfd, evs, NCONN + 1, 2000);
        if (n == 0) {
            /* No event for 2 s: re-check every endpoint (a missed wake-up shows as a failure below). */
            for (int i = 0; i < s->accepted; i++) {
                evs[n++].data.u64 = (uint64_t)i;
            }
            if (n == 0) {
                continue;
            }
        }
        for (int k = 0; k < n; k++) {
            uint64_t id = evs[k].data.u64;
            if (id == UINT64_MAX) {
                duct_ep *ep;
                while (s->accepted < NCONN && duct_accept(s->l, 0, &ep) == 0) {
                    c[s->accepted].ep = ep;
                    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = (uint64_t)s->accepted};
                    epoll_ctl(epfd, EPOLL_CTL_ADD, duct_ep_fd(ep), &ev);
                    s->accepted++;
                }
                continue;
            }
            struct sconn *sc = &c[id];
            if (sc->dead) {
                continue;
            }
            for (;;) {
                int rc;
                if (sc->has_pending) {
                    rc = duct_send(sc->ep, sc->pending.data, sc->pending.len, 0);
                    if (rc == -ETIMEDOUT) {
                        goto rearm;
                    }
                    duct_rx_release(sc->ep, &sc->pending);
                    sc->has_pending = 0;
                    if (rc != 0) {
                        sc->dead = 1;
                        s->closed++;
                        break;
                    }
                    s->echoed++;
                }
                rc = duct_recv(sc->ep, &sc->pending, 0);
                if (rc == 0) {
                    sc->has_pending = 1;
                    continue;
                }
                if (rc != -ETIMEDOUT) {
                    if (rc != DUCT_ECLOSED) {
                        s->error = rc;
                    }
                    sc->dead = 1;
                    s->closed++;
                    break;
                }
            rearm:
                rc = arm(sc->ep);
                if (rc == 0) {
                    break;
                }
                if (rc < 0) {
                    sc->dead = 1;
                    s->closed++;
                    break;
                }
            }
        }
    }
    for (int i = 0; i < s->accepted; i++) {
        if (c[i].has_pending) {
            duct_rx_release(c[i].ep, &c[i].pending);
        }
        duct_ep_close(c[i].ep);
    }
    close(epfd);
    free(c);
    return NULL;
}

struct cconn {
    duct_ep *ep;
    int sent;
    int received;
};

static void run(enum duct_transport t, enum duct_wait w, const char *addr, const char *name) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = t;
    o.wait = w;
    o.max_msg = MSG_MAX;
    o.recv_depth = DEPTH;
    o.send_depth = DEPTH;
    o.connect_timeout_ms = 10000;
    struct server s = {0};
    int rc = duct_ctx_create(&o, &s.ctx);
    EXPECT(rc == 0, "%s: ctx: %s", name, duct_strerror(rc));
    if (rc != 0) {
        return;
    }
    rc = duct_listen(s.ctx, addr, "0", &s.l);
    EXPECT(rc == 0, "%s: listen: %s", name, duct_strerror(rc));
    if (rc != 0) {
        duct_ctx_destroy(s.ctx);
        return;
    }
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(s.l));
    pthread_t th;
    pthread_create(&th, NULL, server_main, &s);

    struct cconn c[NCONN] = {0};
    int connected = 0;
    for (int i = 0; i < NCONN; i++) {
        rc = duct_connect(s.ctx, addr, port, &c[i].ep);
        EXPECT(rc == 0, "%s: connect %d: %s", name, i, duct_strerror(rc));
        if (rc != 0) {
            break;
        }
        connected++;
    }
    int epfd = epoll_create1(0);
    for (int i = 0; i < connected; i++) {
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
        epoll_ctl(epfd, EPOLL_CTL_ADD, duct_ep_fd(c[i].ep), &ev);
    }
    int done = 0;
    int stalls = 0;
    int fallbacks = 0;
    unsigned char buf[MSG_MAX];
    struct epoll_event evs[NCONN];
    /* Every connection starts runnable. */
    int n = connected;
    for (int i = 0; i < n; i++) {
        evs[i].data.u32 = (uint32_t)i;
    }
    while (done < connected && stalls < 5) {
        for (int k = 0; k < n; k++) {
            struct cconn *cc = &c[evs[k].data.u32];
            if (cc->received == PER_CONN) {
                continue;
            }
            int idx = (int)evs[k].data.u32;
            for (;;) {
                int progressed = 0;
                while (cc->sent < PER_CONN) {
                    msg_fill(buf, idx, cc->sent);
                    rc = duct_send(cc->ep, buf, msg_len(idx, cc->sent), 0);
                    if (rc == -ETIMEDOUT) {
                        break;
                    }
                    EXPECT(rc == 0, "%s: send: %s", name, duct_strerror(rc));
                    if (rc != 0) {
                        cc->received = PER_CONN;
                        done++;
                        break;
                    }
                    cc->sent++;
                    progressed = 1;
                }
                struct duct_rx rx;
                while (cc->received < cc->sent && (rc = duct_recv(cc->ep, &rx, 0)) == 0) {
                    msg_fill(buf, idx, cc->received);
                    size_t want = msg_len(idx, cc->received);
                    EXPECT(rx.len == want && memcmp(rx.data, buf, want) == 0, "%s: conn %d echo %d mismatch",
                           name, idx, cc->received);
                    duct_rx_release(cc->ep, &rx);
                    cc->received++;
                    progressed = 1;
                }
                if (cc->received == PER_CONN) {
                    done++;
                    break;
                }
                if (progressed) {
                    continue;
                }
                rc = arm(cc->ep);
                if (rc == 0) {
                    break;
                }
                EXPECT(rc > 0, "%s: arm: %s", name, duct_strerror(rc));
                if (rc < 0) {
                    cc->received = PER_CONN;
                    done++;
                    break;
                }
            }
        }
        if (done == connected) {
            break;
        }
        n = ep_wait(epfd, evs, NCONN, 2000);
        if (n == 0) {
            stalls++;
            fallbacks++;
            /* Time out: poll everything once more; repeated stalls mean a lost wake-up. */
            for (int i = 0; i < connected; i++) {
                evs[n++].data.u32 = (uint32_t)i;
            }
        } else {
            stalls = 0;
        }
    }
    EXPECT(done == connected && connected == NCONN, "%s: %d of %d connections finished (lost wake-up?)", name,
           done, NCONN);
    EXPECT(fallbacks == 0, "%s: %d rounds needed the 2 s fallback poll: a descriptor missed a wake-up", name,
           fallbacks);
    for (int i = 0; i < connected; i++) {
        EXPECT(c[i].received == PER_CONN, "%s: conn %d received %d of %d", name, i, c[i].received, PER_CONN);
        duct_ep_close(c[i].ep);
    }
    close(epfd);
    pthread_join(th, NULL);
    EXPECT(s.error == 0, "%s: server error: %s", name, duct_strerror(s.error));
    EXPECT(s.echoed == (long)NCONN * PER_CONN, "%s: server echoed %ld", name, s.echoed);
    duct_listener_close(s.l);
    duct_ctx_destroy(s.ctx);
    printf("%s: %d connections x %d messages through one epoll set per side\n", name, NCONN, PER_CONN);
}

int main(void) {
    run(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1", "tcp");
#ifdef DUCT_HAVE_RDMA
    const char *addr = getenv("DUCT_TEST_ADDR");
    if (addr != NULL && addr[0] != '\0') {
        run(DUCT_TRANSPORT_RDMA, DUCT_WAIT_POLL, addr, "rdma-poll");
        run(DUCT_TRANSPORT_RDMA, DUCT_WAIT_EVENT, addr, "rdma-event");
    } else {
        puts("test_poll: RDMA cases skipped (DUCT_TEST_ADDR not set)");
    }
#endif
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_poll: ok");
    return 0;
}
