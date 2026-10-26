/*
 * test_net.c - server integration over real connections: text protocols, persistence, limits, TLS and
 * the native protocol revision 2.
 *
 *   - memcached and RESP over TCP: 2000 pipelined commands in one write, values up to 512 KiB, replies
 *     of 64 MiB that the client reads only after sending everything (backpressure), 16 concurrent
 *     clients with self-describing values
 *   - request counters kept per worker: stats and INFO report the sums over all workers
 *   - connection limit and idle timeout, including activity just before a second boundary
 *   - snapshot: save at stop, load at start with flags and expiry, corrupt and truncated files refused,
 *     BGSAVE and LASTSAVE
 *   - TLS (build with TLS=1): RESP over TLS with a certificate that the test creates
 *   - native protocol revision 2 through the client library: store modes, CAS, metadata, increments,
 *     touch, flush, MGET, authentication, pipelining within the window, consistent hashing over servers,
 *     failover of a pool entry with a primary and a replica (reads from the replica, promotion, reconnect)
 *   - native batching: a window of 100 requests with values of 1 to 40,000 bytes, so that client staging
 *     is sent by count and by size, and server responses are batched, limited by send space and queued
 */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef AKV_HAVE_TLS
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#include "akvclient.h"
#include "akvd.h"
#include "srv.h"

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

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void base_config(akvd_config *cfg) {
    akvd_config_default(cfg);
    cfg->transport = DUCT_TRANSPORT_TCP;
    cfg->addr = "127.0.0.1";
    cfg->port = "0";
    cfg->text_addr = "127.0.0.1";
    cfg->memcached_port = "0";
    cfg->resp_port = "0";
    cfg->store.capacity = 64u << 20;
    cfg->store.shards = 8;
    cfg->store.max_entry = 1u << 20;
    cfg->threads = 2;
    cfg->log_level = AKVD_LOG_ERROR;
}

static int tcp_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {
        .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(0x7f000001)};
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int send_all(int fd, const void *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, (const char *)p + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* Reads exactly n bytes (timeout 10 s). Returns 0 or -1. */
static int recv_all(int fd, void *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, 10000) <= 0) {
            return -1;
        }
        ssize_t r = recv(fd, (char *)p + off, n - off, 0);
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

/* Reads until `want` bytes arrived or the peer closes; returns the count. */
static size_t recv_upto(int fd, char *p, size_t want, int timeout_ms) {
    size_t off = 0;
    while (off < want) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, timeout_ms) <= 0) {
            break;
        }
        ssize_t r = recv(fd, p + off, want - off, 0);
        if (r <= 0) {
            break;
        }
        off += (size_t)r;
    }
    return off;
}

/* Sends a request and compares the reply with want. */
static void exchange(int fd, const char *req, const char *want, const char *what) {
    size_t n = strlen(want);
    char *got = calloc(1, n + 1);
    int ok = send_all(fd, req, strlen(req)) == 0 && recv_all(fd, got, n) == 0 && memcmp(got, want, n) == 0;
    EXPECT(ok, "%s: got [%.*s] want [%s]", what, (int)n, got, want);
    free(got);
}

/* ---- text protocols --------------------------------------------------------------------------------- */

static void test_pipeline(akvd *d) {
    int fd = tcp_connect(akvd_memcached_port(d));
    EXPECT(fd >= 0, "memcached connect");
    /* 1000 sets and 1000 gets in one write */
    size_t cap = 1 << 20;
    char *req = malloc(cap);
    char *want = malloc(cap);
    size_t rn = 0;
    size_t wn = 0;
    for (int i = 0; i < 1000; i++) {
        rn += (size_t)snprintf(req + rn, cap - rn, "set p%d 0 0 %d\r\nv%04d\r\n", i, 5, i);
        wn += (size_t)snprintf(want + wn, cap - wn, "STORED\r\n");
    }
    for (int i = 0; i < 1000; i++) {
        rn += (size_t)snprintf(req + rn, cap - rn, "get p%d\r\n", i);
        wn += (size_t)snprintf(want + wn, cap - wn, "VALUE p%d 0 5\r\nv%04d\r\nEND\r\n", i, i);
    }
    char *got = malloc(wn);
    EXPECT(send_all(fd, req, rn) == 0 && recv_all(fd, got, wn) == 0 && memcmp(got, want, wn) == 0,
           "2000 pipelined memcached commands");
    free(got);
    free(req);
    free(want);

    /* a 512 KiB value */
    size_t big = 512u << 10;
    char *val = malloc(big + 64);
    for (size_t i = 0; i < big; i++) {
        val[i] = (char)('a' + i % 26);
    }
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "set big 7 0 %zu\r\n", big);
    send_all(fd, hdr, (size_t)hl);
    send_all(fd, val, big);
    exchange(fd, "\r\n", "STORED\r\n", "store 512 KiB");
    hl = snprintf(hdr, sizeof(hdr), "VALUE big 7 %zu\r\n", big);
    char *reply = malloc(big + 100);
    send_all(fd, "get big\r\n", 9);
    EXPECT(recv_all(fd, reply, (size_t)hl + big + 7) == 0 && memcmp(reply, hdr, (size_t)hl) == 0 &&
               memcmp(reply + hl, val, big) == 0 && memcmp(reply + hl + big, "\r\nEND\r\n", 7) == 0,
           "read 512 KiB");

    /* backpressure: 128 gets of 512 KiB sent before reading anything (64 MiB of replies) */
    int sent = 0;
    for (int i = 0; i < 128; i++) {
        sent += send_all(fd, "get big\r\n", 9) == 0;
    }
    int ok = 1;
    for (int i = 0; i < 128 && ok; i++) {
        ok = recv_all(fd, reply, (size_t)hl + big + 7) == 0 && memcmp(reply + hl, val, big) == 0;
    }
    EXPECT(sent == 128 && ok, "64 MiB of pipelined replies delivered in order");
    free(reply);
    free(val);
    close(fd);

    fd = tcp_connect(akvd_resp_port(d));
    EXPECT(fd >= 0, "resp connect");
    cap = 1 << 20;
    req = malloc(cap);
    want = malloc(cap);
    rn = wn = 0;
    for (int i = 0; i < 1000; i++) {
        rn +=
            (size_t)snprintf(req + rn, cap - rn, "*3\r\n$3\r\nSET\r\n$5\r\nr%04d\r\n$1\r\n%d\r\n", i, i % 10);
        wn += (size_t)snprintf(want + wn, cap - wn, "+OK\r\n");
    }
    for (int i = 0; i < 1000; i++) {
        rn += (size_t)snprintf(req + rn, cap - rn, "INCR r%04d\r\n", i);
        wn += (size_t)snprintf(want + wn, cap - wn, ":%d\r\n", i % 10 + 1);
    }
    got = malloc(wn);
    EXPECT(send_all(fd, req, rn) == 0 && recv_all(fd, got, wn) == 0 && memcmp(got, want, wn) == 0,
           "2000 pipelined RESP commands");
    free(got);
    free(req);
    free(want);
    close(fd);
}

struct hammer {
    int port;
    int resp;
    unsigned id;
    int bad;
};

/* Each client writes and reads its own keys with values that encode key and version. */
static void *hammer_main(void *arg) {
    struct hammer *h = arg;
    int fd = tcp_connect(h->port);
    if (fd < 0) {
        h->bad++;
        return NULL;
    }
    char req[256];
    char want[256];
    for (unsigned i = 0; i < 2000; i++) {
        unsigned k = (h->id * 7919 + i) % 300;
        int n;
        int wl;
        if (h->resp) {
            n = snprintf(req, sizeof(req), "SET h%u-%u v%u-%u\r\nGET h%u-%u\r\n", h->id, k, i, k, h->id, k);
            char v[64];
            int vl = snprintf(v, sizeof(v), "v%u-%u", i, k);
            wl = snprintf(want, sizeof(want), "+OK\r\n$%d\r\n%s\r\n", vl, v);
        } else {
            char v[64];
            int vl = snprintf(v, sizeof(v), "v%u-%u", i, k);
            n = snprintf(req, sizeof(req), "set h%u-%u 0 0 %d\r\n%s\r\nget h%u-%u\r\n", h->id, k, vl, v,
                         h->id, k);
            wl =
                snprintf(want, sizeof(want), "STORED\r\nVALUE h%u-%u 0 %d\r\n%s\r\nEND\r\n", h->id, k, vl, v);
        }
        char got[256];
        if (send_all(fd, req, (size_t)n) != 0 || recv_all(fd, got, (size_t)wl) != 0 ||
            memcmp(got, want, (size_t)wl)) {
            h->bad++;
            break;
        }
    }
    close(fd);
    return NULL;
}

static void test_concurrent(akvd *d) {
    enum { N = 16 };
    struct hammer h[N];
    pthread_t th[N];
    for (unsigned i = 0; i < N; i++) {
        h[i] = (struct hammer){
            .port = i % 2 ? akvd_resp_port(d) : akvd_memcached_port(d), .resp = i % 2, .id = i};
        pthread_create(&th[i], NULL, hammer_main, &h[i]);
    }
    int bad = 0;
    for (unsigned i = 0; i < N; i++) {
        pthread_join(th[i], NULL);
        bad += h[i].bad;
    }
    EXPECT(bad == 0, "%d of %d concurrent text clients saw a wrong reply", bad, N);
}

static void test_limits(void) {
    akvd_config cfg;
    base_config(&cfg);
    cfg.max_conns = 2;
    cfg.idle_timeout_s = 1;
    cfg.port = NULL;
    int err;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "limits server: %d", err);
    if (d == NULL) {
        return;
    }
    int a = tcp_connect(akvd_resp_port(d));
    int b = tcp_connect(akvd_resp_port(d));
    exchange(a, "PING\r\n", "+PONG\r\n", "first connection");
    exchange(b, "PING\r\n", "+PONG\r\n", "second connection");
    int c = tcp_connect(akvd_resp_port(d));
    char buf[16];
    send_all(c, "PING\r\n", 6);
    EXPECT(recv_upto(c, buf, sizeof(buf), 2000) == 0, "third connection closed by the limit");
    close(c);
    /* idle timeout: no traffic for about 2 s closes both */
    sleep_ms(2500);
    send_all(a, "PING\r\n", 6);
    EXPECT(recv_upto(a, buf, sizeof(buf), 2000) == 0, "idle connection closed");
    close(a);
    close(b);
    akvd_stop(d);
}

/*
 * A connection active just before a second boundary of the server's clock is not idle right after it. One
 * worker serves a and b; a sends a request shortly before a boundary, b one shortly after it, which wakes
 * the worker and runs its idle scan; a's next request must still be answered.
 */
static void test_idle_boundary(void) {
    akvd_config cfg;
    base_config(&cfg);
    cfg.threads = 1;
    cfg.idle_timeout_s = 1;
    cfg.port = NULL;
    akvd *d = akvd_start(&cfg, NULL);
    EXPECT(d != NULL, "idle boundary server");
    if (d == NULL) {
        return;
    }
    int a = tcp_connect(akvd_resp_port(d));
    int b = tcp_connect(akvd_resp_port(d));
    for (int i = 0; i < 3; i++) {
        struct timespec ts;
        do {
            sleep_ms(5);
            clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
        } while (ts.tv_nsec < 900000000L);
        exchange(a, "PING\r\n", "+PONG\r\n", "request before a second boundary");
        exchange(b, "PING\r\n", "+PONG\r\n", "other connection before the boundary");
        do {
            sleep_ms(5);
            clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
        } while (ts.tv_nsec >= 900000000L);
        sleep_ms(50);
        exchange(b, "PING\r\n", "+PONG\r\n", "other connection after the boundary");
        exchange(a, "PING\r\n", "+PONG\r\n", "request after the boundary within the idle timeout");
    }
    close(a);
    close(b);
    akvd_stop(d);
}

/* ---- persistence ------------------------------------------------------------------------------------ */

static void test_snapshot(void) {
    char path[] = "/tmp/akv-test-snap-XXXXXX";
    int tfd = mkstemp(path);
    close(tfd);
    unlink(path); /* start without a snapshot */
    akvd_config cfg;
    base_config(&cfg);
    cfg.port = NULL;
    cfg.memcached_port = NULL;
    cfg.snapshot_path = path;
    cfg.save_on_stop = 1;
    int err;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "snapshot server: %d", err);
    if (d == NULL) {
        return;
    }
    akv *s = akvd_store(d);
    char key[32];
    for (int i = 0; i < 5000; i++) {
        int n = snprintf(key, sizeof(key), "s%d", i);
        akv_store_opts o = {.flags = (uint32_t)i, .ttl_ms = i % 2 ? 600000 : 0};
        akv_store(s, key, (size_t)n, key, (size_t)n, &o, NULL);
    }
    akv_store_opts gone = {.ttl_ms = 1};
    akv_store(s, "short", 5, "x", 1, &gone, NULL);
    int fd = tcp_connect(akvd_resp_port(d));
    exchange(fd, "BGSAVE\r\n", "+Background saving started\r\n", "bgsave");
    for (int i = 0; i < 100 && akvd_lastsave(d) == 0; i++) {
        sleep_ms(20);
    }
    EXPECT(akvd_lastsave(d) != 0, "lastsave set after bgsave");
    close(fd);
    sleep_ms(5);
    akvd_stop(d); /* writes the snapshot again */

    d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "restart from snapshot: %d", err);
    if (d != NULL) {
        s = akvd_store(d);
        int ok = 1;
        for (int i = 0; i < 5000 && ok; i++) {
            int n = snprintf(key, sizeof(key), "s%d", i);
            char val[32];
            size_t vlen;
            akv_meta m;
            ok = akv_get_meta(s, key, (size_t)n, val, sizeof(val), &vlen, &m, AKV_TTL_KEEP) == 0 &&
                 vlen == (size_t)n && memcmp(val, key, vlen) == 0 && m.flags == (uint32_t)i &&
                 (i % 2 ? (m.ttl_ms > 500000 && m.ttl_ms <= 600000) : m.ttl_ms == -1);
        }
        EXPECT(ok, "entries, flags and expiry restored");
        EXPECT(!akv_exists(s, "short", 5), "expired entry not restored");
        akvd_stop(d);
    }

    /* Time while stopped counts towards expiry: a file created 700 s ago drops the 600 s entries. */
    FILE *f = fopen(path, "r+b");
    uint64_t past = (uint64_t)time(NULL) - 700;
    uint8_t le[8];
    for (int i = 0; i < 8; i++) {
        le[i] = (uint8_t)(past >> (8 * i));
    }
    fseek(f, 16, SEEK_SET);
    fwrite(le, 1, sizeof(le), f);
    fclose(f);
    d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "restart from an old snapshot: %d", err);
    if (d != NULL) {
        s = akvd_store(d);
        int kept = 0;
        int expired = 0;
        for (int i = 0; i < 5000; i++) {
            int n = snprintf(key, sizeof(key), "s%d", i);
            int present = akv_exists(s, key, (size_t)n);
            kept += i % 2 == 0 && present;
            expired += i % 2 == 1 && !present;
        }
        EXPECT(kept == 2500 && expired == 2500, "downtime counted: %d kept, %d expired", kept, expired);
        akvd_stop(d);
    }

    /* corruption: flip a byte inside the first block */
    f = fopen(path, "r+b");
    fseek(f, 100, SEEK_SET);
    int c = fgetc(f);
    fseek(f, 100, SEEK_SET);
    fputc(c ^ 0x55, f);
    fclose(f);
    d = akvd_start(&cfg, &err);
    EXPECT(d == NULL && err == -EBADMSG, "corrupt snapshot refused (err %d)", err);
    if (d != NULL) {
        akvd_stop(d);
    }
    /* truncation */
    f = fopen(path, "r+b");
    fseek(f, 100, SEEK_SET);
    fputc(c, f);
    fclose(f);
    if (truncate(path, 200) != 0) {
        EXPECT(0, "truncate");
    }
    d = akvd_start(&cfg, &err);
    EXPECT(d == NULL && err == -EBADMSG, "truncated snapshot refused (err %d)", err);
    unlink(path);
}

/* ---- TLS -------------------------------------------------------------------------------------------- */

#ifdef AKV_HAVE_TLS
/* Writes a self-signed certificate and its key for 127.0.0.1 (valid one day). */
static int make_cert(const char *cert_path, const char *key_path) {
    EVP_PKEY *pk = EVP_RSA_gen(2048);
    X509 *x = X509_new();
    if (pk == NULL || x == NULL) {
        return -1;
    }
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 86400);
    X509_set_pubkey(x, pk);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"127.0.0.1", -1, -1, 0);
    X509_set_issuer_name(x, name);
    /* Address verification (RFC 6125, section 6.2) matches subjectAltName IP entries, not the common name. */
    X509_set_version(x, 2);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, NULL, NULL, 0);
    X509_EXTENSION *san = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, "IP:127.0.0.1");
    if (san != NULL) {
        X509_add_ext(x, san, -1);
        X509_EXTENSION_free(san);
    }
    X509_sign(x, pk, EVP_sha256());
    FILE *cf = fopen(cert_path, "w");
    FILE *kf = fopen(key_path, "w");
    int ok = cf != NULL && kf != NULL && PEM_write_X509(cf, x) == 1 &&
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

static void test_tls(void) {
    char cert[] = "/tmp/akv-test-cert-XXXXXX";
    char key[] = "/tmp/akv-test-key-XXXXXX";
    close(mkstemp(cert));
    close(mkstemp(key));
    EXPECT(make_cert(cert, key) == 0, "certificate generation");
    akvd_config cfg;
    base_config(&cfg);
    cfg.port = NULL;
    cfg.tls_resp_port = "0";
    cfg.tls_cert = cert;
    cfg.tls_key = key;
    int err;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "TLS server: %d", err);
    if (d != NULL) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_load_verify_locations(ctx, cert, NULL);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        int fd = tcp_connect(akvd_tls_resp_port(d));
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        int hs = SSL_connect(ssl);
        EXPECT(hs == 1, "TLS handshake with certificate verification");
        if (hs == 1) {
            size_t big = 300000;
            char *val = malloc(big);
            memset(val, 'q', big);
            char hdr[64];
            int hl = snprintf(hdr, sizeof(hdr), "*3\r\n$3\r\nSET\r\n$1\r\nt\r\n$%zu\r\n", big);
            SSL_write(ssl, hdr, hl);
            SSL_write(ssl, val, (int)big);
            SSL_write(ssl, "\r\nGET t\r\n", 9);
            size_t want = 5 + (size_t)snprintf(hdr, sizeof(hdr), "$%zu\r\n", big) + big + 2;
            char *got = malloc(want);
            size_t off = 0;
            while (off < want) {
                int r = SSL_read(ssl, got + off, (int)(want - off));
                if (r <= 0) {
                    break;
                }
                off += (size_t)r;
            }
            EXPECT(off == want && memcmp(got, "+OK\r\n", 5) == 0 &&
                       memcmp(got + want - 2 - big, val, big) == 0,
                   "300 KB value over TLS (%zu of %zu bytes)", off, want);
            free(got);
            free(val);
            SSL_shutdown(ssl);
        }
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        akvd_stop(d);
    }
    unlink(cert);
    unlink(key);
}
#endif

/* ---- native protocol revision 2 ---------------------------------------------------------------------- */

static struct duct_opts client_opts(void) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = DUCT_TRANSPORT_TCP;
    o.max_msg = 64u * 1024u;
    o.recv_depth = 16;
    o.send_depth = 16;
    return o;
}

static akvc *native_connect(akvd *d) {
    struct duct_opts o = client_opts();
    char port[16];
    snprintf(port, sizeof(port), "%d", akvd_port(d));
    akvc *c = NULL;
    EXPECT(akvc_connect(&c, &o, "127.0.0.1", port) == 0, "native connect");
    return c;
}

static void test_native(akvd *d) {
    akvc *c = native_connect(d);
    if (c == NULL) {
        return;
    }
    char buf[256];
    size_t n;
    uint64_t cas1;
    uint64_t cas2;
    struct akvp_ext x = {.uflags = 42, .ttl_ms = 60000};
    EXPECT(akvc_store(c, AKVP_ADD, "k", 1, "one", 3, &x, &cas1) == AKVP_OK && cas1 != 0,
           "add with flags and ttl");
    EXPECT(akvc_store(c, AKVP_ADD, "k", 1, "two", 3, NULL, NULL) == AKVP_EXISTS, "add existing");
    EXPECT(akvc_store(c, AKVP_REPLACE, "none", 4, "x", 1, NULL, NULL) == AKVP_NOTFOUND, "replace absent");
    uint32_t flags;
    uint64_t cas;
    int64_t ttl;
    EXPECT(akvc_get_meta(c, "k", 1, buf, sizeof(buf), &n, &flags, &cas, &ttl, INT64_MIN) == AKVP_OK &&
               n == 3 && flags == 42 && cas == cas1 && ttl > 0 && ttl <= 60000,
           "metadata");
    struct akvp_ext bad = {.cas = cas1 + 1};
    EXPECT(akvc_store(c, AKVP_CAS, "k", 1, "zz", 2, &bad, NULL) == AKVP_EXISTS, "cas mismatch");
    struct akvp_ext good = {.cas = cas1};
    EXPECT(akvc_store(c, AKVP_CAS, "k", 1, "zz", 2, &good, &cas2) == AKVP_OK && cas2 != cas1, "cas match");
    EXPECT(akvc_store(c, AKVP_APPEND, "k", 1, "-a", 2, NULL, NULL) == AKVP_OK &&
               akvc_get(c, "k", 1, buf, sizeof(buf), &n) == AKVP_OK && n == 4 && memcmp(buf, "zz-a", 4) == 0,
           "append");
    int64_t r;
    akvc_put(c, "n", 1, "7", 1);
    EXPECT(akvc_incr(c, "n", 1, 5, 0, &r) == AKVP_OK && r == 12, "incr");
    EXPECT(akvc_incr(c, "k", 1, 1, 0, &r) == AKVP_NOTNUM, "incr non-numeric");
    EXPECT(akvc_incr(c, "new", 3, -2, 1, &r) == AKVP_OK && r == -2, "signed incr creates");
    EXPECT(akvc_touch(c, "n", 1, 1) == AKVP_OK, "touch");
    sleep_ms(20);
    EXPECT(akvc_exists(c, "n", 1) == AKVP_NOTFOUND && akvc_exists(c, "k", 1) == AKVP_OK,
           "exists after expiry");

    /* MGET through a raw request: keys k, missing, new */
    uint8_t req[128];
    struct akvp_req h = {.op = AKVP_MGET, .reqid = 777, .vlen = 2 + 1 + 2 + 7 + 2 + 3};
    akvp_req_encode(req, &h);
    uint8_t *p = req + AKVP_REQ_HDR;
    const char *keys[3] = {"k", "missing", "new"};
    for (int i = 0; i < 3; i++) {
        size_t kl = strlen(keys[i]);
        p[0] = (uint8_t)kl;
        p[1] = 0;
        memcpy(p + 2, keys[i], kl);
        p += 2 + kl;
    }
    const uint8_t *resp;
    size_t rlen;
    int rc = akvc_raw(c, req, (size_t)(p - req), &resp, &rlen);
    struct akvp_resp rs = {0};
    EXPECT(rc == 0 && akvp_resp_decode(resp, rlen, &rs) == 0 && rs.status == AKVP_OK && rs.reqid == 777,
           "mget response");
    if (rc == 0 && rs.status == AKVP_OK) {
        const uint8_t *q = resp + AKVP_RESP_HDR;
        EXPECT(q[0] == AKVP_OK && akvp_get32(q + 4) == 4 && memcmp(q + 8, "zz-a", 4) == 0, "mget first");
        q += 12;
        EXPECT(q[0] == AKVP_NOTFOUND && akvp_get32(q + 4) == 0, "mget missing");
        q += 8;
        EXPECT(q[0] == AKVP_OK && akvp_get32(q + 4) == 2 && memcmp(q + 8, "-2", 2) == 0, "mget third");
    }

    /* pipelining: a full window of puts, then gets, collected in order */
    unsigned win = akvc_window(c);
    EXPECT(win >= 8, "window %u", win);
    for (unsigned round = 0; round < 50; round++) {
        uint32_t ids[64];
        for (unsigned i = 0; i < win; i++) {
            char k[16];
            int kl = snprintf(k, sizeof(k), "pp%u", i);
            EXPECT(akvc_post(c, AKVP_PUT, k, (size_t)kl, &round, sizeof(round), NULL, &ids[i]) == 0,
                   "post %u", i);
        }
        EXPECT(akvc_post(c, AKVP_NOOP, NULL, 0, NULL, 0, NULL, NULL) == -EBUSY, "window enforced");
        for (unsigned i = 0; i < win; i++) {
            struct akvc_resp rr;
            EXPECT(akvc_next(c, &rr, 5000) == 0 && rr.reqid == ids[i] && rr.status == AKVP_OK,
                   "response %u in order", i);
        }
    }
    unsigned got_round = 0;
    EXPECT(akvc_get(c, "pp3", 3, &got_round, sizeof(got_round), &n) == AKVP_OK && got_round == 49,
           "pipelined data");
    EXPECT(akvc_flush(c) == AKVP_OK && akvc_exists(c, "k", 1) == AKVP_NOTFOUND, "flush");
    uint64_t st[AKVP_STATS_FIELDS];
    EXPECT(akvc_stats(c, st) == AKVP_OK && st[15] >= 1 && st[17] > 0, "stats with server counters");
    akvc_close(c);
}

/* Value of request i in round r: length and bytes derived from (r, i). */
static size_t batch_value(unsigned r, unsigned i, uint8_t *out) {
    size_t len = i % 5 == 0 ? 40000 : 1 + (i * 37 + r) % 200;
    for (size_t k = 0; k < len; k++) {
        out[k] = (uint8_t)(r * 131 + i * 7 + k);
    }
    return len;
}

static void test_native_batches(akvd *d) {
    struct duct_opts o = client_opts();
    o.recv_depth = 101;
    o.send_depth = 101;
    char port[16];
    snprintf(port, sizeof(port), "%d", akvd_port(d));
    akvc *c = NULL;
    EXPECT(akvc_connect(&c, &o, "127.0.0.1", port) == 0, "batch connect");
    if (c == NULL) {
        return;
    }
    unsigned win = akvc_window(c);
    EXPECT(win == 100, "window %u", win);
    static uint8_t val[40000];
    static uint8_t want[40000];
    for (unsigned r = 0; r < 3; r++) {
        uint32_t ids[100];
        for (unsigned i = 0; i < win; i++) {
            char k[16];
            int kl = snprintf(k, sizeof(k), "nb%u", i);
            size_t vl = batch_value(r, i, val);
            EXPECT(akvc_post(c, AKVP_PUT, k, (size_t)kl, val, vl, NULL, &ids[i]) == 0, "batch put %u", i);
        }
        for (unsigned i = 0; i < win; i++) {
            struct akvc_resp rr;
            EXPECT(akvc_next(c, &rr, 5000) == 0 && rr.reqid == ids[i] && rr.status == AKVP_OK, "put reply %u",
                   i);
        }
        for (unsigned i = 0; i < win; i++) {
            char k[16];
            int kl = snprintf(k, sizeof(k), "nb%u", i);
            EXPECT(akvc_post(c, AKVP_GET, k, (size_t)kl, NULL, 0, NULL, &ids[i]) == 0, "batch get %u", i);
        }
        EXPECT(akvc_push(c) == 0, "push");
        for (unsigned i = 0; i < win; i++) {
            struct akvc_resp rr;
            size_t wl = batch_value(r, i, want);
            int ok = akvc_next(c, &rr, 5000) == 0 && rr.reqid == ids[i] && rr.status == AKVP_OK &&
                     rr.len == wl && memcmp(rr.payload, want, wl) == 0;
            EXPECT(ok, "get reply %u of round %u", i, r);
        }
    }
    akvc_close(c);
}

static void test_native_auth(void) {
    char path[] = "/tmp/akv-test-cred-XXXXXX";
    int fd = mkstemp(path);
    if (write(fd, "bob:pw\n", 7) != 7) {
        EXPECT(0, "write credentials");
    }
    close(fd);
    akvd_config cfg;
    base_config(&cfg);
    cfg.memcached_port = NULL;
    cfg.resp_port = NULL;
    cfg.auth_file = path;
    int err;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "auth server: %d", err);
    if (d != NULL) {
        akvc *c = native_connect(d);
        if (c != NULL) {
            EXPECT(akvc_put(c, "a", 1, "b", 1) == AKVP_NOAUTH, "unauthenticated put");
            EXPECT(akvc_auth(c, "bob:nope") == AKVP_NOAUTH, "wrong password");
            EXPECT(akvc_auth(c, "bob:pw") == AKVP_OK && akvc_put(c, "a", 1, "b", 1) == AKVP_OK,
                   "authenticated");
            akvc_close(c);
        }
        akvd_stop(d);
    }
    unlink(path);
}

static void test_pool(void) {
    enum { S = 4 };
    akvd *d[S];
    char names[S][32];
    const char *servers[S];
    for (int i = 0; i < S; i++) {
        akvd_config cfg;
        base_config(&cfg);
        cfg.memcached_port = NULL;
        cfg.resp_port = NULL;
        cfg.threads = 1;
        int err;
        d[i] = akvd_start(&cfg, &err);
        EXPECT(d[i] != NULL, "pool server %d", i);
        if (d[i] == NULL) {
            return;
        }
        snprintf(names[i], sizeof(names[i]), "127.0.0.1:%d", akvd_port(d[i]));
        servers[i] = names[i];
    }
    struct duct_opts o = client_opts();
    akvc_pool *p4;
    akvc_pool *p3;
    EXPECT(akvc_pool_create(&p4, &o, servers, 4) == 0 && akvc_pool_create(&p3, &o, servers, 3) == 0, "pools");
    unsigned count[S] = {0};
    unsigned moved = 0;
    for (unsigned i = 0; i < 20000; i++) {
        char k[16];
        int kl = snprintf(k, sizeof(k), "key%u", i);
        unsigned a = akvc_pool_index(p4, k, (size_t)kl);
        unsigned b = akvc_pool_index(p3, k, (size_t)kl);
        count[a]++;
        /* keys of the removed server move; all others keep their server */
        if (a != 3) {
            moved += a != b;
        }
        if (i < 200) {
            akvc_put(akvc_pool_pick(p4, k, (size_t)kl), k, (size_t)kl, k, (size_t)kl);
        }
    }
    for (int i = 0; i < S; i++) {
        EXPECT(count[i] > 3500 && count[i] < 6500, "server %d owns %u of 20000 keys", i, count[i]);
    }
    EXPECT(moved == 0, "%u keys moved between remaining servers when one was removed", moved);
    char v[16];
    size_t n;
    EXPECT(akvc_get(akvc_pool_pick(p4, "key7", 4), "key7", 4, v, sizeof(v), &n) == AKVP_OK && n == 4,
           "key read back from its server");
    akvc_pool_close(p4);
    akvc_pool_close(p3);
    for (int i = 0; i < S; i++) {
        akvd_stop(d[i]);
    }
}

/* Polls a read until it returns want (replication lag), for up to 5 s. */
static int read_until(akvc *c, const char *key, const char *want) {
    for (int i = 0; i < 500; i++) {
        char v[64];
        size_t n = 0;
        if (akvc_get(c, key, strlen(key), v, sizeof(v), &n) == AKVP_OK && n == strlen(want) &&
            memcmp(v, want, n) == 0) {
            return 1;
        }
        struct timespec ts = {.tv_nsec = 10000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void test_pool_failover(void) {
    akvd_config cfg;
    base_config(&cfg);
    cfg.memcached_port = NULL;
    cfg.resp_port = NULL;
    int err;
    akvd *primary = akvd_start(&cfg, &err);
    EXPECT(primary != NULL, "primary: %d", err);
    if (primary == NULL) {
        return;
    }
    char pport[16];
    snprintf(pport, sizeof(pport), "%d", akvd_port(primary));
    cfg.replicaof_host = "127.0.0.1";
    cfg.replicaof_port = pport;
    akvd *replica = akvd_start(&cfg, &err);
    EXPECT(replica != NULL, "replica: %d", err);
    if (replica == NULL) {
        akvd_stop(primary);
        return;
    }
    char entry[64];
    snprintf(entry, sizeof(entry), "127.0.0.1:%d,127.0.0.1:%d", akvd_port(primary), akvd_port(replica));
    const char *servers[1] = {entry};
    struct duct_opts o = client_opts();
    akvc_pool *p;
    EXPECT(akvc_pool_create(&p, &o, servers, 1) == 0, "pool with a primary and a replica");
    int replica_role = -1;
    EXPECT(akvc_role(akvc_pool_pick(p, "k", 1), &replica_role) == AKVP_OK && replica_role == 0,
           "writes go to the primary");
    EXPECT(akvc_put(akvc_pool_pick(p, "k", 1), "k", 1, "v1", 2) == AKVP_OK, "write before the failure");
    akvc *reader = akvc_pool_pick_read(p, "k", 1);
    EXPECT(reader != akvc_pool_pick(p, "k", 1) && akvc_role(reader, &replica_role) == AKVP_OK &&
               replica_role == 1,
           "reads may go to the replica");
    EXPECT(read_until(reader, "k", "v1"), "the replica serves the write");
    EXPECT(akvc_put(reader, "x", 1, "v", 1) == AKVP_READONLY, "the replica refuses writes");

    akvd_stop(primary);
    EXPECT(akvc_put(akvc_pool_pick(p, "k", 1), "k", 1, "v2", 2) < 0, "the old primary is gone");
    EXPECT(akvc_pool_reconnect(p, "k", 1) == -ENOTCONN, "no primary before the promotion");
    repl_follow(akvd_repl(replica), NULL, NULL);
    EXPECT(akvc_pool_reconnect(p, "k", 1) == 0, "the promoted replica is found");
    EXPECT(akvc_put(akvc_pool_pick(p, "k", 1), "k", 1, "v2", 2) == AKVP_OK,
           "writes continue after the failover");
    EXPECT(read_until(akvc_pool_pick(p, "k", 1), "k", "v2"), "read after the failover");
    akvc_pool_close(p);
    akvd_stop(replica);
}

#ifdef AKV_HAVE_TLS
/*
 * Encrypted native listener and replication link: an encrypted client verified against the certificate, an
 * unencrypted client refused, and a replica that verifies its primary and requires client certificates.
 */
static void test_native_tls(void) {
    char cert[] = "/tmp/akv-test-ncert-XXXXXX";
    char key[] = "/tmp/akv-test-nkey-XXXXXX";
    close(mkstemp(cert));
    close(mkstemp(key));
    EXPECT(make_cert(cert, key) == 0, "certificate generation");
    akvd_config cfg;
    base_config(&cfg);
    cfg.memcached_port = NULL;
    cfg.resp_port = NULL;
    cfg.native_tls = 1;
    cfg.tls_cert = cert;
    cfg.tls_key = key;
    int err;
    akvd *primary = akvd_start(&cfg, &err);
    EXPECT(primary != NULL, "encrypted primary: %d", err);
    if (primary == NULL) {
        return;
    }
    char pport[16];
    snprintf(pport, sizeof(pport), "%d", akvd_port(primary));
    cfg.replicaof_host = "127.0.0.1";
    cfg.replicaof_port = pport;
    cfg.tls_ca = cert;
    akvd *replica = akvd_start(&cfg, &err);
    EXPECT(replica != NULL, "encrypted replica: %d", err);

    struct duct_opts o = client_opts();
    o.tls = 1;
    o.tls_ca = cert;
    akvc *c = NULL;
    EXPECT(akvc_connect(&c, &o, "127.0.0.1", pport) == 0, "encrypted client verifies the primary");
    if (c != NULL) {
        char big[60000];
        memset(big, 'b', sizeof(big));
        EXPECT(akvc_put(c, "k", 1, "secret", 6) == AKVP_OK, "write over the encrypted endpoint");
        EXPECT(akvc_put(c, "big", 3, big, sizeof(big)) == AKVP_OK, "60000-byte value");
        EXPECT(read_until(c, "k", "secret"), "read over the encrypted endpoint");
        akvc_close(c);
    }
    struct duct_opts plain = client_opts();
    akvc *p = NULL;
    if (akvc_connect(&p, &plain, "127.0.0.1", pport) == 0) {
        EXPECT(akvc_put(p, "x", 1, "v", 1) < 0, "unencrypted client refused");
        akvc_close(p);
    }
    if (replica != NULL) {
        char rport[16];
        snprintf(rport, sizeof(rport), "%d", akvd_port(replica));
        akvc *r = NULL;
        if (akvc_connect(&r, &o, "127.0.0.1", rport) == 0) {
            EXPECT(akvc_put(r, "x", 1, "v", 1) < 0, "replica refuses a client without a certificate");
            akvc_close(r);
        }
        o.tls_cert = cert;
        o.tls_key = key;
        r = NULL;
        EXPECT(akvc_connect(&r, &o, "127.0.0.1", rport) == 0, "client certificate accepted by the replica");
        if (r != NULL) {
            EXPECT(read_until(r, "k", "secret"), "the replica received the write over the encrypted link");
            akvc_close(r);
        }
        akvd_stop(replica);
    }
    akvd_stop(primary);
    unlink(cert);
    unlink(key);
}
#endif

/* Value of "STAT <name> <n>" in a memcached stats reply, or -1. */
static long long stat_value(const char *reply, const char *name) {
    char pat[64];
    snprintf(pat, sizeof(pat), "STAT %s ", name);
    const char *p = strstr(reply, pat);
    return p != NULL ? strtoll(p + strlen(pat), NULL, 10) : -1;
}

/*
 * Request counters are kept per worker thread: 8 connections spread over 4 workers each send 50 hits and
 * 50 misses; the memcached stats and the RESP INFO reply must report the sums of all workers.
 */
static void test_counters(void) {
    akvd_config cfg;
    base_config(&cfg);
    cfg.threads = 4;
    akvd *d = akvd_start(&cfg, NULL);
    EXPECT(d != NULL, "counter server");
    if (d == NULL) {
        return;
    }
    enum { CONNS = 8, EACH = 50 };
    int fd0 = tcp_connect(akvd_memcached_port(d));
    exchange(fd0, "set c-hit 0 0 1\r\nv\r\n", "STORED\r\n", "counter set");
    int fds[CONNS];
    static char req[CONNS * EACH * 32];
    size_t rl = 0;
    for (int i = 0; i < EACH; i++) {
        rl += (size_t)snprintf(req + rl, sizeof(req) - rl, "get c-hit\r\nget c-miss\r\n");
    }
    for (int c = 0; c < CONNS; c++) {
        fds[c] = tcp_connect(akvd_memcached_port(d));
        EXPECT(fds[c] >= 0 && send_all(fds[c], req, rl) == 0, "counter connection %d", c);
    }
    static char buf[1 << 16];
    for (int c = 0; c < CONNS; c++) {
        size_t want = (size_t)EACH * (strlen("VALUE c-hit 0 1\r\nv\r\nEND\r\n") + strlen("END\r\n"));
        EXPECT(recv_all(fds[c], buf, want) == 0, "counter replies %d", c);
        close(fds[c]);
    }
    char stats[4096];
    size_t n = 0;
    EXPECT(send_all(fd0, "stats\r\n", 7) == 0, "stats request");
    while (n < sizeof(stats) - 1 && (n < 5 || memcmp(stats + n - 5, "END\r\n", 5) != 0)) {
        size_t r = recv_upto(fd0, stats + n, 1, 5000);
        if (r == 0) {
            break;
        }
        n += r;
    }
    stats[n] = '\0';
    long long gets = (long long)CONNS * EACH * 2;
    EXPECT(stat_value(stats, "cmd_get") == gets && stat_value(stats, "get_hits") == gets / 2 &&
               stat_value(stats, "get_misses") == gets / 2,
           "stats over 4 workers: cmd_get %lld get_hits %lld get_misses %lld, want %lld/%lld/%lld",
           stat_value(stats, "cmd_get"), stat_value(stats, "get_hits"), stat_value(stats, "get_misses"), gets,
           gets / 2, gets / 2);
    close(fd0);
    int rf = tcp_connect(akvd_resp_port(d));
    char info[8192];
    EXPECT(send_all(rf, "INFO\r\n", 6) == 0, "info request");
    size_t in = recv_upto(rf, info, sizeof(info) - 1, 300);
    info[in] = '\0';
    const char *h = strstr(info, "keyspace_hits:");
    EXPECT(h != NULL && strtoll(h + 14, NULL, 10) == gets / 2, "INFO keyspace_hits: %.40s", h ? h : "absent");
    close(rf);
    akvd_stop(d);
}

int main(void) {
    akvd_config cfg;
    base_config(&cfg);
    int err;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "server: %d", err);
    if (d != NULL) {
        test_pipeline(d);
        test_concurrent(d);
        test_native(d);
        test_native_batches(d);
        akvd_stop(d);
    }
    test_counters();
    test_limits();
    test_idle_boundary();
    test_snapshot();
    test_native_auth();
    test_pool();
    test_pool_failover();
#ifdef AKV_HAVE_TLS
    test_tls();
    test_native_tls();
#else
    puts("test_net: TLS cases skipped (build with TLS=1)");
#endif
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_net: ok");
    return 0;
}
