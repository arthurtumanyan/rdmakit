/*
 * STEP 9 - One thread serving many clients
 * ========================================
 * Steps 4 to 6 used one thread per client, each blocked on its own completion channel. A server with thousands
 * of clients instead waits for all of them in one epoll set, as network servers do with sockets. Every rdmaduct
 * endpoint has a descriptor for that (duct_ep_fd): on TCP an epoll set around the socket, on RDMA the completion
 * channel. The protocol, from rdmaduct.h:
 *
 *   1. duct_ep_arm(ep) before waiting. 0: nothing to do, wait on the descriptor. -EAGAIN: work is possible now
 *      (a message arrived meanwhile), so serve the endpoint again instead of waiting, or the message would wait
 *      for an event that already happened.
 *   2. epoll_wait() on all descriptors (and duct_listener_fd for new connections).
 *   3. For each ready endpoint, duct_recv(ep, &rx, 0) until it returns -ETIMEDOUT (no more messages), then 1.
 *
 * The alternative is busy polling: one loop calls duct_recv(ep, &rx, 0) on every endpoint without sleeping. It
 * reacts sooner, but keeps a CPU core busy even when no client sends anything. The server prints its CPU time
 * at the end, and the client keeps its connections idle for one second before its requests, so the two modes
 * can be compared: run the server once with and once without --busy.
 *
 * Usage:
 *   step9_duct_event_loop server [--transport tcp|rdma] [--wait poll|event] [--port P] [--busy] [--clients K]
 *   step9_duct_event_loop client HOST [--transport tcp|rdma] [--port P] [--clients K] [--requests N]
 * --clients on the server: exit after K connections have closed (default: run until stopped).
 * --clients on the client: connections, one thread each (default 16); --requests: round trips per connection
 * (default 10000).
 */
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <unistd.h>

#include "duct_common.h"

#define DEFAULT_PORT "7492"
#define MAX_CONNS 1024

struct conn {
    duct_ep *ep;
    int live;
    int ready; /* duct_ep_arm() returned -EAGAIN: serve again before waiting */
};

static struct conn conns[MAX_CONNS];
static unsigned long long served;
static unsigned closed;

static double cpu_seconds(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6 + (double)ru.ru_stime.tv_sec +
           (double)ru.ru_stime.tv_usec * 1e-6;
}

static void drop(int epfd, struct conn *c) {
    if (epfd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, duct_ep_fd(c->ep), NULL);
    }
    duct_ep_close(c->ep);
    c->live = 0;
    c->ready = 0;
    closed++;
}

/* Answers every waiting request. Returns 0, or -1 when the connection ended. */
static int drain(struct conn *c) {
    struct duct_rx rx;
    int rc;
    while ((rc = duct_recv(c->ep, &rx, 0)) == 0) {
        int src = duct_send(c->ep, rx.data, rx.len, 1000);
        duct_rx_release(c->ep, &rx);
        if (src != 0) {
            return -1;
        }
        served++;
    }
    return rc == -ETIMEDOUT ? 0 : -1;
}

/* Accepts every waiting connection. With epfd >= 0 they join the epoll set. */
static void accept_all(duct_listener *l, int epfd) {
    duct_ep *ep;
    while (duct_accept(l, 0, &ep) == 0) {
        unsigned i = 0;
        while (i < MAX_CONNS && conns[i].live) {
            i++;
        }
        if (i == MAX_CONNS) {
            duct_ep_close(ep);
            continue;
        }
        conns[i] = (struct conn){.ep = ep, .live = 1, .ready = 1}; /* a request may already be waiting */
        if (epfd >= 0) {
            struct epoll_event ev = {.events = EPOLLIN, .data.ptr = &conns[i]};
            epoll_ctl(epfd, EPOLL_CTL_ADD, duct_ep_fd(ep), &ev);
        }
    }
}

static void event_loop(duct_listener *l, unsigned stop_after) {
    int epfd = epoll_create1(0);
    struct epoll_event lev = {.events = EPOLLIN, .data.ptr = NULL};
    epoll_ctl(epfd, EPOLL_CTL_ADD, duct_listener_fd(l), &lev);
    while (stop_after == 0 || closed < stop_after) {
        int any_ready = 0;
        for (unsigned i = 0; i < MAX_CONNS; i++) {
            any_ready |= conns[i].live && conns[i].ready;
        }
        struct epoll_event evs[64];
        int n = epoll_wait(epfd, evs, 64, any_ready ? 0 : -1);
        for (int k = 0; k < n; k++) {
            if (evs[k].data.ptr == NULL) {
                accept_all(l, epfd);
            } else {
                ((struct conn *)evs[k].data.ptr)->ready = 1;
            }
        }
        for (unsigned i = 0; i < MAX_CONNS; i++) {
            struct conn *c = &conns[i];
            if (!c->live || !c->ready) {
                continue;
            }
            if (drain(c) != 0) {
                drop(epfd, c);
                continue;
            }
            int rc = duct_ep_arm(c->ep);
            if (rc == 0) {
                c->ready = 0; /* wait for the descriptor */
            } else if (rc != -EAGAIN) {
                drop(epfd, c);
            }
        }
    }
    close(epfd);
}

static void busy_loop(duct_listener *l, unsigned stop_after) {
    while (stop_after == 0 || closed < stop_after) {
        accept_all(l, -1);
        for (unsigned i = 0; i < MAX_CONNS; i++) {
            if (conns[i].live && drain(&conns[i]) != 0) {
                drop(-1, &conns[i]);
            }
        }
    }
}

static int run_server(const struct lab_opts *lo, int busy, unsigned stop_after) {
    struct duct_opts o;
    lab_duct_opts(lo, &o);
    o.max_msg = 4096;
    o.recv_depth = 8;
    o.send_depth = 8;
    duct_ctx *ctx;
    lab_check(duct_ctx_create(&o, &ctx), "duct_ctx_create");
    duct_listener *l;
    lab_check(duct_listen(ctx, NULL, lo->port, &l), "duct_listen");
    printf("listening on port %d (%s, %s)\n", duct_listener_port(l),
           lab_transport_name(duct_ctx_transport(ctx)), busy ? "busy polling" : "epoll");
    fflush(stdout);
    double t0 = lab_now();
    double c0 = cpu_seconds();
    if (busy) {
        busy_loop(l, stop_after);
    } else {
        event_loop(l, stop_after);
    }
    double wall = lab_now() - t0;
    double cpu = cpu_seconds() - c0;
    printf("served %llu requests on %u connections in one thread\n", served, closed);
    printf("server CPU time %.2f s over %.2f s (%.0f%% of one core), %.2f us per request\n", cpu, wall,
           100.0 * cpu / wall, served ? cpu * 1e6 / (double)served : 0.0);
    duct_listener_close(l);
    duct_ctx_destroy(ctx);
    return 0;
}

struct client {
    duct_ctx *ctx;
    const char *host;
    const char *port;
    unsigned requests;
    pthread_barrier_t *start;
    double seconds;
    int failed;
};

static void *client_main(void *arg) {
    struct client *c = arg;
    duct_ep *ep;
    int rc = duct_connect(c->ctx, c->host, c->port, &ep);
    pthread_barrier_wait(c->start); /* every connection is open */
    if (rc != 0) {
        c->failed = 1;
        pthread_barrier_wait(c->start);
        return NULL;
    }
    pthread_barrier_wait(c->start); /* after the idle second */
    char msg[32] = "request";
    double t0 = lab_now();
    for (unsigned i = 0; i < c->requests && !c->failed; i++) {
        struct duct_rx rx;
        c->failed = duct_send(ep, msg, sizeof(msg), 5000) != 0 || duct_recv(ep, &rx, 5000) != 0;
        if (!c->failed) {
            c->failed = rx.len != sizeof(msg);
            duct_rx_release(ep, &rx);
        }
    }
    c->seconds = lab_now() - t0;
    duct_ep_close(ep);
    return NULL;
}

static int run_client(const char *host, const struct lab_opts *lo, unsigned nclients, unsigned requests) {
    struct duct_opts o;
    lab_duct_opts(lo, &o);
    o.max_msg = 4096;
    o.recv_depth = 8;
    o.send_depth = 8;
    duct_ctx *ctx;
    lab_check(duct_ctx_create(&o, &ctx), "duct_ctx_create");
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, nclients + 1);
    struct client *cs = calloc(nclients, sizeof(*cs));
    pthread_t *th = calloc(nclients, sizeof(*th));
    for (unsigned i = 0; i < nclients; i++) {
        cs[i] = (struct client){
            .ctx = ctx, .host = host, .port = lo->port, .requests = requests, .start = &start};
        pthread_create(&th[i], NULL, client_main, &cs[i]);
    }
    pthread_barrier_wait(&start);
    printf("%u connections open; idle for 1 s\n", nclients);
    fflush(stdout);
    sleep(1);
    double t0 = lab_now();
    pthread_barrier_wait(&start);
    int failed = 0;
    double sum = 0;
    for (unsigned i = 0; i < nclients; i++) {
        pthread_join(th[i], NULL);
        failed |= cs[i].failed;
        sum += cs[i].seconds;
    }
    double wall = lab_now() - t0;
    double total = (double)nclients * requests;
    printf("%u connections x %u round trips: %.0f requests/s, mean round trip %.1f us%s\n", nclients,
           requests, total / wall, sum / total * 1e6, failed ? " (some connections failed)" : "");
    free(cs);
    free(th);
    duct_ctx_destroy(ctx);
    return failed;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0) ||
        (strcmp(argv[1], "client") == 0 && argc < 3)) {
        fprintf(stderr, "usage: %s server [options] | client HOST [options]\n", argv[0]);
        return 2;
    }
    struct lab_opts lo = {.transport = DUCT_TRANSPORT_TCP, .wait = DUCT_WAIT_POLL, .port = DEFAULT_PORT};
    int client = strcmp(argv[1], "client") == 0;
    int busy = 0;
    unsigned nclients = client ? 16 : 0;
    unsigned requests = 10000;
    int i = client ? 3 : 2;
    for (int prev = -1; prev != i;) {
        prev = i;
        i = duct_lab_parse(argc, argv, i, &lo);
        if (i < argc && strcmp(argv[i], "--busy") == 0) {
            busy = 1;
            i++;
        } else if (i + 1 < argc && strcmp(argv[i], "--clients") == 0) {
            nclients = (unsigned)strtoul(argv[i + 1], NULL, 10);
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "--requests") == 0) {
            requests = (unsigned)strtoul(argv[i + 1], NULL, 10);
            i += 2;
        }
    }
    if (i != argc || nclients > MAX_CONNS || (client && (nclients == 0 || requests == 0))) {
        fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    return client ? run_client(argv[2], &lo, nclients, requests) : run_server(&lo, busy, nclients);
}
