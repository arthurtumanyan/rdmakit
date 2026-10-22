/*
 * STEP 11 - One-sided operations: shared memory, a counter and a spinlock
 * =======================================================================
 * Step 5 passed address and rkey of a region in the connection's private data and used RDMA READ, WRITE and
 * atomics on it. With rdmaduct the server registers a region with the rights it grants (duct_mr_reg with
 * DUCT_ACCESS_REMOTE_READ, _WRITE, _ATOMIC), exports a 32-byte descriptor (duct_mr_export: address, length, key,
 * rights) and sends it in an ordinary message. The client then calls
 *
 *   duct_read / duct_write       copy bytes between its own registered memory and the server's region
 *   duct_atomic_fadd             add to a 64-bit word and return its previous value, as one indivisible step
 *   duct_atomic_cswap            replace a 64-bit word if it equals a given value; returns the previous value
 *
 * Each call posts the operation and returns its sequence number; duct_wait() waits for it (step 10). Range and
 * rights are checked by the library before posting (-EINVAL) and by the server's adapter (DUCT_EACCESS).
 *
 * On RDMA the server's adapter executes these operations; the server program is not involved. On TCP there is
 * no such adapter: the server's rdmaduct library executes them, and it runs only while the server program
 * calls into it (duct_recv, duct_progress, ...) on that endpoint. The server below therefore waits on its
 * endpoints' descriptors and calls duct_recv(..., 0) whenever one is ready, though it receives only one message
 * per client. With --no-progress it stops calling into the library after sending the descriptor: on TCP every
 * one-sided operation of the client then times out, on RDMA they still complete.
 *
 * Region layout (server): word 0 a counter for fetch-and-add, word 1 a lock (0 free, else the holder's id), word 2
 * a counter that clients update with READ, add, WRITE while holding the lock, bytes 64 to 319 a text field.
 *
 * Each client thread adds 1 to word 0 N times with fetch-and-add, and N times takes the lock with
 * compare-and-swap, reads word 2, writes it back plus one, waits for the write to complete, and releases the lock.
 * Both counters must end at clients * N. With --no-lock the read-add-write runs without the lock and updates are
 * lost when two clients interleave.
 *
 * Usage:
 *   step11_duct_one_sided server [--transport tcp|rdma] [--port P] [--no-progress] [--clients K]
 *   step11_duct_one_sided client HOST [--transport tcp|rdma] [--port P] [--clients K] [--ops N] [--no-lock]
 * Server --clients: exit after K clients said goodbye. Client defaults: 2 clients, 2000 operations each.
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>

#include "duct_common.h"

#define DEFAULT_PORT "7494"
#define REGION 4096u
#define W_COUNTER 0u
#define W_LOCK 8u
#define W_GUARDED 16u
#define TEXT 64u
#define TEXT_LEN 256u
#define MAX_CLIENTS 64
#define TIMEOUT_MS 3000

static duct_ctx *make_ctx(const struct lab_opts *lo) {
    struct duct_opts o;
    lab_duct_opts(lo, &o);
    o.max_msg = 4096;
    o.recv_depth = 8;
    o.send_depth = 8;
    duct_ctx *ctx;
    lab_check(duct_ctx_create(&o, &ctx), "duct_ctx_create");
    return ctx;
}

static int run_server(const struct lab_opts *lo, int progress, unsigned stop_after) {
    duct_ctx *ctx = make_ctx(lo);
    static uint64_t region[REGION / 8]; /* 8-byte aligned, as atomics require */
    duct_mr *mr;
    lab_check(duct_mr_reg(ctx, region, sizeof(region),
                          DUCT_ACCESS_REMOTE_READ | DUCT_ACCESS_REMOTE_WRITE | DUCT_ACCESS_REMOTE_ATOMIC,
                          &mr),
              "duct_mr_reg");
    struct duct_mr_desc desc;
    duct_mr_export(mr, &desc);
    duct_listener *l;
    lab_check(duct_listen(ctx, NULL, lo->port, &l), "duct_listen");
    printf("listening on port %d (%s)%s\n", duct_listener_port(l),
           lab_transport_name(duct_ctx_transport(ctx)),
           progress ? "" : ", not calling into the library after the descriptor");
    fflush(stdout);
    duct_ep *eps[MAX_CLIENTS];
    unsigned n = 0;
    unsigned done = 0;
    while (stop_after == 0 || done < stop_after) {
        duct_ep *ep;
        if (n < MAX_CLIENTS && duct_accept(l, 0, &ep) == 0) {
            lab_check(duct_send(ep, &desc, sizeof(desc), 5000), "duct_send"); /* the client's only message */
            eps[n++] = ep;
        }
        if (!progress) {
            usleep(10000); /* the endpoints are left alone: nothing services emulated operations on TCP */
            continue;
        }
        struct pollfd fds[MAX_CLIENTS + 1];
        int wait_ms = 100;
        for (unsigned i = 0; i < n; i++) {
            struct duct_rx rx;
            int rc = duct_recv(eps[i], &rx, 0); /* also executes the client's emulated operations (TCP) */
            if (rc == 0) {
                duct_rx_release(eps[i], &rx); /* goodbye */
                done++;
                rc = DUCT_ECLOSED;
            }
            if (rc == -ETIMEDOUT) {
                rc = duct_ep_arm(eps[i]);
            }
            if (rc == -EAGAIN) {
                wait_ms = 0;
            } else if (rc != 0) {
                duct_ep_close(eps[i]);
                eps[i--] = eps[--n];
                continue;
            }
            fds[i] = (struct pollfd){.fd = duct_ep_fd(eps[i]), .events = POLLIN};
        }
        fds[n] = (struct pollfd){.fd = duct_listener_fd(l), .events = POLLIN};
        poll(fds, n + 1, wait_ms);
    }
    printf("counter %llu, lock-protected counter %llu, text \"%s\"\n",
           (unsigned long long)region[W_COUNTER / 8], (unsigned long long)region[W_GUARDED / 8],
           (const char *)region + TEXT);
    for (unsigned i = 0; i < n; i++) {
        duct_ep_close(eps[i]);
    }
    duct_listener_close(l);
    duct_mr_dereg(mr);
    duct_ctx_destroy(ctx);
    return 0;
}

struct client {
    duct_ctx *ctx;
    const char *host;
    const char *port;
    unsigned id;
    unsigned ops;
    int lock;
    pthread_barrier_t *start;
    int rc; /* first error */
};

/* Local memory of one client: results of reads and atomics, and the source of writes. */
struct scratch {
    uint64_t word[4];
    char text[TEXT_LEN];
};

static int lock_take(duct_ep *ep, duct_mr *mr, const struct duct_mr_desc *d, uint64_t id) {
    struct scratch *sc = duct_mr_addr(mr);
    for (;;) {
        uint64_t seq;
        int rc = duct_atomic_cswap(ep, mr, 0, d, W_LOCK, 0, id, TIMEOUT_MS, &seq);
        if (rc == 0) {
            rc = duct_wait(ep, seq, TIMEOUT_MS);
        }
        if (rc != 0 || sc->word[0] == 0) {
            return rc; /* the previous value was 0: the lock is ours */
        }
    }
}

static int lock_give(duct_ep *ep, duct_mr *mr, const struct duct_mr_desc *d, uint64_t id) {
    uint64_t seq;
    int rc = duct_atomic_cswap(ep, mr, 0, d, W_LOCK, id, 0, TIMEOUT_MS, &seq);
    return rc != 0 ? rc : duct_wait(ep, seq, TIMEOUT_MS);
}

/* Reads word 2, adds one and writes it back; the write has completed when this returns. */
static int bump(duct_ep *ep, duct_mr *mr, const struct duct_mr_desc *d) {
    struct scratch *sc = duct_mr_addr(mr);
    uint64_t seq;
    int rc = duct_read(ep, mr, 8, d, W_GUARDED, 8, TIMEOUT_MS, &seq);
    if (rc == 0 && (rc = duct_wait(ep, seq, TIMEOUT_MS)) == 0) {
        sc->word[1]++;
        rc = duct_write(ep, mr, 8, d, W_GUARDED, 8, TIMEOUT_MS, &seq);
    }
    /* Waiting for the write before the lock is released: the next holder must read the new value. */
    return rc != 0 ? rc : duct_wait(ep, seq, TIMEOUT_MS);
}

static void *client_main(void *arg) {
    struct client *c = arg;
    duct_ep *ep = NULL;
    duct_mr *mr = NULL;
    static __thread struct scratch sc;
    struct duct_rx rx;
    struct duct_mr_desc d;
    int rc = duct_connect(c->ctx, c->host, c->port, &ep);
    if (rc == 0 && (rc = duct_recv(ep, &rx, 5000)) == 0) {
        memcpy(&d, rx.data, rx.len == sizeof(d) ? sizeof(d) : 0);
        rc = rx.len == sizeof(d) ? 0 : -EPROTO;
        duct_rx_release(ep, &rx);
    }
    if (rc == 0) {
        rc = duct_mr_reg(c->ctx, &sc, sizeof(sc), DUCT_ACCESS_LOCAL, &mr);
    }
    pthread_barrier_wait(c->start);
    if (rc == 0 && c->id == 1) {
        uint64_t seq;
        snprintf(sc.text, sizeof(sc.text), "written by client 1 with a one-sided write");
        rc = duct_write(ep, mr, offsetof(struct scratch, text), &d, TEXT, TEXT_LEN, TIMEOUT_MS, &seq);
        if (rc == 0 && (rc = duct_wait(ep, seq, TIMEOUT_MS)) == 0) {
            memset(sc.text, 0, sizeof(sc.text));
            rc = duct_read(ep, mr, offsetof(struct scratch, text), &d, TEXT, TEXT_LEN, TIMEOUT_MS, &seq);
            rc = rc != 0 ? rc : duct_wait(ep, seq, TIMEOUT_MS);
        }
        if (rc == 0) {
            printf("client 1 read back: \"%s\"\n", sc.text);
        }
    }
    for (unsigned i = 0; i < c->ops && rc == 0; i++) {
        uint64_t seq;
        rc = duct_atomic_fadd(ep, mr, 0, &d, W_COUNTER, 1, TIMEOUT_MS, &seq);
        rc = rc != 0 ? rc : duct_wait(ep, seq, TIMEOUT_MS);
    }
    for (unsigned i = 0; i < c->ops && rc == 0; i++) {
        if (c->lock && (rc = lock_take(ep, mr, &d, c->id)) != 0) {
            break;
        }
        rc = bump(ep, mr, &d);
        if (c->lock && rc == 0) {
            rc = lock_give(ep, mr, &d, c->id);
        }
    }
    c->rc = rc;
    pthread_barrier_wait(c->start); /* every client has finished its updates */
    if (rc == 0 && c->id == 1) {
        uint64_t seq;
        rc = duct_read(ep, mr, 0, &d, W_COUNTER, 24, TIMEOUT_MS, &seq);
        rc = rc != 0 ? rc : duct_wait(ep, seq, TIMEOUT_MS);
        if (rc == 0) {
            printf("fetch-and-add counter %llu, lock-protected counter %llu\n",
                   (unsigned long long)sc.word[0], (unsigned long long)sc.word[2]);
            c->rc = sc.word[0] == sc.word[2] ? 0 : 1; /* main compares with the expected total */
        }
    }
    if (ep != NULL && c->rc >= 0) {
        duct_send(ep, "bye", 3, 1000);
    }
    duct_mr_dereg(mr);
    duct_ep_close(ep);
    return NULL;
}

static int run_client(const char *host, const struct lab_opts *lo, unsigned nclients, unsigned ops,
                      int lock) {
    duct_ctx *ctx = make_ctx(lo);
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, nclients);
    struct client cs[MAX_CLIENTS];
    pthread_t th[MAX_CLIENTS];
    for (unsigned i = 0; i < nclients; i++) {
        cs[i] = (struct client){.ctx = ctx,
                                .host = host,
                                .port = lo->port,
                                .id = i + 1,
                                .ops = ops,
                                .lock = lock,
                                .start = &start};
        pthread_create(&th[i], NULL, client_main, &cs[i]);
    }
    int bad = 0;
    for (unsigned i = 0; i < nclients; i++) {
        pthread_join(th[i], NULL);
        if (cs[i].rc < 0) {
            printf("client %u: %s\n", i + 1, duct_strerror(cs[i].rc));
            bad = 1;
        }
    }
    if (!bad) {
        printf("expected %u for both counters%s\n", nclients * ops,
               cs[0].rc == 0 ? "" : ": the lock-protected counter lost updates");
        bad = cs[0].rc != 0;
    }
    duct_ctx_destroy(ctx);
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0) ||
        (strcmp(argv[1], "client") == 0 && argc < 3)) {
        fprintf(stderr, "usage: %s server [options] | client HOST [options]\n", argv[0]);
        return 2;
    }
    struct lab_opts lo = {.transport = DUCT_TRANSPORT_TCP, .wait = DUCT_WAIT_POLL, .port = DEFAULT_PORT};
    int client = strcmp(argv[1], "client") == 0;
    int progress = 1;
    int lock = 1;
    unsigned nclients = client ? 2 : 0;
    unsigned ops = 2000;
    int i = client ? 3 : 2;
    for (int prev = -1; prev != i;) {
        prev = i;
        i = duct_lab_parse(argc, argv, i, &lo);
        if (i < argc && strcmp(argv[i], "--no-progress") == 0) {
            progress = 0;
            i++;
        } else if (i < argc && strcmp(argv[i], "--no-lock") == 0) {
            lock = 0;
            i++;
        } else if (i + 1 < argc && strcmp(argv[i], "--clients") == 0) {
            nclients = (unsigned)strtoul(argv[i + 1], NULL, 10);
            i += 2;
        } else if (i + 1 < argc && strcmp(argv[i], "--ops") == 0) {
            ops = (unsigned)strtoul(argv[i + 1], NULL, 10);
            i += 2;
        }
    }
    if (i != argc || nclients > MAX_CLIENTS || (client && nclients == 0)) {
        fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    return client ? run_client(argv[2], &lo, nclients, ops, lock) : run_server(&lo, progress, nclients);
}
