/*
 * tls.c - encrypted endpoints (TLS=1 builds): a TLS 1.3 handshake carried in messages of an inner
 * endpoint, then AES-256-GCM per message.
 *
 * Every message of an encrypted endpoint starts with a type byte:
 *   TYPE_HANDSHAKE  bytes of the TLS handshake (records written by OpenSSL into a memory BIO)
 *   TYPE_DATA       one application message: ciphertext, then the 16-byte GCM tag
 *
 * After the handshake both sides take 88 bytes from the TLS exporter (RFC 8446, section 7.5; label
 * "EXPORTER-rdmaduct-message-keys", no context): client-to-server key (32) and IV (12), then
 * server-to-client key and IV. The nonce of message number n of a direction is its IV XOR n (big-endian in
 * the last 8 bytes), as TLS 1.3 builds record nonces (RFC 8446, section 5.3); the inner transport delivers
 * messages reliably and in order, so both sides count the same n. The type byte is the additional
 * authenticated data. After every `rekey` messages of a direction (2^24 by default; RFC 8446, section 5.5
 * limits AES-GCM to 2^24.5 full-size records per key) both sides replace that direction's key and IV with
 * HMAC-SHA256(key, "rdmaduct key update" || generation), keeping key and IV independent of the message data.
 *
 * Session tickets are disabled, so that no handshake bytes follow the first data message. The connecting
 * side completes the handshake inside duct_connect() (within the connect timeout); an accepted endpoint is
 * returned at once and completes its handshake inside recv, progress and arm, so that a slow client cannot
 * block the thread that accepts. An accepted endpoint whose handshake does not finish within the connect
 * timeout fails with DUCT_EPROTO.
 *
 * A message that fails authentication breaks the endpoint (DUCT_EPROTO from then on). Registered-memory
 * sends and one-sided operations are not available (-ENOTSUP): a one-sided access bypasses the peer's CPU,
 * so it cannot be encrypted this way.
 */
#ifdef DUCT_HAVE_TLS

#include <arpa/inet.h>
#include <errno.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define TYPE_HANDSHAKE 1u
#define TYPE_DATA 2u
#define TAG 16u
#define OVERHEAD (1u + TAG)
#define KEYLEN 32u
#define IVLEN 12u

static uint64_t rekey_interval = 1ull << 24;

struct dir {
    EVP_CIPHER_CTX *cx;
    uint8_t key[KEYLEN];
    uint8_t iv[IVLEN];
    uint64_t seq;
    uint32_t gen;
};

struct tls_ep {
    struct duct_ep base;
    duct_ep *inner;
    SSL *ssl;
    BIO *rbio; /* network -> OpenSSL */
    BIO *wbio; /* OpenSSL -> network */
    int server;
    int ready;  /* handshake complete, keys derived */
    int broken; /* sticky error */
    int64_t deadline;
    struct dir tx;
    struct dir rx;
    uint8_t *pending; /* handshake bytes not yet accepted by the inner endpoint */
    size_t npending;
    uint8_t *txbuf; /* staging of encrypted messages */
    size_t txcap;
    struct duct_msg *batch;
    unsigned batch_cap;
};

/* Test hook: messages per key and direction; applies to endpoints of every context of the process. */
void duct_tls_set_rekey_interval(uint64_t n) {
    rekey_interval = n;
}

/* ---- keys ---------------------------------------------------------------------------------------------- */

static void next_key(struct dir *d) {
    uint8_t msg[24] = "rdmaduct key update";
    msg[20] = (uint8_t)(d->gen >> 24);
    msg[21] = (uint8_t)(d->gen >> 16);
    msg[22] = (uint8_t)(d->gen >> 8);
    msg[23] = (uint8_t)d->gen;
    uint8_t out[32];
    unsigned len = sizeof(out);
    HMAC(EVP_sha256(), d->key, KEYLEN, msg, sizeof(msg), out, &len);
    uint8_t ivout[32];
    msg[19] = 'i';
    HMAC(EVP_sha256(), d->key, KEYLEN, msg, sizeof(msg), ivout, &len);
    memcpy(d->key, out, KEYLEN);
    memcpy(d->iv, ivout, IVLEN);
    d->gen++;
    d->seq = 0;
    OPENSSL_cleanse(out, sizeof(out));
    OPENSSL_cleanse(ivout, sizeof(ivout));
}

static void nonce_of(const struct dir *d, uint8_t out[IVLEN]) {
    memcpy(out, d->iv, IVLEN);
    for (int i = 0; i < 8; i++) {
        out[IVLEN - 1 - i] ^= (uint8_t)(d->seq >> (8 * i));
    }
}

static int derive(struct tls_ep *e) {
    uint8_t km[2 * (KEYLEN + IVLEN)];
    static const char label[] = "EXPORTER-rdmaduct-message-keys";
    if (SSL_export_keying_material(e->ssl, km, sizeof(km), label, sizeof(label) - 1, NULL, 0, 0) != 1) {
        return DUCT_EPROTO;
    }
    const uint8_t *c2s = km;
    const uint8_t *s2c = km + KEYLEN + IVLEN;
    const uint8_t *txk = e->server ? s2c : c2s;
    const uint8_t *rxk = e->server ? c2s : s2c;
    memcpy(e->tx.key, txk, KEYLEN);
    memcpy(e->tx.iv, txk + KEYLEN, IVLEN);
    memcpy(e->rx.key, rxk, KEYLEN);
    memcpy(e->rx.iv, rxk + KEYLEN, IVLEN);
    OPENSSL_cleanse(km, sizeof(km));
    e->tx.cx = EVP_CIPHER_CTX_new();
    e->rx.cx = EVP_CIPHER_CTX_new();
    if (e->tx.cx == NULL || e->rx.cx == NULL ||
        EVP_EncryptInit_ex(e->tx.cx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_DecryptInit_ex(e->rx.cx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        return -ENOMEM;
    }
    e->ready = 1;
    return 0;
}

/* Encrypts len bytes of p into out (len + OVERHEAD bytes). Returns 0 or DUCT_EPROTO. */
static int seal(struct tls_ep *e, const void *p, size_t len, uint8_t *out) {
    struct dir *d = &e->tx;
    if (d->seq == rekey_interval) {
        next_key(d);
    }
    uint8_t nonce[IVLEN];
    nonce_of(d, nonce);
    out[0] = TYPE_DATA;
    int n = 0;
    int ok = EVP_EncryptInit_ex(d->cx, NULL, NULL, d->key, nonce) == 1 &&
             EVP_EncryptUpdate(d->cx, NULL, &n, out, 1) == 1 &&
             (len == 0 || EVP_EncryptUpdate(d->cx, out + 1, &n, p, (int)len) == 1) &&
             EVP_EncryptFinal_ex(d->cx, out + 1 + len, &n) == 1 &&
             EVP_CIPHER_CTX_ctrl(d->cx, EVP_CTRL_GCM_GET_TAG, TAG, out + 1 + len) == 1;
    return ok ? 0 : DUCT_EPROTO;
}

/* Decrypts a data message in place (m[0, len) including type and tag). Returns 0 or DUCT_EPROTO. */
static int open_msg(struct tls_ep *e, uint8_t *m, size_t len) {
    struct dir *d = &e->rx;
    if (len < OVERHEAD) {
        return DUCT_EPROTO;
    }
    if (d->seq == rekey_interval) {
        next_key(d);
    }
    uint8_t nonce[IVLEN];
    nonce_of(d, nonce);
    size_t plen = len - OVERHEAD;
    int n = 0;
    int ok = EVP_DecryptInit_ex(d->cx, NULL, NULL, d->key, nonce) == 1 &&
             EVP_DecryptUpdate(d->cx, NULL, &n, m, 1) == 1 &&
             (plen == 0 || EVP_DecryptUpdate(d->cx, m + 1, &n, m + 1, (int)plen) == 1) &&
             EVP_CIPHER_CTX_ctrl(d->cx, EVP_CTRL_GCM_SET_TAG, TAG, m + 1 + plen) == 1 &&
             EVP_DecryptFinal_ex(d->cx, m + 1 + plen, &n) == 1;
    if (!ok) {
        return DUCT_EPROTO;
    }
    d->seq++;
    return 0;
}

/* ---- handshake ----------------------------------------------------------------------------------------- */

/* Moves OpenSSL's output into one handshake message and tries to send it (and earlier ones) without
 * blocking. Returns 0 or a transport error. */
static int flush_handshake(struct tls_ep *e, int timeout_ms) {
    size_t avail = (size_t)BIO_ctrl_pending(e->wbio);
    if (avail > 0) {
        uint8_t *p = realloc(e->pending, e->npending + (e->npending == 0 ? 1 : 0) + avail);
        if (p == NULL) {
            return -ENOMEM;
        }
        e->pending = p;
        if (e->npending == 0) {
            e->pending[0] = TYPE_HANDSHAKE;
            e->npending = 1;
        }
        int r = BIO_read(e->wbio, e->pending + e->npending, (int)avail);
        e->npending += r > 0 ? (size_t)r : 0;
    }
    if (e->npending <= 1) {
        return 0;
    }
    if (e->npending > e->inner->peer_max_msg) {
        return DUCT_EPROTO; /* a handshake flight larger than a message (certificate chains of several KiB fit 64 KiB) */
    }
    int rc = e->inner->ops->send(e->inner, e->pending, e->npending, timeout_ms);
    if (rc == -ETIMEDOUT && timeout_ms == 0) {
        return 0; /* retried on the next call */
    }
    if (rc == 0) {
        e->npending = 0;
    }
    return rc;
}

/* Advances the handshake: feeds one received handshake message (or none) to OpenSSL and sends its output. */
static int step(struct tls_ep *e, const uint8_t *msg, size_t len, int timeout_ms) {
    if (msg != NULL && len > 1 && BIO_write(e->rbio, msg + 1, (int)(len - 1)) != (int)(len - 1)) {
        return -ENOMEM;
    }
    int r = SSL_do_handshake(e->ssl);
    int rc = flush_handshake(e, timeout_ms);
    if (rc != 0) {
        return rc;
    }
    if (r == 1) {
        return derive(e);
    }
    int err = SSL_get_error(e->ssl, r);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        ERR_clear_error();
        return DUCT_EPROTO;
    }
    return 0;
}

/*
 * Runs the handshake until it completes or the deadline (or, with block = 0, until no handshake message
 * is waiting). Returns 0 when ready, -ETIMEDOUT when not yet ready, or an error.
 */
static int drive(struct tls_ep *e, int64_t deadline, int block) {
    while (!e->ready) {
        if (duct_now_ms() > e->deadline) {
            e->broken = DUCT_EPROTO;
            return e->broken;
        }
        int rc = flush_handshake(e, 0);
        if (rc != 0) {
            return rc;
        }
        struct duct_rx rx;
        int wait = block ? duct_remaining_ms(deadline) : 0;
        rc = e->inner->ops->recv(e->inner, &rx, wait < 0 || wait > 100 ? (block ? 100 : 0) : wait);
        if (rc == -ETIMEDOUT) {
            if (!block || (deadline >= 0 && duct_now_ms() >= deadline)) {
                return -ETIMEDOUT;
            }
            continue;
        }
        if (rc != 0) {
            return rc;
        }
        const uint8_t *m = rx.data;
        int bad = rx.len < 1 || m[0] != TYPE_HANDSHAKE;
        rc = bad ? DUCT_EPROTO : step(e, m, rx.len, block ? 1000 : 0);
        e->inner->ops->release(e->inner, &rx);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ---- endpoint operations ------------------------------------------------------------------------------- */

static int ready_for_send(struct tls_ep *e, int timeout_ms) {
    if (e->broken) {
        return e->broken;
    }
    if (e->ready) {
        return 0;
    }
    int rc = drive(e, duct_deadline(timeout_ms), timeout_ms != 0);
    if (rc != 0 && rc != -ETIMEDOUT) {
        e->broken = rc;
    }
    return rc;
}

static int reserve(struct tls_ep *e, size_t n) {
    if (n <= e->txcap) {
        return 0;
    }
    uint8_t *p = realloc(e->txbuf, n);
    if (p == NULL) {
        return -ENOMEM;
    }
    e->txbuf = p;
    e->txcap = n;
    return 0;
}

static int tls_send(duct_ep *base, const void *data, size_t len, int timeout_ms) {
    struct tls_ep *e = (struct tls_ep *)base;
    int rc = ready_for_send(e, timeout_ms);
    if (rc != 0) {
        return rc;
    }
    if ((rc = reserve(e, len + OVERHEAD)) != 0 || (rc = seal(e, data, len, e->txbuf)) != 0) {
        return rc;
    }
    rc = e->inner->ops->send(e->inner, e->txbuf, len + OVERHEAD, timeout_ms);
    if (rc == 0) {
        e->tx.seq++;
    }
    return rc;
}

/* Encrypts msgs[0, n) into the staging area as consecutive sequence numbers starting at the current one. */
static int seal_batch(struct tls_ep *e, const struct duct_msg *msgs, unsigned n) {
    size_t need = 0;
    for (unsigned i = 0; i < n; i++) {
        need += msgs[i].len + OVERHEAD;
    }
    if (reserve(e, need) != 0) {
        return -ENOMEM;
    }
    if (n > e->batch_cap) {
        struct duct_msg *b = realloc(e->batch, n * sizeof(*b));
        if (b == NULL) {
            return -ENOMEM;
        }
        e->batch = b;
        e->batch_cap = n;
    }
    uint64_t seq = e->tx.seq;
    uint32_t gen = e->tx.gen;
    uint8_t key[KEYLEN];
    uint8_t iv[IVLEN];
    memcpy(key, e->tx.key, KEYLEN);
    memcpy(iv, e->tx.iv, IVLEN);
    size_t off = 0;
    int rc = 0;
    for (unsigned i = 0; i < n && rc == 0; i++) {
        rc = seal(e, msgs[i].data, msgs[i].len, e->txbuf + off);
        e->batch[i] = (struct duct_msg){.data = e->txbuf + off, .len = msgs[i].len + OVERHEAD};
        off += msgs[i].len + OVERHEAD;
        e->tx.seq++;
    }
    /* The caller commits the messages that were accepted; restore the state before them. */
    e->tx.seq = seq;
    e->tx.gen = gen;
    memcpy(e->tx.key, key, KEYLEN);
    memcpy(e->tx.iv, iv, IVLEN);
    OPENSSL_cleanse(key, sizeof(key));
    return rc;
}

/* Advances the transmit state by n accepted messages (re-deriving keys at the rekey boundaries). */
static void commit(struct tls_ep *e, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (e->tx.seq == rekey_interval) {
            next_key(&e->tx);
        }
        e->tx.seq++;
    }
}

/*
 * The inner endpoint's batch send does not report how many messages a timeout left sent, and the
 * transmit sequence must count exactly the messages the peer receives. The batch therefore goes out
 * through send_some, and a single blocking send waits when no message is accepted.
 */
static int tls_send_batch(duct_ep *base, const struct duct_msg *msgs, unsigned n, int timeout_ms) {
    struct tls_ep *e = (struct tls_ep *)base;
    int64_t deadline = duct_deadline(timeout_ms);
    int rc = ready_for_send(e, timeout_ms);
    if (rc != 0 || (rc = seal_batch(e, msgs, n)) != 0) {
        return rc;
    }
    unsigned done = 0;
    while (done < n) {
        int sent = e->inner->ops->send_some(e->inner, e->batch + done, n - done);
        if (sent < 0) {
            return sent;
        }
        if (sent > 0) {
            commit(e, (unsigned)sent);
            done += (unsigned)sent;
            continue;
        }
        rc = e->inner->ops->send(e->inner, e->batch[done].data, e->batch[done].len,
                                 duct_remaining_ms(deadline));
        if (rc != 0) {
            return rc;
        }
        commit(e, 1);
        done++;
    }
    return 0;
}

static int tls_send_some(duct_ep *base, const struct duct_msg *msgs, unsigned n) {
    struct tls_ep *e = (struct tls_ep *)base;
    int rc = ready_for_send(e, 0);
    if (rc == -ETIMEDOUT) {
        return 0; /* handshake in progress: nothing accepted */
    }
    if (rc != 0 || (rc = seal_batch(e, msgs, n)) != 0) {
        return rc;
    }
    int sent = e->inner->ops->send_some(e->inner, e->batch, n);
    if (sent > 0) {
        commit(e, (unsigned)sent);
    }
    return sent;
}

static int tls_recv(duct_ep *base, struct duct_rx *rx, int timeout_ms) {
    struct tls_ep *e = (struct tls_ep *)base;
    if (e->broken) {
        return e->broken;
    }
    int64_t deadline = duct_deadline(timeout_ms);
    if (!e->ready) {
        int rc = drive(e, deadline, timeout_ms != 0);
        if (rc != 0) {
            if (rc != -ETIMEDOUT) {
                e->broken = rc;
            }
            return rc;
        }
    }
    int rc = e->inner->ops->recv(e->inner, rx, duct_remaining_ms(deadline));
    if (rc != 0) {
        return rc;
    }
    uint8_t *m = (uint8_t *)rx->data; /* the inner endpoint's receive buffer, decrypted in place */
    if (rx->len < OVERHEAD || m[0] != TYPE_DATA || open_msg(e, m, rx->len) != 0) {
        e->inner->ops->release(e->inner, rx);
        e->broken = DUCT_EPROTO;
        return e->broken;
    }
    rx->data = m + 1;
    rx->len -= OVERHEAD;
    return 0;
}

static int tls_release(duct_ep *base, struct duct_rx *rx) {
    struct tls_ep *e = (struct tls_ep *)base;
    rx->data = (const uint8_t *)rx->data - 1;
    rx->len += OVERHEAD;
    return e->inner->ops->release(e->inner, rx);
}

/* Closes the inner endpoint and frees e with its TLS state and key material. */
static void tls_free(struct tls_ep *e) {
    e->inner->ops->close(e->inner);
    SSL_free(e->ssl); /* frees both BIOs */
    EVP_CIPHER_CTX_free(e->tx.cx);
    EVP_CIPHER_CTX_free(e->rx.cx);
    OPENSSL_cleanse(&e->tx, sizeof(e->tx));
    OPENSSL_cleanse(&e->rx, sizeof(e->rx));
    free(e->pending);
    free(e->txbuf);
    free(e->batch);
    free(e);
}

static void tls_close(duct_ep *base) {
    tls_free((struct tls_ep *)base);
}

static int tls_fd(const duct_ep *base) {
    const struct tls_ep *e = (const struct tls_ep *)base;
    return e->inner->ops->fd(e->inner);
}

static int tls_arm(duct_ep *base) {
    struct tls_ep *e = (struct tls_ep *)base;
    if (!e->ready && !e->broken) {
        int rc = drive(e, 0, 0);
        if (rc != 0 && rc != -ETIMEDOUT) {
            e->broken = rc;
            return -EAGAIN; /* the next recv reports the error */
        }
    }
    return e->inner->ops->arm(e->inner);
}

static int tls_direct_fd(duct_ep *base) {
    struct tls_ep *e = (struct tls_ep *)base;
    return e->inner->ops->direct_fd(e->inner);
}

static uint32_t tls_wait_events(const duct_ep *base) {
    const struct tls_ep *e = (const struct tls_ep *)base;
    return e->inner->ops->wait_events(e->inner);
}

static int tls_progress(duct_ep *base) {
    struct tls_ep *e = (struct tls_ep *)base;
    if (!e->ready && !e->broken) {
        int rc = drive(e, 0, 0);
        if (rc != 0 && rc != -ETIMEDOUT) {
            e->broken = rc;
            return rc;
        }
    }
    return e->broken ? e->broken : e->inner->ops->progress(e->inner);
}

static unsigned tls_send_space(duct_ep *base) {
    struct tls_ep *e = (struct tls_ep *)base;
    return e->ready ? e->inner->ops->send_space(e->inner) : 0;
}

static int tls_send_mr(duct_ep *base, const duct_mr *mr, size_t off, size_t len, int timeout_ms,
                       uint64_t *seq) {
    (void)base;
    (void)mr;
    (void)off;
    (void)len;
    (void)timeout_ms;
    (void)seq;
    return -ENOTSUP;
}

static int tls_rma(duct_ep *base, const struct duct_rma *op, int timeout_ms, uint64_t *seq) {
    (void)base;
    (void)op;
    (void)timeout_ms;
    (void)seq;
    return -ENOTSUP;
}

static uint64_t tls_completed(duct_ep *base) {
    (void)base;
    return 0;
}

static int tls_wait(duct_ep *base, uint64_t seq, int timeout_ms) {
    (void)base;
    (void)seq;
    (void)timeout_ms;
    return -ENOTSUP;
}

static int tls_peer_addr(const duct_ep *base, char *buf, size_t n) {
    const struct tls_ep *e = (const struct tls_ep *)base;
    return e->inner->ops->peer_addr(e->inner, buf, n);
}

static const struct duct_ep_ops tls_ep_ops = {
    .send = tls_send,
    .send_batch = tls_send_batch,
    .send_some = tls_send_some,
    .recv = tls_recv,
    .release = tls_release,
    .close = tls_close,
    .fd = tls_fd,
    .arm = tls_arm,
    .direct_fd = tls_direct_fd,
    .wait_events = tls_wait_events,
    .progress = tls_progress,
    .send_space = tls_send_space,
    .send_mr = tls_send_mr,
    .rma = tls_rma,
    .completed = tls_completed,
    .wait = tls_wait,
    .peer_addr = tls_peer_addr,
};

/* ---- contexts ------------------------------------------------------------------------------------------ */

/*
 * Creates the client and server TLS contexts of ctx from its options. Returns 0, -ENOMEM, or -EINVAL when
 * a certificate, key or CA file cannot be loaded. Without a certificate only the client context exists.
 */
int duct_tls_ctx_init(duct_ctx *ctx, const struct duct_opts *o) {
    ctx->tls_verify = o->tls_ca != NULL;
    if (o->tls_server_name != NULL && (ctx->tls_name = strdup(o->tls_server_name)) == NULL) {
        return -ENOMEM;
    }
    for (int server = 0; server < 2; server++) {
        SSL_CTX *t = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
        if (t == NULL) {
            return -ENOMEM;
        }
        SSL_CTX_set_min_proto_version(t, TLS1_3_VERSION);
        SSL_CTX_set_num_tickets(t, 0);
        SSL_CTX_set_options(t, SSL_OP_NO_TICKET);
        int ok = 1;
        if (o->tls_cert != NULL) {
            ok = SSL_CTX_use_certificate_chain_file(t, o->tls_cert) == 1 &&
                 SSL_CTX_use_PrivateKey_file(t, o->tls_key, SSL_FILETYPE_PEM) == 1 &&
                 SSL_CTX_check_private_key(t) == 1;
        } else if (server) {
            ok = 0; /* a listening side needs a certificate */
        }
        if (ok && o->tls_ca != NULL) {
            ok = SSL_CTX_load_verify_locations(t, o->tls_ca, NULL) == 1;
            SSL_CTX_set_verify(
                t, server ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_PEER, NULL);
        }
        if (!ok) {
            SSL_CTX_free(t);
            ERR_clear_error();
            if (server && o->tls_cert == NULL) {
                ctx->tls_server = NULL; /* no certificate: duct_listen() is refused with -EINVAL */
                continue;
            }
            return -EINVAL; /* the caller frees the client context with duct_tls_ctx_fini() */
        }
        if (server) {
            ctx->tls_server = t;
        } else {
            ctx->tls_client = t;
        }
    }
    return 0;
}

/* Frees what duct_tls_ctx_init() created; safe on a partly initialized context. */
void duct_tls_ctx_fini(duct_ctx *ctx) {
    SSL_CTX_free(ctx->tls_client);
    SSL_CTX_free(ctx->tls_server);
    free(ctx->tls_name);
    ctx->tls_client = NULL;
    ctx->tls_server = NULL;
    ctx->tls_name = NULL;
}

/*
 * Sets the name the server certificate must match. An IPv4 or IPv6 literal is matched against the
 * certificate's IP address entries and not sent as server name indication (RFC 6066, section 3, allows host
 * names only); other names are matched as DNS names (RFC 6125) and sent as SNI. verify = 0 sends SNI only.
 */
static int set_peer_name(SSL *ssl, const char *name, int verify) {
    unsigned char buf[16];
    int ip = inet_pton(AF_INET, name, buf) == 1 || inet_pton(AF_INET6, name, buf) == 1;
    if (!ip && SSL_set_tlsext_host_name(ssl, name) != 1) {
        return -1;
    }
    if (!verify) {
        return 0;
    }
    if (ip) {
        return X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name) == 1 ? 0 : -1;
    }
    return SSL_set1_host(ssl, name) == 1 ? 0 : -1;
}

/* Wraps inner; on failure inner is closed. */
static int wrap(duct_ctx *ctx, duct_ep *inner, int server, const char *name, duct_ep **out) {
    SSL_CTX *t = server ? ctx->tls_server : ctx->tls_client;
    struct tls_ep *e = calloc(1, sizeof(*e));
    if (t == NULL || e == NULL || inner->peer_max_msg <= OVERHEAD) {
        free(e);
        inner->ops->close(inner);
        return t == NULL ? -EINVAL : e == NULL ? -ENOMEM : -EMSGSIZE;
    }
    e->base.ops = &tls_ep_ops;
    e->base.ctx = ctx;
    e->base.peer_max_msg = inner->peer_max_msg - OVERHEAD;
    e->inner = inner;
    e->server = server;
    e->ssl = SSL_new(t);
    e->rbio = BIO_new(BIO_s_mem());
    e->wbio = BIO_new(BIO_s_mem());
    if (e->ssl == NULL || e->rbio == NULL || e->wbio == NULL) {
        BIO_free(e->rbio);
        BIO_free(e->wbio);
        SSL_free(e->ssl);
        free(e);
        inner->ops->close(inner);
        return -ENOMEM;
    }
    SSL_set_bio(e->ssl, e->rbio, e->wbio);
    if (server) {
        SSL_set_accept_state(e->ssl);
    } else {
        SSL_set_connect_state(e->ssl);
        if (name != NULL && set_peer_name(e->ssl, name, ctx->tls_verify) != 0) {
            tls_free(e);
            return -ENOMEM;
        }
    }
    int timeout = ctx->opts.connect_timeout_ms;
    e->deadline = timeout < 0 ? INT64_MAX : duct_now_ms() + timeout;
    *out = &e->base;
    return 0;
}

int duct_tls_client(duct_ctx *ctx, duct_ep *inner, const char *name, duct_ep **out) {
    duct_ep *ep;
    int rc = wrap(ctx, inner, 0, ctx->tls_name != NULL ? ctx->tls_name : name, &ep);
    if (rc != 0) {
        return rc;
    }
    struct tls_ep *e = (struct tls_ep *)ep;
    rc = step(e, NULL, 0, ctx->opts.connect_timeout_ms); /* ClientHello */
    if (rc == 0) {
        rc = drive(e, e->deadline == INT64_MAX ? -1 : e->deadline, 1);
    }
    if (rc != 0) {
        tls_close(ep);
        return rc == -ETIMEDOUT ? -ETIMEDOUT : DUCT_EPROTO;
    }
    *out = ep;
    return 0;
}

int duct_tls_server(duct_ctx *ctx, duct_ep *inner, duct_ep **out) {
    return wrap(ctx, inner, 1, NULL, out);
}

#endif
