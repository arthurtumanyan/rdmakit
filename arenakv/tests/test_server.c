/*
 * test_server.c - server integration tests over TCP and, when DUCT_TEST_ADDR is set, RDMA (both wait modes).
 *
 * Covers protocol round trips, malformed requests, size limits on both sides, concurrent clients with
 * self-describing values, connection accounting, and shutdown while clients are connected.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akvclient.h"
#include "akvd.h"

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

#define SERVER_MSG (64u * 1024u)

static struct duct_opts client_opts(enum duct_transport t, enum duct_wait w, uint32_t max_msg) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = t;
    o.wait = w;
    o.max_msg = max_msg;
    o.recv_depth = 8;
    o.send_depth = 8;
    o.connect_timeout_ms = 5000;
    return o;
}

static akvd *start_server(enum duct_transport t, enum duct_wait w, const char *addr, uint64_t capacity,
                          uint32_t shards) {
    akvd_config cfg;
    akvd_config_default(&cfg);
    cfg.transport = t;
    cfg.wait = w;
    cfg.addr = addr;
    cfg.port = "0";
    cfg.max_msg = SERVER_MSG;
    cfg.store.capacity = capacity;
    cfg.store.shards = shards;
    cfg.store.max_entry = SERVER_MSG;
    cfg.store.avg_entry = 512;
    int err = 0;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "server start: %s", duct_strerror(err));
    return d;
}

static akvc *connect_client(enum duct_transport t, enum duct_wait w, const char *addr, int port,
                            uint32_t max_msg) {
    struct duct_opts o = client_opts(t, w, max_msg);
    char p[16];
    snprintf(p, sizeof(p), "%d", port);
    akvc *c = NULL;
    int rc = akvc_connect(&c, &o, addr, p);
    EXPECT(rc == 0, "client connect: %s", duct_strerror(rc));
    return c;
}

static void test_roundtrip(akvc *c) {
    static uint8_t buf[SERVER_MSG];
    size_t n = 0;
    EXPECT(akvc_get(c, "missing", 7, buf, sizeof(buf), &n) == AKVP_NOTFOUND, "missing key");
    EXPECT(akvc_put(c, "alpha", 5, "hello", 5) == AKVP_OK, "put");
    EXPECT(akvc_get(c, "alpha", 5, buf, sizeof(buf), &n) == AKVP_OK && n == 5 && memcmp(buf, "hello", 5) == 0,
           "get");
    EXPECT(akvc_put(c, "alpha", 5, "x\0y", 3) == AKVP_OK, "overwrite with binary value");
    EXPECT(akvc_get(c, "alpha", 5, buf, sizeof(buf), &n) == AKVP_OK && n == 3 && memcmp(buf, "x\0y", 3) == 0,
           "binary value");
    EXPECT(akvc_put(c, "empty", 5, "", 0) == AKVP_OK, "empty value");
    EXPECT(akvc_get(c, "empty", 5, buf, sizeof(buf), &n) == AKVP_OK && n == 0, "empty value read");
    EXPECT(akvc_del(c, "alpha", 5) == AKVP_OK, "delete");
    EXPECT(akvc_del(c, "alpha", 5) == AKVP_NOTFOUND, "delete twice");
    EXPECT(akvc_get(c, "alpha", 5, buf, sizeof(buf), &n) == AKVP_NOTFOUND, "deleted key");

    /* value sizes up to the message limit, key sizes from 1 byte to 4 KiB */
    uint8_t *val = malloc(SERVER_MSG);
    uint8_t *key = malloc(4096);
    for (size_t i = 0; i < 4096; i++) {
        key[i] = (uint8_t)('a' + i % 23);
    }
    static const size_t vsizes[] = {1,    7,    8,     9,     100,
                                    1000, 4096, 16384, 40000, SERVER_MSG - AKVP_REQ_HDR - 64};
    for (unsigned i = 0; i < sizeof(vsizes) / sizeof(vsizes[0]); i++) {
        size_t klen = 1 + (i * 457) % 64;
        for (size_t k = 0; k < vsizes[i]; k++) {
            val[k] = (uint8_t)(k * 13 + i);
        }
        key[0] = (uint8_t)('A' + i);
        int rc = akvc_put(c, key, klen, val, vsizes[i]);
        EXPECT(rc == AKVP_OK, "put size %zu: %d", vsizes[i], rc);
        memset(buf, 0, 16);
        rc = akvc_get(c, key, klen, buf, sizeof(buf), &n);
        EXPECT(rc == AKVP_OK && n == vsizes[i] && memcmp(buf, val, n) == 0, "get size %zu: rc=%d n=%zu",
               vsizes[i], rc, n);
    }
    EXPECT(akvc_put(c, key, 4096, "long", 4) == AKVP_OK, "4 KiB key");
    EXPECT(akvc_get(c, key, 4096, buf, sizeof(buf), &n) == AKVP_OK && n == 4, "4 KiB key read");
    free(val);
    free(key);

    uint64_t st[AKVP_STATS_FIELDS];
    EXPECT(akvc_stats(c, st) == AKVP_OK, "stats");
    EXPECT(st[0] > 0 && st[3] > 0 && st[4] > 0 && st[5] > 0,
           "stats content: entries=%llu gets=%llu hits=%llu puts=%llu", (unsigned long long)st[0],
           (unsigned long long)st[3], (unsigned long long)st[4], (unsigned long long)st[5]);
}

/* Raw requests with defects: answered with AKVP_BADREQ and the connection stays usable. */
static void test_malformed(akvc *c) {
    const uint8_t *resp;
    size_t rlen;
    uint8_t msg[64];
    struct akvp_resp rs;

    struct akvp_req bad_op = {.op = 99, .klen = 1, .reqid = 41, .vlen = 0};
    akvp_req_encode(msg, &bad_op);
    msg[AKVP_REQ_HDR] = 'k';
    EXPECT(akvc_raw(c, msg, AKVP_REQ_HDR + 1, &resp, &rlen) == 0, "raw send");
    EXPECT(akvp_resp_decode(resp, rlen, &rs) == 0 && rs.status == AKVP_BADREQ && rs.reqid == 41,
           "unknown op");

    struct akvp_req wrong_len = {.op = AKVP_GET, .klen = 5, .reqid = 42, .vlen = 0};
    akvp_req_encode(msg, &wrong_len);
    memcpy(msg + AKVP_REQ_HDR, "abc", 3);
    EXPECT(akvc_raw(c, msg, AKVP_REQ_HDR + 3, &resp, &rlen) == 0, "raw send");
    EXPECT(akvp_resp_decode(resp, rlen, &rs) == 0 && rs.status == AKVP_BADREQ && rs.reqid == 42,
           "length mismatch");

    struct akvp_req no_key = {.op = AKVP_GET, .klen = 0, .reqid = 43, .vlen = 0};
    akvp_req_encode(msg, &no_key);
    EXPECT(akvc_raw(c, msg, AKVP_REQ_HDR, &resp, &rlen) == 0, "raw send");
    EXPECT(akvp_resp_decode(resp, rlen, &rs) == 0 && rs.status == AKVP_BADREQ && rs.reqid == 43, "empty key");

    struct akvp_req get_with_value = {.op = AKVP_GET, .klen = 1, .reqid = 44, .vlen = 2};
    akvp_req_encode(msg, &get_with_value);
    memcpy(msg + AKVP_REQ_HDR, "kvv", 3);
    EXPECT(akvc_raw(c, msg, AKVP_REQ_HDR + 3, &resp, &rlen) == 0, "raw send");
    EXPECT(akvp_resp_decode(resp, rlen, &rs) == 0 && rs.status == AKVP_BADREQ && rs.reqid == 44,
           "GET carrying a value");

    struct akvp_req flags = {.op = AKVP_GET, .klen = 1, .reqid = 45, .vlen = 0};
    akvp_req_encode(msg, &flags);
    msg[1] = 0x80;
    msg[AKVP_REQ_HDR] = 'k';
    EXPECT(akvc_raw(c, msg, AKVP_REQ_HDR + 1, &resp, &rlen) == 0, "raw send");
    EXPECT(akvp_resp_decode(resp, rlen, &rs) == 0 && rs.status == AKVP_BADREQ && rs.reqid == 45,
           "reserved flags set");

    uint8_t buf[32];
    size_t n;
    EXPECT(akvc_put(c, "still", 5, "works", 5) == AKVP_OK, "connection usable after bad requests");
    EXPECT(akvc_get(c, "still", 5, buf, sizeof(buf), &n) == AKVP_OK && n == 5, "read after bad requests");
}

static void test_short_message_closes(enum duct_transport t, enum duct_wait w, const char *addr, int port) {
    akvc *c = connect_client(t, w, addr, port, SERVER_MSG);
    const uint8_t *resp;
    size_t rlen;
    uint8_t tiny[5] = {1, 0, 0, 0, 0};
    int rc = akvc_raw(c, tiny, sizeof(tiny), &resp, &rlen);
    EXPECT(rc != 0, "message shorter than a header must not be answered (rc=%d)", rc);
    akvc_close(c);
}

/* A small-limit client reads a value larger than its message limit: the server answers AKVP_TOOBIG. */
static void test_limits(enum duct_transport t, enum duct_wait w, const char *addr, int port) {
    akvc *big = connect_client(t, w, addr, port, SERVER_MSG);
    akvc *small = connect_client(t, w, addr, port, 4096);
    static uint8_t val[20000];
    static uint8_t out[SERVER_MSG];
    memset(val, 0x5a, sizeof(val));
    size_t n = 0;
    EXPECT(akvc_put(big, "bulky", 5, val, sizeof(val)) == AKVP_OK, "store a large value");
    EXPECT(akvc_get(small, "bulky", 5, out, sizeof(out), &n) == AKVP_TOOBIG,
           "response above the client's limit");
    EXPECT(akvc_put(small, "small", 5, "ok", 2) == AKVP_OK, "small client still works");
    EXPECT(akvc_put(small, "k", 1, val, sizeof(val)) == AKVP_TOOBIG,
           "request above the client's limit is refused locally");
    EXPECT(akvc_get(big, "bulky", 5, out, 100, &n) == AKVP_TOOBIG && n == sizeof(val),
           "caller buffer too small reports the size");
    akvc_close(small);
    akvc_close(big);
}

struct hammer {
    enum duct_transport t;
    enum duct_wait w;
    const char *addr;
    int port;
    unsigned id;
    unsigned ops;
    int bad;
};

static void make_val(uint8_t *buf, uint64_t id, uint32_t len) {
    memcpy(buf, &id, 8);
    memcpy(buf + 8, &len, 4);
    for (uint32_t i = 12; i < len; i++) {
        buf[i] = (uint8_t)(id + i);
    }
}

static void *hammer_main(void *arg) {
    struct hammer *h = arg;
    akvc *c = connect_client(h->t, h->w, h->addr, h->port, SERVER_MSG);
    if (c == NULL) {
        h->bad++;
        return NULL;
    }
    uint8_t val[3000];
    uint8_t out[3100];
    uint64_t state = 0x9e3779b97f4a7c15ull * (h->id + 1);
    for (unsigned i = 0; i < h->ops; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint64_t id = state % 3000;
        char key[24];
        size_t klen = (size_t)snprintf(key, sizeof(key), "h%llu", (unsigned long long)id);
        unsigned op = (unsigned)((state >> 20) % 100);
        if (op < 35) {
            uint32_t len = 12 + (uint32_t)((state >> 32) % 2800);
            make_val(val, id, len);
            if (akvc_put(c, key, klen, val, len) != AKVP_OK) {
                h->bad++;
            }
        } else if (op < 95) {
            size_t n = 0;
            int rc = akvc_get(c, key, klen, out, sizeof(out), &n);
            if (rc == AKVP_OK) {
                uint8_t expect[3100];
                make_val(expect, id, (uint32_t)n);
                uint32_t stored_len;
                memcpy(&stored_len, out + 8, 4);
                if (n < 12 || stored_len != n || memcmp(out, expect, n) != 0) {
                    h->bad++;
                }
            } else if (rc != AKVP_NOTFOUND) {
                h->bad++;
            }
        } else {
            int rc = akvc_del(c, key, klen);
            if (rc != AKVP_OK && rc != AKVP_NOTFOUND) {
                h->bad++;
            }
        }
    }
    akvc_close(c);
    return NULL;
}

static void test_concurrent(enum duct_transport t, enum duct_wait w, const char *addr, int port,
                            unsigned ops) {
    enum { CLIENTS = 6 };
    struct hammer hs[CLIENTS];
    pthread_t th[CLIENTS];
    for (unsigned i = 0; i < CLIENTS; i++) {
        hs[i] = (struct hammer){.t = t, .w = w, .addr = addr, .port = port, .id = i, .ops = ops};
        pthread_create(&th[i], NULL, hammer_main, &hs[i]);
    }
    int bad = 0;
    for (unsigned i = 0; i < CLIENTS; i++) {
        pthread_join(th[i], NULL);
        bad += hs[i].bad;
    }
    EXPECT(bad == 0, "%d inconsistent responses from concurrent clients", bad);
}

static void run(enum duct_transport t, enum duct_wait w, const char *addr, unsigned ops) {
    /* One shard of 4 MiB: values above 2 MiB are refused by the engine (AKVP_TOOBIG); eviction is active under the hammer test. */
    akvd *d = start_server(t, w, addr, 6u << 20, 4);
    if (d == NULL) {
        return;
    }
    int port = akvd_port(d);
    akvc *c = connect_client(t, w, addr, port, SERVER_MSG);
    if (c != NULL) {
        test_roundtrip(c);
        test_malformed(c);
        akvc_close(c);
    }
    test_short_message_closes(t, w, addr, port);
    test_limits(t, w, addr, port);
    test_concurrent(t, w, addr, port, ops);

    /* All client connections close; the server notices. */
    for (int i = 0; i < 100 && akvd_connections(d) != 0; i++) {
        usleep(50000);
    }
    EXPECT(akvd_connections(d) == 0, "%u connections still open after all clients closed",
           akvd_connections(d));

    /* Shutdown with a connected client: stop must not hang and the client must see the end. */
    akvc *lingering = connect_client(t, w, addr, port, SERVER_MSG);
    akvd_stop(d);
    uint8_t buf[16];
    size_t n;
    if (lingering != NULL) {
        int rc = akvc_get(lingering, "x", 1, buf, sizeof(buf), &n);
        EXPECT(rc < 0, "request after server shutdown returned %d", rc);
        akvc_close(lingering);
    }
}

static void test_engine_limit(enum duct_transport t, enum duct_wait w, const char *addr) {
    akvd *d = start_server(t, w, addr, 128u << 10, 1); /* arena 128 KiB: entries above 64 KiB are refused */
    if (d == NULL) {
        return;
    }
    akvc *c = connect_client(t, w, addr, akvd_port(d), SERVER_MSG);
    /* Largest value a 4-byte key allows within the message limit: 28 + 65520 bytes exceed half of the 128 KiB arena. */
    static uint8_t val[SERVER_MSG - AKVP_REQ_HDR - 4];
    memset(val, 1, sizeof(val));
    if (c != NULL) {
        EXPECT(akvc_put(c, "huge", 4, val, sizeof(val)) == AKVP_TOOBIG, "entry above half the arena");
        EXPECT(akvc_put(c, "fits", 4, val, 20000) == AKVP_OK, "entry within the limit");
        akvc_close(c);
    }
    akvd_stop(d);
}

int main(void) {
    run(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1", 40000);
    test_engine_limit(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1");
    const char *addr = getenv("DUCT_TEST_ADDR");
    if (addr != NULL && addr[0] != '\0') {
        for (int w = 0; w < 2; w++) {
            enum duct_wait wait = w == 0 ? DUCT_WAIT_POLL : DUCT_WAIT_EVENT;
            run(DUCT_TRANSPORT_RDMA, wait, addr, 15000);
            test_engine_limit(DUCT_TRANSPORT_RDMA, wait, addr);
        }
    } else {
        puts("test_server: RDMA cases skipped (DUCT_TEST_ADDR not set)");
    }
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_server: ok");
    return 0;
}
