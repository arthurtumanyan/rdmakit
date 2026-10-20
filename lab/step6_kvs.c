/*
 * STEP 6 - A real application: RDMA key-value store ("pilaf-lite")
 * ===============================================================
 * Everything from steps 1-5 combined, using the design of research systems Pilaf / HERD / FaRM:
 *
 *   GET  = one-sided RDMA READ.  The client computes hash(key), reads the right slots straight out of the
 *          server's memory. The server CPU never sees the request - lookups scale with the NIC, not the CPU.
 *   PUT / DEL = two-sided SEND/RECV.  Writes must be serialized and validated, so the server CPU applies them
 *          and answers with a SEND.
 *
 * The hard part of one-sided reads: the server may be modifying a slot while the NIC reads it ("torn read").
 * Every slot carries a CRC over its contents; the client re-checks it and retries on mismatch (the approach of the
 * Pilaf key-value store). The server writes the CRC *last*, so a half-written slot never validates.
 *
 * Memory layout (one registered region, handed to each client in the connection's private data):
 *
 *     slot 0 | slot 1 | ... | slot NSLOTS-1 | PROBE-1 extra slots
 *     each slot = 64 bytes: crc(4) flags(4) key[24] val[32]
 *
 * A key lives in one of the PROBE consecutive slots starting at hash(key) % NSLOTS (linear probing with a
 * fixed window). A GET therefore needs exactly ONE RDMA READ of PROBE*64 bytes. The extra tail slots mean the
 * window never wraps around.
 *
 * Server: one thread per client; each thread SLEEPS on a completion channel until a request arrives
 * (no busy polling), so an idle server uses no CPU.
 *
 * Usage:
 *   step6_kvs server [port]
 *   step6_kvs client <server-ip> [port]            interactive:  put k v | get k | del k | bench [N] | quit
 *   step6_kvs client <server-ip> <port> <cmd...>   one command,   e.g.  ... 7473 put city Yerevan
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include "common.h"

#define DEFAULT_PORT "7473"
#define NSLOTS 4096
#define PROBE 4
#define KEYLEN 24
#define VALLEN 32

struct slot {
    uint32_t crc;
    uint32_t flags;
    char key[KEYLEN];
    char val[VALLEN];
}; // 64 bytes
#define SLOT_USED 1
_Static_assert(sizeof(struct slot) == 64, "slot must be 64 bytes");

// Request/response message for the two-sided part (always 64 bytes).
enum { OP_PUT = 1, OP_DEL = 2 };
enum { ST_OK = 0, ST_FULL = 1, ST_NOTFOUND = 2 };
struct msg {
    uint8_t op;
    uint8_t status;
    uint8_t pad[6];
    char key[KEYLEN];
    char val[VALLEN];
};
_Static_assert(sizeof(struct msg) == 64, "msg must be 64 bytes");

// Sent from server to client as connection private data.
struct table_info {
    uint64_t addr;
    uint32_t rkey;
    uint32_t nslots;
};

static uint64_t hash_key(const char *k) { // FNV-1a, 64 bit
    uint64_t h = 1469598103934665603ull;
    for (; *k; k++)
        h = (h ^ (uint8_t)*k) * 1099511628211ull;
    return h;
}
static uint32_t slot_crc(const struct slot *s) { // FNV-1a over everything except the crc field itself
    uint32_t h = 2166136261u;
    for (const uint8_t *p = (const uint8_t *)s + 4; p < (const uint8_t *)s + sizeof *s; p++)
        h = (h ^ *p) * 16777619u;
    return h ? h : 1; // never 0, so an all-zero (fresh) slot can't look valid
}

static struct rdma_cm_event *expect_event(struct rdma_event_channel *ec, enum rdma_cm_event_type want) {
    struct rdma_cm_event *ev;
    CHECK_ZERO(rdma_get_cm_event(ec, &ev), "rdma_get_cm_event");
    if (ev->event != want)
        die("unexpected CM event %s (wanted %s), status=%d", rdma_event_str(ev->event), rdma_event_str(want),
            ev->status);
    return ev;
}

/* ================================================================================================ SERVER */
static struct ibv_pd *g_pd;
static struct ibv_mr *g_table_mr;
static struct slot *g_table;
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER; // serializes PUT/DEL (readers use none!)
static unsigned long g_puts;
static unsigned long g_dels;

// Apply PUT/DEL to the table. Runs on the server CPU, under the write lock.
static int table_apply(const struct msg *m) {
    struct slot *w = &g_table[hash_key(m->key) % NSLOTS]; // window start
    struct slot *hit = NULL;
    struct slot *free_ = NULL;
    for (int i = 0; i < PROBE; i++) {
        if (w[i].flags & SLOT_USED) {
            if (!strncmp(w[i].key, m->key, KEYLEN))
                hit = &w[i];
        } else if (!free_)
            free_ = &w[i];
    }
    struct slot *s = hit ? hit : free_;
    if (m->op == OP_DEL) {
        if (!hit)
            return ST_NOTFOUND;
        memset(hit, 0, sizeof *hit); // crc 0 + flags 0 => invalid + unused
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_dels++;
        return ST_OK;
    }
    if (!s)
        return ST_FULL;
    // Torn-read protocol: break the old CRC first, fill the data, publish the new CRC last.
    s->crc = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->flags = SLOT_USED;
    strncpy(s->key, m->key, KEYLEN);
    strncpy(s->val, m->val, VALLEN);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->crc = slot_crc(s);
    g_puts++;
    return ST_OK;
}

struct conn {
    struct rdma_cm_id *id;
    struct ibv_comp_channel *chan;
    struct ibv_cq *cq;
    struct ibv_mr *mr;
    struct msg *buf;
    volatile int closing;
}; // set by the CM thread when the client disconnects
#define RING 8 // buf[0..RING) = receive ring, buf[RING..2*RING) = reply ring

// One thread per client connection.
static void *worker(void *arg) {
    struct conn *c = arg;
    int sidx = 0;
    int n;
    struct ibv_wc wc;
    for (;;) {
        // Sleep until the CQ has something: poll() on the completion channel's fd => ~0% CPU while idle.
        // The 200 ms timeout allows noticing "client disconnected" (flag set by the CM thread) - a QP that
        // went to the error state does not reliably wake the thread on every RDMA stack.
        if (c->closing)
            break;
        struct pollfd pfd = {.fd = c->chan->fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0)
            continue;
        struct ibv_cq *ecq;
        void *ectx;
        if (ibv_get_cq_event(c->chan, &ecq, &ectx))
            continue;
        ibv_ack_cq_events(ecq, 1);
        // Re-arm BEFORE draining: a completion arriving between drain and re-arm would otherwise be missed.
        if (ibv_req_notify_cq(c->cq, 0))
            break;
        int dead = 0;
        while (!dead && (n = ibv_poll_cq(c->cq, 1, &wc)) > 0) {
            if (wc.status != IBV_WC_SUCCESS) { // flushed on disconnect => leave
                dead = 1;
                break;
            }
            if (wc.opcode != IBV_WC_RECV)
                continue; // send completion of a reply: nothing to do
            struct msg *req = &c->buf[wc.wr_id];
            struct msg *rep = &c->buf[RING + (sidx++ % RING)];
            *rep = *req;
            rep->key[KEYLEN - 1] = 0;
            rep->val[VALLEN - 1] = 0;
            pthread_mutex_lock(&g_write_lock);
            rep->status = table_apply(rep);
            pthread_mutex_unlock(&g_write_lock);
            // reply
            struct ibv_sge sg = {.addr = (uintptr_t)rep, .length = sizeof *rep, .lkey = c->mr->lkey};
            struct ibv_send_wr *bs;
            struct ibv_send_wr sw = {.wr_id = 100,
                                     .sg_list = &sg,
                                     .num_sge = 1,
                                     .opcode = IBV_WR_SEND,
                                     .send_flags = IBV_SEND_SIGNALED};
            if (ibv_post_send(c->id->qp, &sw, &bs)) {
                dead = 1;
                break;
            }
            // give the receive buffer back to the NIC
            struct ibv_sge rg = {.addr = (uintptr_t)req, .length = sizeof *req, .lkey = c->mr->lkey};
            struct ibv_recv_wr *br;
            struct ibv_recv_wr rw = {.wr_id = wc.wr_id, .sg_list = &rg, .num_sge = 1};
            if (ibv_post_recv(c->id->qp, &rw, &br)) {
                dead = 1;
                break;
            }
        }
        if (dead)
            break;
    }
    unsigned used = 0;
    for (int i = 0; i < NSLOTS; i++)
        used += (g_table[i].flags & SLOT_USED) != 0;
    printf("server: client gone. table: %u keys, %lu puts, %lu dels applied so far (GETs are invisible to "
           "me!)\n",
           used, g_puts, g_dels);
    rdma_destroy_qp(c->id);
    ibv_destroy_cq(c->cq);
    ibv_destroy_comp_channel(c->chan);
    ibv_dereg_mr(c->mr);
    free(c->buf);
    rdma_destroy_id(c->id);
    free(c);
    return NULL;
}

static void run_server(const char *port) {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    CHECK_PTR(ec, "event channel");
    struct rdma_cm_id *lid;
    CHECK_ZERO(rdma_create_id(ec, &lid, NULL, RDMA_PS_TCP), "create_id");
    struct addrinfo *ai;
    CHECK_ZERO(getaddrinfo(NULL, port, &(struct addrinfo){.ai_flags = AI_PASSIVE, .ai_family = AF_INET}, &ai),
               "gai");
    CHECK_ZERO(rdma_bind_addr(lid, ai->ai_addr), "bind");
    CHECK_ZERO(rdma_listen(lid, 16), "listen");
    printf("server: kvs listening on %s (%d slots x 64 B = %d KiB table)\n", port, NSLOTS,
           NSLOTS * 64 / 1024);

    for (;;) {
        struct rdma_cm_event *ev;
        CHECK_ZERO(rdma_get_cm_event(ec, &ev), "get_cm_event");
        struct rdma_cm_id *id = ev->id;
        enum rdma_cm_event_type t = ev->event;
        struct conn *dc = (t == RDMA_CM_EVENT_DISCONNECTED) ? id->context : NULL;
        rdma_ack_cm_event(ev);
        if (t == RDMA_CM_EVENT_DISCONNECTED) { // tell that client's worker thread to clean up
            if (dc)
                dc->closing = 1; // (worker may free dc any time after this - don't touch it again)
            continue;
        }
        if (t == RDMA_CM_EVENT_ESTABLISHED) {
            printf("server: client connected\n");
            continue;
        }
        if (t != RDMA_CM_EVENT_CONNECT_REQUEST) {
            printf("server: ignoring CM event %s\n", rdma_event_str(t));
            continue;
        }

        // The first connection fixes the device; create the shared PD and register the table once.
        if (!g_pd) {
            g_pd = ibv_alloc_pd(id->verbs);
            CHECK_PTR(g_pd, "alloc_pd");
            size_t bytes = (NSLOTS + PROBE - 1) * sizeof(struct slot);
            g_table = aligned_alloc(4096, (bytes + 4095) & ~4095ul);
            memset(g_table, 0, bytes);
            // REMOTE_READ is the ONLY remote right granted: a buggy or malicious client can't corrupt the table with an RDMA WRITE.
            g_table_mr = ibv_reg_mr(g_pd, g_table, bytes, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
            CHECK_PTR(g_table_mr, "reg table");
        }
        struct conn *c = calloc(1, sizeof *c);
        c->id = id;
        id->context = c;
        c->chan = ibv_create_comp_channel(id->verbs);
        CHECK_PTR(c->chan, "comp channel");
        fcntl(c->chan->fd, F_SETFL,
              fcntl(c->chan->fd, F_GETFL) | O_NONBLOCK); // so ibv_get_cq_event never blocks after poll()
        c->cq = ibv_create_cq(id->verbs, 2 * RING, NULL, c->chan, 0);
        CHECK_PTR(c->cq, "cq");
        c->buf = aligned_alloc(4096, 4096);
        memset(c->buf, 0, 4096);
        c->mr = ibv_reg_mr(g_pd, c->buf, 4096, IBV_ACCESS_LOCAL_WRITE);
        CHECK_PTR(c->mr, "msg mr");
        struct ibv_qp_init_attr qa = {
            .send_cq = c->cq,
            .recv_cq = c->cq,
            .qp_type = IBV_QPT_RC,
            .cap = {.max_send_wr = 2 * RING, .max_recv_wr = RING, .max_send_sge = 1, .max_recv_sge = 1}};
        CHECK_ZERO(rdma_create_qp(id, g_pd, &qa), "create_qp");
        for (int i = 0; i < RING; i++) { // pre-post the whole receive ring
            struct ibv_sge sg = {
                .addr = (uintptr_t)&c->buf[i], .length = sizeof(struct msg), .lkey = c->mr->lkey};
            struct ibv_recv_wr *br;
            struct ibv_recv_wr rw = {.wr_id = i, .sg_list = &sg, .num_sge = 1};
            CHECK_ZERO(ibv_post_recv(id->qp, &rw, &br), "post_recv");
        }
        CHECK_ZERO(ibv_req_notify_cq(c->cq, 0), "arm cq");
        struct table_info ti = {.addr = (uintptr_t)g_table, .rkey = g_table_mr->rkey, .nslots = NSLOTS};
        struct rdma_conn_param p = {.responder_resources = 8,
                                    .initiator_depth = 1,
                                    .rnr_retry_count = 7,
                                    .private_data = &ti,
                                    .private_data_len = sizeof ti};
        CHECK_ZERO(rdma_accept(id, &p), "accept");
        pthread_t th;
        pthread_create(&th, NULL, worker, c);
        pthread_detach(th);
    }
}

/* ================================================================================================ CLIENT */
struct client {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *mr;
    struct table_info ti;
    struct slot *win;      // landing zone for the GET: PROBE slots read from the server
    struct msg *req;       // PUT/DEL request buffer (lives in the same MR)
    struct msg *rep;       // PUT/DEL reply buffer
    unsigned long retries; // torn reads detected
};

static void client_connect(struct client *c, const char *ip, const char *port) {
    c->ec = rdma_create_event_channel();
    CHECK_ZERO(rdma_create_id(c->ec, &c->id, NULL, RDMA_PS_TCP), "create_id");
    struct addrinfo *ai;
    CHECK_ZERO(getaddrinfo(ip, port, &(struct addrinfo){.ai_family = AF_INET}, &ai), "gai");
    CHECK_ZERO(rdma_resolve_addr(c->id, NULL, ai->ai_addr, 2000), "resolve_addr");
    rdma_ack_cm_event(expect_event(c->ec, RDMA_CM_EVENT_ADDR_RESOLVED));
    CHECK_ZERO(rdma_resolve_route(c->id, 2000), "resolve_route");
    rdma_ack_cm_event(expect_event(c->ec, RDMA_CM_EVENT_ROUTE_RESOLVED));
    c->pd = ibv_alloc_pd(c->id->verbs);
    c->cq = ibv_create_cq(c->id->verbs, 16, NULL, NULL, 0);
    struct ibv_qp_init_attr qa = {
        .send_cq = c->cq,
        .recv_cq = c->cq,
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = 8, .max_recv_wr = 8, .max_send_sge = 1, .max_recv_sge = 1}};
    CHECK_ZERO(rdma_create_qp(c->id, c->pd, &qa), "create_qp");
    char *mem = aligned_alloc(4096, 4096);
    memset(mem, 0, 4096);
    c->win = (struct slot *)mem;
    c->req = (struct msg *)(mem + 1024);
    c->rep = (struct msg *)(mem + 2048);
    c->mr = ibv_reg_mr(c->pd, mem, 4096, IBV_ACCESS_LOCAL_WRITE);
    CHECK_PTR(c->mr, "reg_mr");
    struct rdma_conn_param p = {
        .responder_resources = 1, .initiator_depth = 8, .retry_count = 7, .rnr_retry_count = 7};
    CHECK_ZERO(rdma_connect(c->id, &p), "connect");
    struct rdma_cm_event *ev = expect_event(c->ec, RDMA_CM_EVENT_ESTABLISHED);
    if (ev->param.conn.private_data_len < sizeof c->ti)
        die("server sent no table info");
    memcpy(&c->ti, ev->param.conn.private_data, sizeof c->ti);
    rdma_ack_cm_event(ev);
}

// GET: ONE RDMA READ of the whole probe window, then look through it locally.
// returns 1 = found (value copied to out), 0 = not found
static int kv_get(struct client *c, const char *key, char out[VALLEN + 1]) {
    uint64_t idx = hash_key(key) % c->ti.nslots;
    for (;;) {
        struct ibv_sge sg = {
            .addr = (uintptr_t)c->win, .length = PROBE * sizeof(struct slot), .lkey = c->mr->lkey};
        struct ibv_send_wr *bad;
        struct ibv_send_wr wr = {
            .wr_id = 1,
            .sg_list = &sg,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_READ,
            .send_flags = IBV_SEND_SIGNALED,
            .wr.rdma = {.remote_addr = c->ti.addr + idx * sizeof(struct slot), .rkey = c->ti.rkey}};
        CHECK_ZERO(ibv_post_send(c->id->qp, &wr, &bad), "post read");
        wait_completion(c->cq, "rdma read");
        int torn = 0;
        int found = -1;
        for (int i = 0; i < PROBE; i++) {
            struct slot *s = &c->win[i];
            if (!(s->flags & SLOT_USED))
                continue;
            if (s->crc != slot_crc(s)) {
                torn = 1;
                continue;
            } // caught the server mid-update
            if (!strncmp(s->key, key, KEYLEN - 1)) {
                snprintf(out, VALLEN + 1, "%.*s", VALLEN, s->val);
                found = 1;
            }
        }
        if (found == 1)
            return 1;
        if (!torn)
            return 0;
        c->retries++; // the torn slot may hold the searched key: read again
    }
}

// PUT / DEL: SEND the request, wait for the server's SEND reply.
static int kv_write(struct client *c, int op, const char *key, const char *val) {
    memset(c->req, 0, sizeof *c->req);
    c->req->op = op;
    snprintf(c->req->key, KEYLEN, "%s", key);
    if (val)
        snprintf(c->req->val, VALLEN, "%s", val);
    struct ibv_sge rg = {.addr = (uintptr_t)c->rep, .length = sizeof *c->rep, .lkey = c->mr->lkey};
    struct ibv_recv_wr *br;
    struct ibv_recv_wr rw = {.wr_id = 2, .sg_list = &rg, .num_sge = 1};
    CHECK_ZERO(ibv_post_recv(c->id->qp, &rw, &br), "post_recv"); // reply buffer first!
    struct ibv_sge sg = {.addr = (uintptr_t)c->req, .length = sizeof *c->req, .lkey = c->mr->lkey};
    struct ibv_send_wr *bs;
    struct ibv_send_wr wr = {
        .wr_id = 3, .sg_list = &sg, .num_sge = 1, .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED};
    CHECK_ZERO(ibv_post_send(c->id->qp, &wr, &bs), "post_send");
    wait_completion(c->cq, "send");
    wait_completion(c->cq, "reply");
    return c->rep->status;
}

static void do_put(struct client *c, const char *k, const char *v) {
    int st = kv_write(c, OP_PUT, k, v);
    printf("%s\n", st == ST_OK ? "OK" : "ERROR: bucket full");
}
static void do_get(struct client *c, const char *k) {
    char v[VALLEN + 1];
    double t = now_us();
    int f = kv_get(c, k, v);
    double us = now_us() - t;
    if (f)
        printf("%s    (one RDMA READ, %.0f us)\n", v, us);
    else
        printf("(not found)    (%.0f us)\n", us);
}

static void do_bench(struct client *c, int n) {
    char k[KEYLEN];
    char v[VALLEN];
    char out[VALLEN + 1];
    int stored = 0;
    double t0 = now_us();
    for (int i = 0; i < n; i++) {
        snprintf(k, sizeof k, "key%06d", i);
        snprintf(v, sizeof v, "value-%d", i * 7);
        if (kv_write(c, OP_PUT, k, v) == ST_OK)
            stored++;
    }
    double put_us = (now_us() - t0) / n;
    t0 = now_us();
    int bad = 0;
    int miss = 0;
    unsigned long r0 = c->retries;
    for (int rep = 0; rep < 3; rep++)
        for (int i = 0; i < n; i++) {
            snprintf(k, sizeof k, "key%06d", i);
            snprintf(v, sizeof v, "value-%d", i * 7);
            int f = kv_get(c, k, out);
            if (!f)
                miss++;
            else if (strcmp(out, v))
                bad++; // never stored / wrong value
        }
    double get_us = (now_us() - t0) / (3.0 * n);
    printf("bench: %d keys offered, %d stored (rest hit a full 4-slot bucket)\n", n, stored);
    printf("  PUT (SEND/RECV, server CPU)   : %7.1f us/op\n", put_us);
    printf("  GET (RDMA READ, server idle)  : %7.1f us/op   (%lu torn-read retries, %d wrong values, %d "
           "misses of %d GETs)\n",
           get_us, c->retries - r0, bad, miss, 3 * n);
}

static void run_client(const char *ip, const char *port, int ncmd, char **cmd) {
    struct client c = {0};
    client_connect(&c, ip, port);
    printf("connected; table at remote addr 0x%lx, %u slots\n", (unsigned long)c.ti.addr, c.ti.nslots);
    char line[256];
    for (;;) {
        char *w[4] = {0};
        int nw = 0;
        if (ncmd > 0) {
            for (; nw < ncmd && nw < 3; nw++)
                w[nw] = cmd[nw];
        } else {
            printf("kvs> ");
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin))
                break;
            for (char *t = strtok(line, " \t\r\n"); t && nw < 3; t = strtok(NULL, " \t\r\n"))
                w[nw++] = t;
            if (!nw)
                continue;
        }
        if (!strcmp(w[0], "put") && nw == 3)
            do_put(&c, w[1], w[2]);
        else if (!strcmp(w[0], "get") && nw == 2)
            do_get(&c, w[1]);
        else if (!strcmp(w[0], "del") && nw == 2)
            printf("%s\n", kv_write(&c, OP_DEL, w[1], NULL) == ST_OK ? "OK" : "(not found)");
        else if (!strcmp(w[0], "bench"))
            do_bench(&c, nw > 1 ? atoi(w[1]) : 1000);
        else if (!strcmp(w[0], "quit"))
            break;
        else
            printf("commands: put <k> <v> | get <k> | del <k> | bench [N] | quit\n");
        if (ncmd > 0)
            break;
    }
    rdma_disconnect(c.id);
    rdma_ack_cm_event(expect_event(c.ec, RDMA_CM_EVENT_DISCONNECTED));
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "server"))
        run_server(argc > 2 ? argv[2] : DEFAULT_PORT);
    else if (argc == 3 && !strcmp(argv[1], "client"))
        run_client(argv[2], DEFAULT_PORT, 0, NULL);
    else if (argc >= 4 && !strcmp(argv[1], "client"))
        run_client(argv[2], argv[3], argc - 4, argv + 4);
    else {
        fprintf(stderr, "usage: %s server [port] | client <ip> [port [cmd...]]\n", argv[0]);
        return 1;
    }
    return 0;
}
