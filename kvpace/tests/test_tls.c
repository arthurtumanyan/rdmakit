/*
 * test_tls.c - TLS for the text drivers (build with TLS=1).
 *
 * An in-process server speaks RESP (SET, GET) over TLS with a self-signed certificate that the test
 * creates for 127.0.0.1. The RESP driver connects with certificate verification against that certificate,
 * runs synchronous requests and 200 pipelined asynchronous requests; a connection that trusts another CA
 * is refused, and --tls-skip-verify accepts the server anyway.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PACE_HAVE_TLS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "driver.h"
#include "sock.h"

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

static int make_cert(const char *cert_path, const char *key_path, const char *cn) {
    EVP_PKEY *pk = EVP_RSA_gen(2048);
    X509 *x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 86400);
    X509_set_pubkey(x, pk);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1, 0);
    X509_set_issuer_name(x, name);
    /* subjectAltName IP:127.0.0.1, which verification of an IP address requires */
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, "IP:127.0.0.1");
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
    X509_sign(x, pk, EVP_sha256());
    FILE *cf = fopen(cert_path, "w");
    FILE *kf = fopen(key_path, "w");
    int ok = cf && kf && PEM_write_X509(cf, x) == 1 &&
             PEM_write_PrivateKey(kf, pk, NULL, NULL, 0, NULL, NULL) == 1;
    if (cf) {
        fclose(cf);
    }
    if (kf) {
        fclose(kf);
    }
    X509_free(x);
    EVP_PKEY_free(pk);
    return ok ? 0 : -1;
}

struct server {
    int ls;
    SSL_CTX *ctx;
    int conns;
};

/* Reads one RESP array of bulk strings from ssl into args (at most 3). Returns the count or -1. */
static int read_req(SSL *ssl, char **args, size_t *lens) {
    char line[64];
    int argc = 0;
    for (int part = -1; part < 0 || part < argc; part++) {
        size_t n = 0;
        char c;
        while (SSL_read(ssl, &c, 1) == 1 && c != '\n' && n + 1 < sizeof(line)) {
            line[n++] = c;
        }
        if (n == 0) {
            return -1;
        }
        line[n] = '\0';
        if (part < 0) {
            argc = atoi(line + 1);
            if (argc < 1 || argc > 3) {
                return -1;
            }
            continue;
        }
        size_t len = (size_t)atol(line + 1);
        args[part] = malloc(len + 2);
        lens[part] = len;
        size_t got = 0;
        while (got < len + 2) {
            int r = SSL_read(ssl, args[part] + got, (int)(len + 2 - got));
            if (r <= 0) {
                return -1;
            }
            got += (size_t)r;
        }
    }
    return argc;
}

static void *server_main(void *arg) {
    struct server *s = arg;
    char *store = NULL;
    size_t store_len = 0;
    for (int conn = 0; conn < s->conns; conn++) {
        int fd = accept(s->ls, NULL, NULL);
        SSL *ssl = SSL_new(s->ctx);
        SSL_set_fd(ssl, fd);
        int acc = SSL_accept(ssl);
        if (acc != 1) {
            ERR_clear_error(); /* the connection that trusts another CA aborts the handshake */
        }
        if (acc == 1) {
            char *args[3];
            size_t lens[3];
            int argc;
            while ((argc = read_req(ssl, args, lens)) > 0) {
                if (argc == 3 && memcmp(args[0], "SET", 3) == 0) {
                    free(store);
                    store = args[2];
                    store_len = lens[2];
                    SSL_write(ssl, "+OK\r\n", 5);
                    free(args[1]);
                } else if (argc == 2 && store != NULL) {
                    char head[32];
                    int n = snprintf(head, sizeof(head), "$%zu\r\n", store_len);
                    SSL_write(ssl, head, n);
                    SSL_write(ssl, store, (int)store_len);
                    SSL_write(ssl, "\r\n", 2);
                    free(args[1]);
                } else {
                    SSL_write(ssl, "$-1\r\n", 5);
                    for (int i = 1; i < argc; i++) {
                        free(args[i]);
                    }
                }
                free(args[0]);
            }
        }
        SSL_free(ssl);
        close(fd);
    }
    free(store);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN); /* OpenSSL writes with write(2); a closed peer must not end the test */
    char cert[] = "/tmp/pace-tls-cert-XXXXXX";
    char key[] = "/tmp/pace-tls-key-XXXXXX";
    char other[] = "/tmp/pace-tls-other-XXXXXX";
    char okey[] = "/tmp/pace-tls-okey-XXXXXX";
    close(mkstemp(cert));
    close(mkstemp(key));
    close(mkstemp(other));
    close(mkstemp(okey));
    EXPECT(make_cert(cert, key, "127.0.0.1") == 0 && make_cert(other, okey, "someone-else") == 0,
           "certificates");

    struct server s = {.conns = 4};
    s.ctx = SSL_CTX_new(TLS_server_method());
    EXPECT(SSL_CTX_use_certificate_file(s.ctx, cert, SSL_FILETYPE_PEM) == 1 &&
               SSL_CTX_use_PrivateKey_file(s.ctx, key, SSL_FILETYPE_PEM) == 1,
           "server certificate and key");
    s.ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(s.ls, (struct sockaddr *)&sa, sizeof(sa));
    listen(s.ls, 8);
    socklen_t sl = sizeof(sa);
    getsockname(s.ls, (struct sockaddr *)&sa, &sl);
    char port[16];
    snprintf(port, sizeof(port), "%u", ntohs(sa.sin_port));
    pthread_t th;
    pthread_create(&th, NULL, server_main, &s);

    const pace_driver *d = pace_driver_find("resp");
    /* 1: verification against the right certificate, synchronous and pipelined */
    pace_tls_cfg t = {.enabled = 1, .cacert = cert};
    EXPECT(pace_tls_setup(&t) == 0, "tls setup");
    pace_conn *c = d->connect("127.0.0.1", port);
    EXPECT(c != NULL, "verified TLS connection");
    if (c != NULL) {
        char buf[64];
        size_t n;
        int prc = d->put(c, "k", 1, "secret-value", 12);
        int grc = d->get(c, "k", 1, buf, sizeof(buf), &n);
        EXPECT(prc == PACE_OK && grc == PACE_OK && n == 12 && memcmp(buf, "secret-value", 12) == 0,
               "round trip over TLS (put %d, get %d, length %zu)", prc, grc, n);
        d->close(c);
    }
    pace_aconn *a = d->async->connect("127.0.0.1", port);
    EXPECT(a != NULL, "asynchronous TLS connection");
    if (a != NULL) {
        int epfd = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN};
        epoll_ctl(epfd, EPOLL_CTL_ADD, d->async->fd(a), &ev);
        for (int i = 0; i < 200; i++) {
            pace_req r = {
                .type = i % 2 ? PACE_OP_GET : PACE_OP_SET, .key = "k", .klen = 1, .val = "v1", .vlen = 2};
            d->async->submit(a, &r);
        }
        int got = 0;
        int ok = 1;
        for (int round = 0; round < 1000 && got < 200 && ok; round++) {
            ok = d->async->io(a) == PACE_OK;
            pace_reply rep;
            while (ok && d->async->next(a, &rep) == 1) {
                ok = rep.status == PACE_OK;
                got++;
            }
            if (got < 200 && !d->async->arm(a)) {
                struct epoll_event e;
                epoll_wait(epfd, &e, 1, 100);
            }
        }
        EXPECT(got == 200 && ok, "200 pipelined requests over TLS (%d)", got);
        close(epfd);
        d->async->close(a);
    }
    /* 2: trusting another CA: refused */
    pace_tls_cfg wrong = {.enabled = 1, .cacert = other};
    pace_tls_setup(&wrong);
    c = d->connect("127.0.0.1", port);
    EXPECT(c == NULL, "untrusted certificate refused");
    if (c != NULL) {
        d->close(c);
    }
    /* 3: skip verification: accepted */
    pace_tls_cfg skip = {.enabled = 1, .skip_verify = 1};
    pace_tls_setup(&skip);
    c = d->connect("127.0.0.1", port);
    EXPECT(c != NULL, "skip-verify accepts the server");
    if (c != NULL) {
        d->close(c);
    }
    pthread_join(th, NULL);
    close(s.ls);
    SSL_CTX_free(s.ctx);
    unlink(cert);
    unlink(key);
    unlink(other);
    unlink(okey);
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_tls: ok");
    return 0;
}
#else
int main(void) {
    puts("test_tls: skipped (build with TLS=1)");
    return 0;
}
#endif
