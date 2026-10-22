/*
 * STEP 12 - Step 6's key-value store on rdmaduct
 * ==============================================
 * The same design as step 6: a table of 64-byte slots in one registered region; PUT and DEL are messages that the
 * server applies; GET is one one-sided READ of the key's probe window, validated by a checksum per slot. What
 * rdmaduct changes:
 *
 *   - setup: duct_ctx_create, duct_listen, duct_connect, duct_mr_reg replace device, protection domain, queue
 *     pairs and connection-manager events (step 4), and the table's descriptor travels in a message;
 *   - one thread serves every client through the endpoints' descriptors (step 9) instead of one thread each;
 *   - the program runs unchanged over TCP, where the server's library executes the READs (step 11), so the
 *     server must keep calling into it;
 *   - GET is also offered as a message (OP_GET), and `bench` compares the two ways to read.
 *
 * Torn reads: the server may rewrite a slot while a client reads it. The server first sets the slot's checksum
 * to 0 (never a valid value), then writes flags, key and value, and writes the new checksum last, with release
 * fences between the steps. A client that reads a used slot whose checksum does not match its contents read it
 * mid-update and reads the window again (the approach of the Pilaf key-value store). On TCP a READ is executed
 * by the server thread between messages, so it never overlaps an update and the retry count stays 0.
 *
 * Table: NSLOTS slots plus PROBE - 1 so that a window never wraps; slot = crc(4) flags(4) key[24] val[32]. A key
 * lives in one of the PROBE slots from hash(key) % NSLOTS on.
 *
 * Usage:
 *   step12_duct_kv server [--transport tcp|rdma] [--port P]
 *   step12_duct_kv client HOST [--transport tcp|rdma] [--port P] COMMAND...
 * Commands: put KEY VALUE | get KEY (one-sided) | rpcget KEY (message) | del KEY | bench [N]
 * bench stores N keys (default 2000) and reads each with both kinds of GET, printing the mean latency.
 */
#include <errno.h>
#include <poll.h>

#include "duct_common.h"

#define DEFAULT_PORT "7495"
#define NSLOTS 65536u /* 4 MiB; with 4096 slots (step 6) a fixed window fills at about 1000 keys */
#define PROBE 4u
#define KEYLEN 24u
#define VALLEN 32u
#define MAX_CLIENTS 64

struct slot {
    uint32_t crc;
    uint32_t flags;
    char key[KEYLEN];
    char val[VALLEN];
};
#define SLOT_USED 1u
_Static_assert(sizeof(struct slot) == 64, "slot must be 64 bytes");

enum { OP_PUT = 1, OP_DEL = 2, OP_GET = 3 };
enum { ST_OK = 0, ST_FULL = 1, ST_NOTFOUND = 2 };

/* Request and response (64 bytes). */
struct msg {
    uint8_t op;
    uint8_t status;
    uint8_t pad[6];
    char key[KEYLEN];
    char val[VALLEN];
};
_Static_assert(sizeof(struct msg) == 64, "message must be 64 bytes");

static uint64_t hash_key(const char *k) { /* FNV-1a, 64 bit */
    uint64_t h = 1469598103934665603ull;
    for (; *k; k++) {
        h = (h ^ (uint8_t)*k) * 1099511628211ull;
    }
    return h;
}

/* FNV-1a over the slot without its crc field; never 0, so that an all-zero slot does not validate. */
static uint32_t slot_crc(const struct slot *s) {
    uint32_t h = 2166136261u;
    for (const uint8_t *p = (const uint8_t *)s + 4; p < (const uint8_t *)s + sizeof(*s); p++) {
        h = (h ^ *p) * 16777619u;
    }
    return h ? h : 1;
}

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

/* ---- server -------------------------------------------------------------------------------------------- */

static struct slot table[NSLOTS + PROBE - 1];

/* Applies one request to the table and fills the response. */
static void apply(const struct msg *m, struct msg *r) {
    struct slot *w = &table[hash_key(m->key) % NSLOTS];
    struct slot *hit = NULL;
    struct slot *free_slot = NULL;
    for (unsigned i = 0; i < PROBE; i++) {
        if (w[i].flags & SLOT_USED) {
            if (strncmp(w[i].key, m->key, KEYLEN) == 0) {
                hit = &w[i];
            }
        } else if (free_slot == NULL) {
            free_slot = &w[i];
        }
    }
    *r = (struct msg){.op = m->op, .status = ST_OK};
    if (m->op == OP_GET) {
        if (hit == NULL) {
            r->status = ST_NOTFOUND;
        } else {
            memcpy(r->val, hit->val, VALLEN);
        }
        return;
    }
    if (m->op == OP_DEL) {
        if (hit == NULL) {
            r->status = ST_NOTFOUND;
            return;
        }
        memset(hit, 0, sizeof(*hit)); /* crc 0 and flags 0: invalid and unused */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        return;
    }
    struct slot *s = hit != NULL ? hit : free_slot;
    if (s == NULL) {
        r->status = ST_FULL;
        return;
    }
    s->crc = 0; /* break the old checksum first */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->flags = SLOT_USED;
    memcpy(s->key, m->key, KEYLEN);
    memcpy(s->val, m->val, VALLEN);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->crc = slot_crc(s); /* publish last */
}

_Noreturn static void run_server(const struct lab_opts *lo) {
    duct_ctx *ctx = make_ctx(lo);
    duct_mr *mr;
    lab_check(duct_mr_reg(ctx, table, sizeof(table), DUCT_ACCESS_REMOTE_READ, &mr), "duct_mr_reg");
    struct duct_mr_desc desc;
    duct_mr_export(mr, &desc);
    duct_listener *l;
    lab_check(duct_listen(ctx, NULL, lo->port, &l), "duct_listen");
    printf("listening on port %d (%s), %u slots\n", duct_listener_port(l),
           lab_transport_name(duct_ctx_transport(ctx)), NSLOTS);
    fflush(stdout);
    duct_ep *eps[MAX_CLIENTS];
    unsigned n = 0;
    for (;;) {
        duct_ep *ep;
        if (n < MAX_CLIENTS && duct_accept(l, 0, &ep) == 0) {
            lab_check(duct_send(ep, &desc, sizeof(desc), 5000), "duct_send");
            eps[n++] = ep;
        }
        struct pollfd fds[MAX_CLIENTS + 1];
        int wait_ms = 100;
        for (unsigned i = 0; i < n; i++) {
            struct duct_rx rx;
            int rc;
            /* Requests are answered in order; on TCP the same calls execute the client's READs. */
            while ((rc = duct_recv(eps[i], &rx, 0)) == 0) {
                struct msg m = {0};
                struct msg r;
                memcpy(&m, rx.data, rx.len < sizeof(m) ? rx.len : sizeof(m));
                duct_rx_release(eps[i], &rx);
                m.key[KEYLEN - 1] = '\0';
                apply(&m, &r);
                if ((rc = duct_send(eps[i], &r, sizeof(r), 1000)) != 0) {
                    break;
                }
            }
            if (rc == -ETIMEDOUT) {
                rc = duct_ep_arm(eps[i]);
            }
            if (rc == -EAGAIN) {
                wait_ms = 0;
            } else if (rc != 0) {
                duct_ep_close(eps[i]); /* the client left */
                eps[i--] = eps[--n];
                continue;
            }
            fds[i] = (struct pollfd){.fd = duct_ep_fd(eps[i]), .events = POLLIN};
        }
        fds[n] = (struct pollfd){.fd = duct_listener_fd(l), .events = POLLIN};
        poll(fds, n + 1, wait_ms);
    }
}

/* ---- client -------------------------------------------------------------------------------------------- */

struct client {
    duct_ep *ep;
    duct_mr *mr;
    struct slot *win; /* PROBE slots of local registered memory */
    struct duct_mr_desc table;
    unsigned long retries;
};

static int call(struct client *c, uint8_t op, const char *key, const char *val, struct msg *r) {
    struct msg m = {.op = op};
    snprintf(m.key, KEYLEN, "%s", key);
    if (val != NULL) {
        snprintf(m.val, VALLEN, "%s", val);
    }
    lab_check(duct_send(c->ep, &m, sizeof(m), 5000), "duct_send");
    struct duct_rx rx;
    lab_check(duct_recv(c->ep, &rx, 5000), "duct_recv");
    memcpy(r, rx.data, rx.len == sizeof(*r) ? sizeof(*r) : 0);
    duct_rx_release(c->ep, &rx);
    return r->status;
}

/* GET by one READ of the probe window. Returns 1 and the value when found, 0 when absent. */
static int get_one_sided(struct client *c, const char *key, char out[VALLEN + 1]) {
    char k[KEYLEN];
    snprintf(k, sizeof(k), "%s", key);
    uint64_t idx = hash_key(k) % NSLOTS;
    for (;;) {
        uint64_t seq;
        lab_check(duct_read(c->ep, c->mr, 0, &c->table, idx * sizeof(struct slot),
                            PROBE * sizeof(struct slot), 5000, &seq),
                  "duct_read");
        lab_check(duct_wait(c->ep, seq, 5000), "duct_wait");
        int torn = 0;
        for (unsigned i = 0; i < PROBE; i++) {
            const struct slot *s = &c->win[i];
            if (!(s->flags & SLOT_USED)) {
                continue;
            }
            if (s->crc != slot_crc(s)) {
                torn = 1; /* read during an update */
                continue;
            }
            if (strncmp(s->key, k, KEYLEN) == 0) {
                snprintf(out, VALLEN + 1, "%.*s", (int)VALLEN, s->val);
                return 1;
            }
        }
        if (!torn) {
            return 0;
        }
        c->retries++; /* the torn slot may hold the key */
    }
}

static int bench(struct client *c, unsigned n) {
    char key[KEYLEN];
    char val[VALLEN];
    char out[VALLEN + 1];
    struct msg r;
    for (unsigned i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "key%u", i);
        snprintf(val, sizeof(val), "value%u", i);
        if (call(c, OP_PUT, key, val, &r) != ST_OK) {
            printf("put %s: table window full\n", key);
            return 1;
        }
    }
    int bad = 0;
    double t0 = lab_now();
    for (unsigned i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "key%u", i);
        snprintf(val, sizeof(val), "value%u", i);
        bad |= !get_one_sided(c, key, out) || strcmp(out, val) != 0;
    }
    double one_sided = (lab_now() - t0) / n;
    t0 = lab_now();
    for (unsigned i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "key%u", i);
        snprintf(val, sizeof(val), "value%u", i);
        bad |= call(c, OP_GET, key, NULL, &r) != ST_OK || strncmp(r.val, val, VALLEN) != 0;
    }
    double rpc = (lab_now() - t0) / n;
    printf("%u keys: one-sided GET %.1f us, message GET %.1f us (mean), %lu torn reads retried%s\n", n,
           one_sided * 1e6, rpc * 1e6, c->retries, bad ? ", WRONG VALUES" : "");
    return bad;
}

static int run_client(const char *host, const struct lab_opts *lo, int argc, char **argv) {
    duct_ctx *ctx = make_ctx(lo);
    static struct slot win[PROBE];
    struct client c = {.win = win};
    lab_check(duct_connect(ctx, host, lo->port, &c.ep), "duct_connect");
    struct duct_rx rx;
    lab_check(duct_recv(c.ep, &rx, 5000), "duct_recv");
    if (rx.len != sizeof(c.table)) {
        fprintf(stderr, "expected the table descriptor\n");
        return 1;
    }
    memcpy(&c.table, rx.data, sizeof(c.table));
    duct_rx_release(c.ep, &rx);
    lab_check(duct_mr_reg(ctx, win, sizeof(win), DUCT_ACCESS_LOCAL, &c.mr), "duct_mr_reg");
    int rc = 2;
    struct msg r;
    char out[VALLEN + 1];
    if (argc == 3 && strcmp(argv[0], "put") == 0) {
        rc = call(&c, OP_PUT, argv[1], argv[2], &r) == ST_OK ? 0 : 1;
        printf("%s\n", rc == 0 ? "OK" : "FULL");
    } else if (argc == 2 && strcmp(argv[0], "get") == 0) {
        rc = get_one_sided(&c, argv[1], out) ? 0 : 1;
        printf("%s\n", rc == 0 ? out : "(not found)");
    } else if (argc == 2 && strcmp(argv[0], "rpcget") == 0) {
        rc = call(&c, OP_GET, argv[1], NULL, &r) == ST_OK ? 0 : 1;
        printf("%.*s\n", (int)VALLEN, rc == 0 ? r.val : "(not found)");
    } else if (argc == 2 && strcmp(argv[0], "del") == 0) {
        rc = call(&c, OP_DEL, argv[1], NULL, &r) == ST_OK ? 0 : 1;
        printf("%s\n", rc == 0 ? "OK" : "(not found)");
    } else if ((argc == 1 || argc == 2) && strcmp(argv[0], "bench") == 0) {
        unsigned n = argc == 2 ? (unsigned)strtoul(argv[1], NULL, 10) : 2000;
        rc = n > 0 ? bench(&c, n) : 2;
    } else {
        fprintf(stderr, "commands: put KEY VALUE | get KEY | rpcget KEY | del KEY | bench [N]\n");
    }
    duct_mr_dereg(c.mr);
    duct_ep_close(c.ep);
    duct_ctx_destroy(ctx);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0) ||
        (strcmp(argv[1], "client") == 0 && argc < 4)) {
        fprintf(stderr, "usage: %s server [options] | client HOST [options] COMMAND...\n", argv[0]);
        return 2;
    }
    struct lab_opts lo = {.transport = DUCT_TRANSPORT_TCP, .wait = DUCT_WAIT_POLL, .port = DEFAULT_PORT};
    int client = strcmp(argv[1], "client") == 0;
    int i = duct_lab_parse(argc, argv, client ? 3 : 2, &lo);
    if (!client) {
        if (i != argc) {
            return 2;
        }
        run_server(&lo);
    }
    return run_client(argv[2], &lo, argc - i, argv + i);
}
