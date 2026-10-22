/*
 * fuzz_tls.c - libFuzzer harness: the receive path of encrypted endpoints (tls.c, built into this harness
 * with DUCT_HAVE_TLS). The first input byte selects the phase that the remaining bytes reach:
 *   bit 0 clear  a client completes a real TLS handshake, then sends the input as raw messages on the
 *                unencrypted endpoint below its encrypted one: data records with forged or changed tags,
 *                unexpected handshake messages, unknown type bytes
 *   bit 0 set    an unencrypted client sends the input as raw messages from the start: handshake bytes
 *                for OpenSSL, data records before any keys exist
 * The rest of the input is split into messages by 2-byte little-endian length prefixes. The encrypted
 * server endpoint receives until it reports an error; DUCT_EPROTO and DUCT_ECLOSED are the acceptable
 * outcomes, and any delivered message must fit the reduced message limit (others abort, which the fuzzer
 * reports). Connections close with a reset (SO_LINGER 0, see fuzz_tcp_stream.c).
 */
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../src/tls.c"

#define MAX_MSG 4096u

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
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"fuzz", -1, -1, 0);
    X509_set_issuer_name(x, name);
    int ok = X509_sign(x, pk, EVP_sha256()) > 0;
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

struct accepted {
    duct_listener *l;
    duct_ep *ep;
    int rc;
    int handshake; /* the client is encrypted: complete the handshake here */
};

static void *accept_main(void *arg) {
    struct accepted *a = arg;
    a->rc = duct_accept(a->l, 2000, &a->ep);
    if (a->rc == 0 && a->handshake) {
        /* Completes the handshake of an encrypted client while the client waits in duct_connect(). */
        struct tls_ep *e = (struct tls_ep *)a->ep;
        drive(e, duct_deadline(2000), 1);
    }
    return NULL;
}

static duct_ctx *make_ctx(int tls, const char *cert, const char *key) {
    struct duct_opts o;
    duct_opts_default(&o);
    o.transport = DUCT_TRANSPORT_TCP;
    o.max_msg = MAX_MSG;
    o.recv_depth = 4;
    o.send_depth = 4;
    o.connect_timeout_ms = 2000;
    o.tls = tls;
    o.tls_cert = cert;
    o.tls_key = key;
    duct_ctx *ctx;
    if (duct_ctx_create(&o, &ctx) != 0) {
        abort();
    }
    return ctx;
}

/* Raw endpoint of a client: the endpoint below the encrypted one, or the plain endpoint itself. */
static duct_ep *raw_of(duct_ep *ep, int encrypted) {
    return encrypted ? ((struct tls_ep *)ep)->inner : ep;
}

static void set_reset(duct_ep *raw) {
    /* duct_ep_fd() of a TCP endpoint is its epoll set; the socket is the direct descriptor. */
    int fd = duct_ep_direct_fd(raw);
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static duct_ctx *server;
    static duct_ctx *client;
    static duct_ctx *plain;
    static duct_listener *l;
    static char port[16];
    if (server == NULL) {
        char cert[64];
        char key[64];
        snprintf(cert, sizeof(cert), "/tmp/duct-fuzz-%d.crt", (int)getpid());
        snprintf(key, sizeof(key), "/tmp/duct-fuzz-%d.key", (int)getpid());
        if (make_cert(cert, key) != 0) {
            abort();
        }
        server = make_ctx(1, cert, key);
        client = make_ctx(1, NULL, NULL);
        plain = make_ctx(0, NULL, NULL);
        unlink(cert);
        unlink(key);
        if (duct_listen(server, "127.0.0.1", "0", &l) != 0) {
            abort();
        }
        snprintf(port, sizeof(port), "%d", duct_listener_port(l));
    }
    if (n < 1) {
        return 0;
    }
    int encrypted = (data[0] & 1) == 0;
    struct accepted a = {.l = l, .handshake = encrypted};
    pthread_t th;
    pthread_create(&th, NULL, accept_main, &a);
    duct_ep *c;
    int rc = duct_connect(encrypted ? client : plain, "127.0.0.1", port, &c);
    pthread_join(th, NULL);
    if (rc != 0 || a.rc != 0) {
        if (rc == 0) {
            duct_ep_close(c);
        }
        if (a.rc == 0) {
            duct_ep_close(a.ep);
        }
        return 0; /* connection setup failed (port exhaustion, timeout): skip the input */
    }
    duct_ep *raw = raw_of(c, encrypted);
    for (size_t off = 1; off + 2 <= n;) {
        size_t len = (size_t)data[off] | (size_t)data[off + 1] << 8;
        off += 2;
        if (len > n - off) {
            len = n - off;
        }
        if (len > raw->peer_max_msg || raw->ops->send(raw, data + off, len, 1000) != 0) {
            break;
        }
        off += len;
    }
    set_reset(raw);
    duct_ep_close(c);
    for (int guard = 0; guard < 256; guard++) {
        struct duct_rx rx;
        rc = duct_recv(a.ep, &rx, 1000);
        if (rc == 0) {
            if (rx.len > MAX_MSG - OVERHEAD) {
                abort();
            }
            duct_rx_release(a.ep, &rx);
            continue;
        }
        if (rc != DUCT_EPROTO && rc != DUCT_ECLOSED) {
            abort();
        }
        break;
    }
    duct_ep_close(a.ep);
    return 0;
}
