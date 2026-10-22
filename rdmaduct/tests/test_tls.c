/*
 * test_tls.c - encrypted endpoints (TLS=1 builds): every case over TCP on loopback, then over RDMA in both
 * waiting modes when the library has RDMA support and DUCT_TEST_ADDR holds the address of an RDMA interface.
 *
 * Checks: echo of every size from 0 to the reduced message limit with certificate verification by IP
 * address and by DNS name, batches and partial batches, a server that waits through duct_ep_arm() and
 * poll() (the non-blocking handshake path), a wrong CA, a wrong name, a client certificate required by the
 * server, an unencrypted peer on either side, a ciphertext byte changed in transit by a relay, a handshake
 * that never starts (connect timeout), key updates every few messages, and the operations that are not
 * available on encrypted endpoints. Without TLS=1 the test checks that encryption is refused.
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#ifndef DUCT_HAVE_TLS

int main(void) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = DUCT_TRANSPORT_TCP;
    o.tls = 1;
    duct_ctx *ctx;
    int rc = duct_ctx_create(&o, &ctx);
    EXPECT(rc == -ENOTSUP, "encryption without TLS=1: %s", duct_strerror(rc));
    printf("test_tls: %s (built without TLS=1)\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

#else

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#define MAX_MSG 4096u
#define OVERHEAD 17u

/* Transport, waiting mode and address of the current pass (TCP on loopback, then RDMA when configured). */
static enum duct_transport g_transport = DUCT_TRANSPORT_TCP;
static enum duct_wait g_wait = DUCT_WAIT_POLL;
static const char *g_addr = "127.0.0.1";

/* Writes a self-signed certificate (valid for one day, subjectAltName IP:127.0.0.1, the RDMA test address
 * when set, and DNS:localhost) and its key. A self-signed certificate is its own trust anchor, so the file also serves as the CA bundle. */
static int make_cert(const char *cert_path, const char *key_path) {
    EVP_PKEY *pk = EVP_EC_gen("P-256");
    X509 *x = X509_new();
    if (pk == NULL || x == NULL) {
        return -1;
    }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 86400);
    X509_set_pubkey(x, pk);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"rdmaduct test", -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, NULL, NULL, 0);
    char names[128];
    const char *rdma_addr = getenv("DUCT_TEST_ADDR");
    if (rdma_addr != NULL && *rdma_addr != '\0') {
        snprintf(names, sizeof(names), "IP:127.0.0.1,IP:%s,DNS:localhost", rdma_addr);
    } else {
        snprintf(names, sizeof(names), "IP:127.0.0.1,DNS:localhost");
    }
    X509_EXTENSION *san = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, names);
    int ok = san != NULL && X509_add_ext(x, san, -1) == 1 && X509_sign(x, pk, EVP_sha256()) > 0;
    X509_EXTENSION_free(san);
    FILE *cf = fopen(cert_path, "w");
    FILE *kf = fopen(key_path, "w");
    ok = ok && cf != NULL && kf != NULL && PEM_write_X509(cf, x) == 1 &&
         PEM_write_PrivateKey(kf, pk, NULL, NULL, 0, NULL, NULL) == 1;
    if (cf != NULL) {
        fclose(cf);
    }
    if (kf != NULL) {
        fclose(kf);
    }
    X509_free(x);
    EVP_PKEY_free(pk);
    return ok ? 0 : -1;
}

static char cert_a[64];
static char key_a[64];
static char cert_b[64];
static char key_b[64];

/* Context options of one side. cert/ca NULL: none. */
static int make_ctx(int tls, const char *cert, const char *key, const char *ca, const char *name,
                    int timeout_ms, duct_ctx **out) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = g_transport;
    o.wait = g_wait;
    o.max_msg = MAX_MSG;
    o.recv_depth = 8;
    o.send_depth = 8;
    o.connect_timeout_ms = timeout_ms;
    o.tls = tls;
    o.tls_cert = cert;
    o.tls_key = key;
    o.tls_ca = ca;
    o.tls_server_name = name;
    return duct_ctx_create(&o, out);
}

/* Echo server for one connection. use_poll: wait through duct_ep_arm() and poll() and use timeout 0. */
struct server {
    duct_listener *l;
    int use_poll;
    int end_code;
    atomic_ulong echoed;
};

static void *server_main(void *arg) {
    struct server *s = arg;
    duct_ep *ep;
    int rc = duct_accept(s->l, 10000, &ep);
    if (rc != 0) {
        s->end_code = rc;
        return NULL;
    }
    for (;;) {
        struct duct_rx rx;
        if (s->use_poll) {
            rc = duct_ep_arm(ep);
            if (rc == 0) {
                struct pollfd p = {.fd = duct_ep_fd(ep), .events = POLLIN};
                poll(&p, 1, 1000);
            } else if (rc != -EAGAIN) {
                break;
            }
            rc = duct_recv(ep, &rx, 0);
            if (rc == -ETIMEDOUT) {
                continue;
            }
        } else {
            rc = duct_recv(ep, &rx, 10000);
        }
        if (rc != 0) {
            break;
        }
        rc = duct_send(ep, rx.data, rx.len, 10000);
        duct_rx_release(ep, &rx);
        if (rc != 0) {
            break;
        }
        atomic_fetch_add(&s->echoed, 1);
    }
    s->end_code = rc;
    duct_ep_close(ep);
    return NULL;
}

static void port_of(duct_listener *l, char *buf, size_t n) {
    snprintf(buf, n, "%d", duct_listener_port(l));
}

static void fill(unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)(seed * 131u + i * 7u + 3u);
    }
}

static int echo(duct_ep *ep, size_t len, unsigned seed) {
    unsigned char out[MAX_MSG];
    fill(out, len, seed);
    int rc = duct_send(ep, out, len, 5000);
    if (rc != 0) {
        return rc;
    }
    struct duct_rx rx;
    rc = duct_recv(ep, &rx, 5000);
    if (rc != 0) {
        return rc;
    }
    int same = rx.len == len && memcmp(rx.data, out, len) == 0;
    duct_rx_release(ep, &rx);
    return same ? 0 : -EBADMSG;
}

/* Server and client contexts with the given options; runs a connection attempt and returns the connect
 * result and the server's end code. On success, check() runs on the client endpoint first. */
struct pair_opts {
    int server_tls;
    const char *server_cert;
    const char *server_ca;
    int client_tls;
    const char *client_cert;
    const char *client_ca;
    const char *name;
    const char *addr;
    int use_poll;
};

static int run_pair(const struct pair_opts *p, void (*check)(duct_ep *), int *server_end) {
    duct_ctx *sc;
    duct_ctx *cc;
    const char *skey = p->server_cert == cert_a ? key_a : key_b;
    const char *ckey = p->client_cert == cert_a ? key_a : key_b;
    int rc =
        make_ctx(p->server_tls, p->server_cert, p->server_cert ? skey : NULL, p->server_ca, NULL, 3000, &sc);
    EXPECT(rc == 0, "server context: %s", duct_strerror(rc));
    rc = make_ctx(p->client_tls, p->client_cert, p->client_cert ? ckey : NULL, p->client_ca, p->name, 3000,
                  &cc);
    EXPECT(rc == 0, "client context: %s", duct_strerror(rc));
    struct server s = {.use_poll = p->use_poll};
    rc = duct_listen(sc, g_addr, "0", &s.l);
    EXPECT(rc == 0, "listen: %s", duct_strerror(rc));
    char port[16];
    port_of(s.l, port, sizeof(port));
    pthread_t th;
    pthread_create(&th, NULL, server_main, &s);
    duct_ep *ep = NULL;
    int crc = duct_connect(cc, p->addr != NULL ? p->addr : g_addr, port, &ep);
    if (crc == 0) {
        if (check != NULL) {
            check(ep);
        }
        duct_ep_close(ep);
    }
    pthread_join(th, NULL);
    *server_end = s.end_code;
    duct_listener_close(s.l);
    duct_ctx_destroy(sc);
    duct_ctx_destroy(cc);
    return crc;
}

static void check_echo(duct_ep *ep) {
    EXPECT(duct_ep_peer_max_msg(ep) == MAX_MSG - OVERHEAD, "peer max_msg %u", duct_ep_peer_max_msg(ep));
    size_t limit = MAX_MSG - OVERHEAD;
    int bad = 0;
    for (size_t len = 0; len <= limit; len++) {
        bad += echo(ep, len, (unsigned)len) != 0;
    }
    EXPECT(bad == 0, "%d of %zu sizes failed", bad, limit + 1);
    unsigned char big[MAX_MSG];
    EXPECT(duct_send(ep, big, limit + 1, 1000) == -EMSGSIZE, "oversize message rejected");
    /* Batches of 16 messages, then partial batches through duct_send_some(). */
    unsigned char bufs[16][256];
    struct duct_msg msgs[16];
    for (unsigned round = 0; round < 50; round++) {
        for (unsigned i = 0; i < 16; i++) {
            fill(bufs[i], sizeof(bufs[i]), round * 16 + i);
            msgs[i] = (struct duct_msg){.data = bufs[i], .len = 1 + (round * 16 + i) % sizeof(bufs[i])};
        }
        int rc;
        if (round % 2 == 0) {
            rc = duct_send_batch(ep, msgs, 16, 5000);
        } else {
            unsigned off = 0;
            rc = 0;
            while (off < 16 && rc >= 0) {
                rc = duct_send_some(ep, msgs + off, 16 - off);
                off += rc > 0 ? (unsigned)rc : 0;
                if (rc == 0) {
                    rc = duct_progress(ep);
                }
            }
        }
        EXPECT(rc >= 0, "batch %u: %s", round, duct_strerror(rc));
        for (unsigned i = 0; i < 16; i++) {
            struct duct_rx rx;
            rc = duct_recv(ep, &rx, 5000);
            EXPECT(rc == 0 && rx.len == msgs[i].len && memcmp(rx.data, msgs[i].data, rx.len) == 0,
                   "batch %u message %u: %s", round, i, duct_strerror(rc));
            if (rc == 0) {
                duct_rx_release(ep, &rx);
            }
        }
    }
    duct_mr *mr;
    static uint8_t region[64];
    EXPECT(duct_mr_reg(ep->ctx, region, sizeof(region), DUCT_ACCESS_REMOTE_READ, &mr) == 0,
           "register region");
    EXPECT(duct_send_mr(ep, mr, 0, 8, 1000, NULL) == -ENOTSUP, "duct_send_mr on an encrypted endpoint");
    struct duct_mr_desc desc;
    duct_mr_export(mr, &desc);
    EXPECT(duct_read(ep, mr, 0, &desc, 0, 8, 1000, NULL) == -ENOTSUP, "duct_read on an encrypted endpoint");
    EXPECT(duct_wait(ep, 1, 0) == -ENOTSUP, "duct_wait on an encrypted endpoint");
    duct_mr_dereg(mr);
    EXPECT(echo(ep, 100, 7) == 0, "endpoint usable after refused operations");
}

static void check_short(duct_ep *ep) {
    for (unsigned i = 0; i < 20; i++) {
        EXPECT(echo(ep, i * 100, i) == 0, "echo %u", i);
    }
}

/* Sends one unencrypted message and waits until the server closes the connection. */
static void check_plain(duct_ep *ep) {
    duct_send(ep, "\x02plain", 6, 1000);
    struct duct_rx rx;
    int rc = duct_recv(ep, &rx, 5000);
    EXPECT(rc == DUCT_ECLOSED, "unencrypted client sees the close: %s", duct_strerror(rc));
}

/* Key updates every 5 messages: single sends and batches cross the boundaries in both directions. */
static void check_rekey(duct_ep *ep) {
    duct_tls_set_rekey_interval(5);
    check_short(ep);
    unsigned char bufs[13][64];
    struct duct_msg msgs[13];
    for (unsigned round = 0; round < 20; round++) {
        for (unsigned i = 0; i < 13; i++) {
            fill(bufs[i], sizeof(bufs[i]), round + i);
            msgs[i] = (struct duct_msg){.data = bufs[i], .len = sizeof(bufs[i])};
        }
        int rc = duct_send_batch(ep, msgs, 13, 5000);
        EXPECT(rc == 0, "rekey batch %u: %s", round, duct_strerror(rc));
        for (unsigned i = 0; i < 13 && rc == 0; i++) {
            struct duct_rx rx;
            rc = duct_recv(ep, &rx, 5000);
            EXPECT(rc == 0 && rx.len == 64 && memcmp(rx.data, bufs[i], 64) == 0,
                   "rekey batch %u message %u: %s", round, i, duct_strerror(rc));
            if (rc == 0) {
                duct_rx_release(ep, &rx);
            }
        }
    }
    duct_tls_set_rekey_interval(1ull << 24);
}

/* ---- tampering relay ---------------------------------------------------------------------------------- */

/* Forwards messages between two unencrypted endpoints; flips one byte of the data message number `flip`
 * (counted over client-to-server data messages, type byte 2). */
struct relay {
    duct_listener *l;
    duct_ctx *ctx;
    char port[16];
    unsigned flip;
};

static void *relay_main(void *arg) {
    struct relay *r = arg;
    duct_ep *down;
    duct_ep *up;
    if (duct_accept(r->l, 10000, &down) != 0) {
        return NULL;
    }
    if (duct_connect(r->ctx, g_addr, r->port, &up) != 0) {
        duct_ep_close(down);
        return NULL;
    }
    unsigned data = 0;
    int run = 1;
    while (run) {
        struct duct_rx rx;
        int idle = 1;
        int rc = duct_recv(down, &rx, 0);
        if (rc == 0) {
            idle = 0;
            uint8_t m[MAX_MSG];
            memcpy(m, rx.data, rx.len);
            if (rx.len > 1 && m[0] == 2 && data++ == r->flip) {
                m[rx.len / 2] ^= 0x01;
            }
            run = duct_send(up, m, rx.len, 5000) == 0;
            duct_rx_release(down, &rx);
        } else if (rc != -ETIMEDOUT) {
            run = 0;
        }
        rc = duct_recv(up, &rx, 0);
        if (rc == 0) {
            idle = 0;
            run = run && duct_send(down, rx.data, rx.len, 5000) == 0;
            duct_rx_release(up, &rx);
        } else if (rc != -ETIMEDOUT) {
            run = 0;
        }
        if (idle) {
            usleep(200);
        }
    }
    duct_ep_close(up);
    duct_ep_close(down);
    return NULL;
}

static void test_tamper(void) {
    duct_ctx *sc;
    duct_ctx *cc;
    duct_ctx *rc_ctx;
    EXPECT(make_ctx(1, cert_a, key_a, NULL, NULL, 3000, &sc) == 0, "server context");
    EXPECT(make_ctx(1, NULL, NULL, cert_a, NULL, 3000, &cc) == 0, "client context");
    EXPECT(make_ctx(0, NULL, NULL, NULL, NULL, 3000, &rc_ctx) == 0, "relay context");
    struct server s = {0};
    duct_listen(sc, g_addr, "0", &s.l);
    struct relay r = {.ctx = rc_ctx, .flip = 3};
    port_of(s.l, r.port, sizeof(r.port));
    duct_listen(rc_ctx, g_addr, "0", &r.l);
    char rport[16];
    port_of(r.l, rport, sizeof(rport));
    pthread_t st;
    pthread_t rt;
    pthread_create(&st, NULL, server_main, &s);
    pthread_create(&rt, NULL, relay_main, &r);
    duct_ep *ep;
    int rc = duct_connect(cc, g_addr, rport, &ep);
    EXPECT(rc == 0, "connect through the relay: %s", duct_strerror(rc));
    if (rc == 0) {
        int ok = 0;
        for (unsigned i = 0; i < 3; i++) {
            ok += echo(ep, 200, i) == 0;
        }
        EXPECT(ok == 3, "messages before the changed one are delivered");
        rc = echo(ep, 200, 9);
        EXPECT(rc != 0, "changed message not delivered");
        duct_ep_close(ep);
    }
    pthread_join(st, NULL);
    EXPECT(s.end_code == DUCT_EPROTO, "server reports the changed message: %s", duct_strerror(s.end_code));
    EXPECT(atomic_load(&s.echoed) == 3, "server echoed %lu messages", atomic_load(&s.echoed));
    pthread_join(rt, NULL);
    duct_listener_close(r.l);
    duct_listener_close(s.l);
    duct_ctx_destroy(rc_ctx);
    duct_ctx_destroy(cc);
    duct_ctx_destroy(sc);
}

/* A client that connects without encryption and sends nothing: the accepted encrypted endpoint fails
 * after connect_timeout_ms (300 ms) instead of waiting forever. The client connects from a second thread,
 * because the transport-level setup needs the accepting side. */
struct silent_client {
    duct_ctx *ctx;
    char port[16];
    duct_ep *ep;
    int rc;
};

static void *silent_main(void *arg) {
    struct silent_client *s = arg;
    s->rc = duct_connect(s->ctx, g_addr, s->port, &s->ep);
    return NULL;
}

static void test_handshake_timeout(void) {
    duct_ctx *sc;
    struct silent_client sl = {0};
    EXPECT(make_ctx(1, cert_a, key_a, NULL, NULL, 300, &sc) == 0, "server context");
    EXPECT(make_ctx(0, NULL, NULL, NULL, NULL, 3000, &sl.ctx) == 0, "client context");
    duct_listener *l;
    duct_listen(sc, g_addr, "0", &l);
    port_of(l, sl.port, sizeof(sl.port));
    pthread_t th;
    pthread_create(&th, NULL, silent_main, &sl);
    duct_ep *ep;
    int rc = duct_accept(l, 3000, &ep);
    int64_t t0 = duct_now_ms();
    pthread_join(th, NULL);
    EXPECT(rc == 0 && sl.rc == 0, "accept returns before the handshake: %s / %s", duct_strerror(rc),
           duct_strerror(sl.rc));
    if (rc == 0) {
        struct duct_rx rx;
        do {
            rc = duct_recv(ep, &rx, 50);
        } while (rc == -ETIMEDOUT && duct_now_ms() - t0 < 5000);
        int64_t took = duct_now_ms() - t0;
        EXPECT(rc == DUCT_EPROTO, "handshake timeout reported as %s", duct_strerror(rc));
        EXPECT(took >= 200 && took < 2000, "handshake failed after %lld ms", (long long)took);
        duct_ep_close(ep);
    }
    if (sl.rc == 0) {
        duct_ep_close(sl.ep);
    }
    duct_listener_close(l);
    duct_ctx_destroy(sl.ctx);
    duct_ctx_destroy(sc);
}

static void test_options(void) {
    duct_ctx *ctx;
    int rc = make_ctx(1, "/nonexistent/cert.pem", "/nonexistent/key.pem", NULL, NULL, 1000, &ctx);
    EXPECT(rc == -EINVAL, "missing certificate file: %s", duct_strerror(rc));
    rc = make_ctx(1, cert_a, key_b, NULL, NULL, 1000, &ctx);
    EXPECT(rc == -EINVAL, "key that does not match the certificate: %s", duct_strerror(rc));
    rc = make_ctx(1, NULL, NULL, "/nonexistent/ca.pem", NULL, 1000, &ctx);
    EXPECT(rc == -EINVAL, "missing CA file: %s", duct_strerror(rc));
    rc = make_ctx(1, NULL, NULL, NULL, NULL, 1000, &ctx);
    EXPECT(rc == 0, "client-only context: %s", duct_strerror(rc));
    if (rc == 0) {
        duct_listener *l;
        EXPECT(duct_listen(ctx, g_addr, "0", &l) == -EINVAL, "listening without a certificate");
        duct_ctx_destroy(ctx);
    }
}

/* Every case on the current transport. */
static void run_all(void) {
    int end;
    int rc;

    rc = run_pair(
        &(struct pair_opts){.server_tls = 1, .server_cert = cert_a, .client_tls = 1, .client_ca = cert_a},
        check_echo, &end);
    EXPECT(rc == 0, "verified by IP address: %s", duct_strerror(rc));
    EXPECT(end == DUCT_ECLOSED, "server end: %s", duct_strerror(end));

    rc = run_pair(
        &(struct pair_opts){
            .server_tls = 1, .server_cert = cert_a, .client_tls = 1, .client_ca = cert_a, .use_poll = 1},
        check_short, &end);
    EXPECT(rc == 0, "server waiting in poll(): %s", duct_strerror(rc));
    EXPECT(end == DUCT_ECLOSED, "server end: %s", duct_strerror(end));

    rc = run_pair(&(struct pair_opts){.server_tls = 1,
                                      .server_cert = cert_a,
                                      .client_tls = 1,
                                      .client_ca = cert_a,
                                      .name = "localhost"},
                  check_short, &end);
    EXPECT(rc == 0, "verified by DNS name: %s", duct_strerror(rc));

    rc = run_pair(&(struct pair_opts){.server_tls = 1, .server_cert = cert_a, .client_tls = 1}, check_rekey,
                  &end);
    EXPECT(rc == 0, "no verification, key updates: %s", duct_strerror(rc));
    EXPECT(end == DUCT_ECLOSED, "server end after key updates: %s", duct_strerror(end));

    rc = run_pair(
        &(struct pair_opts){.server_tls = 1, .server_cert = cert_a, .client_tls = 1, .client_ca = cert_b},
        NULL, &end);
    EXPECT(rc == DUCT_EPROTO, "wrong CA: %s", duct_strerror(rc));
    EXPECT(end != 0, "server end after a wrong CA: %s", duct_strerror(end));

    rc = run_pair(&(struct pair_opts){.server_tls = 1,
                                      .server_cert = cert_a,
                                      .client_tls = 1,
                                      .client_ca = cert_a,
                                      .name = "other.example"},
                  NULL, &end);
    EXPECT(rc == DUCT_EPROTO, "wrong name: %s", duct_strerror(rc));

    rc = run_pair(&(struct pair_opts){.server_tls = 1,
                                      .server_cert = cert_a,
                                      .server_ca = cert_b,
                                      .client_tls = 1,
                                      .client_ca = cert_a},
                  NULL, &end);
    EXPECT(rc != 0 || end != DUCT_ECLOSED, "client certificate required but none sent: %s / %s",
           duct_strerror(rc), duct_strerror(end));

    rc = run_pair(&(struct pair_opts){.server_tls = 1,
                                      .server_cert = cert_a,
                                      .server_ca = cert_b,
                                      .client_tls = 1,
                                      .client_cert = cert_b,
                                      .client_ca = cert_a},
                  check_short, &end);
    EXPECT(rc == 0 && end == DUCT_ECLOSED, "client certificate accepted: %s / %s", duct_strerror(rc),
           duct_strerror(end));

    rc = run_pair(&(struct pair_opts){.server_tls = 0, .client_tls = 1, .client_ca = cert_a}, NULL, &end);
    EXPECT(rc == DUCT_EPROTO, "encrypted client, unencrypted server: %s", duct_strerror(rc));

    rc = run_pair(&(struct pair_opts){.server_tls = 1, .server_cert = cert_a, .client_tls = 0}, check_plain,
                  &end);
    EXPECT(rc == 0, "unencrypted client connects at the transport level: %s", duct_strerror(rc));
    EXPECT(end == DUCT_EPROTO, "server end with an unencrypted client: %s", duct_strerror(end));

    test_tamper();
    test_handshake_timeout();
    test_options();
}

int main(void) {
    snprintf(cert_a, sizeof(cert_a), "/tmp/duct-tls-a-%d.crt", (int)getpid());
    snprintf(key_a, sizeof(key_a), "/tmp/duct-tls-a-%d.key", (int)getpid());
    snprintf(cert_b, sizeof(cert_b), "/tmp/duct-tls-b-%d.crt", (int)getpid());
    snprintf(key_b, sizeof(key_b), "/tmp/duct-tls-b-%d.key", (int)getpid());
    if (make_cert(cert_a, key_a) != 0 || make_cert(cert_b, key_b) != 0) {
        fprintf(stderr, "certificate generation failed\n");
        return 1;
    }
    run_all();
    printf("test_tls: TCP %s\n", failures ? "FAILED" : "ok");
#ifdef DUCT_HAVE_RDMA
    const char *rdma_addr = getenv("DUCT_TEST_ADDR");
    if (rdma_addr != NULL && *rdma_addr != '\0') {
        g_transport = DUCT_TRANSPORT_RDMA;
        g_addr = rdma_addr;
        g_wait = DUCT_WAIT_POLL;
        run_all();
        g_wait = DUCT_WAIT_EVENT;
        run_all();
        printf("test_tls: RDMA (%s, polling and event waiting) %s\n", rdma_addr, failures ? "FAILED" : "ok");
    } else {
        puts("test_tls: RDMA skipped (DUCT_TEST_ADDR not set)");
    }
#endif
    unlink(cert_a);
    unlink(key_a);
    unlink(cert_b);
    unlink(key_b);
    printf("test_tls: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

#endif
