/* sock.c - TCP connection with optional TLS, see sock.h. */
#include "sock.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef PACE_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

static SSL_CTX *g_ctx;
static pace_tls_cfg g_cfg;
#endif

int pace_tls_setup(const pace_tls_cfg *cfg) {
#ifdef PACE_HAVE_TLS
    g_cfg = *cfg;
    SSL_CTX_free(g_ctx); /* a repeated setup replaces the context; connections hold their own reference */
    g_ctx = NULL;
    if (!cfg->enabled) {
        return 0;
    }
    g_ctx = SSL_CTX_new(TLS_client_method());
    if (g_ctx == NULL) {
        return -1;
    }
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(g_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (cfg->skip_verify) {
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, NULL);
        int ok = cfg->cacert != NULL ? SSL_CTX_load_verify_locations(g_ctx, cfg->cacert, NULL)
                                     : SSL_CTX_set_default_verify_paths(g_ctx);
        if (ok != 1) {
            fprintf(stderr, "cannot load CA certificates %s\n", cfg->cacert ? cfg->cacert : "(default)");
            return -1;
        }
    }
    if (cfg->cert != NULL && (SSL_CTX_use_certificate_chain_file(g_ctx, cfg->cert) != 1 ||
                              SSL_CTX_use_PrivateKey_file(g_ctx, cfg->key != NULL ? cfg->key : cfg->cert,
                                                          SSL_FILETYPE_PEM) != 1)) {
        fprintf(stderr, "cannot load client certificate or key\n");
        return -1;
    }
    return 0;
#else
    if (cfg->enabled) {
        fprintf(stderr, "TLS needs a build with TLS=1\n");
        return -1;
    }
    return 0;
#endif
}

const pace_tls_cfg *pace_tls_config(void) {
#ifdef PACE_HAVE_TLS
    return &g_cfg;
#else
    static const pace_tls_cfg none;
    return &none;
#endif
}

int pace_tls_enabled(void) {
#ifdef PACE_HAVE_TLS
    return g_ctx != NULL;
#else
    return 0;
#endif
}

int pace_sock_connect(pace_sock *s, const char *host, const char *port, int nonblock) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    s->fd = fd;
#ifdef PACE_HAVE_TLS
    if (g_ctx != NULL) {
        SSL *ssl = SSL_new(g_ctx);
        const char *name = g_cfg.sni != NULL ? g_cfg.sni : host;
        if (ssl == NULL || SSL_set_fd(ssl, fd) != 1) {
            if (ssl != NULL) {
                SSL_free(ssl);
            }
            pace_sock_close(s);
            return -1;
        }
        SSL_set_tlsext_host_name(ssl, name);
        if (!g_cfg.skip_verify) {
            X509_VERIFY_PARAM *vp = SSL_get0_param(ssl);
            /* Accept the name as a DNS name or an IP address. */
            if (X509_VERIFY_PARAM_set1_ip_asc(vp, name) != 1) {
                X509_VERIFY_PARAM_set1_host(vp, name, 0);
            }
        }
        if (SSL_connect(ssl) != 1) {
            fprintf(stderr, "TLS handshake with %s:%s failed\n", host, port);
            SSL_free(ssl);
            pace_sock_close(s);
            return -1;
        }
        s->ssl = ssl;
    }
#endif
    if (nonblock) {
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    return 0;
}

ssize_t pace_sock_read(pace_sock *s, void *buf, size_t n) {
#ifdef PACE_HAVE_TLS
    if (s->ssl != NULL) {
        int r = SSL_read(s->ssl, buf, (int)(n > 1u << 30 ? 1u << 30 : n));
        if (r > 0) {
            return r;
        }
        int e = SSL_get_error(s->ssl, r);
        if (e == SSL_ERROR_WANT_READ) {
            return 0;
        }
        if (e == SSL_ERROR_WANT_WRITE) {
            s->want_write = 1;
            return 0;
        }
        return -1;
    }
#endif
    for (;;) {
        ssize_t r = recv(s->fd, buf, n, 0);
        if (r > 0) {
            return r;
        }
        if (r == 0) {
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

ssize_t pace_sock_write(pace_sock *s, const void *buf, size_t n) {
#ifdef PACE_HAVE_TLS
    if (s->ssl != NULL) {
        int r = SSL_write(s->ssl, buf, (int)(n > 1u << 30 ? 1u << 30 : n));
        if (r > 0) {
            return r;
        }
        int e = SSL_get_error(s->ssl, r);
        if (e == SSL_ERROR_WANT_WRITE) {
            s->want_write = 1;
            return 0;
        }
        if (e == SSL_ERROR_WANT_READ) {
            return 0;
        }
        return -1;
    }
#endif
    for (;;) {
        ssize_t w = send(s->fd, buf, n, MSG_NOSIGNAL);
        if (w >= 0) {
            return w;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

int pace_sock_pending(const pace_sock *s) {
#ifdef PACE_HAVE_TLS
    return s->ssl != NULL && SSL_pending(s->ssl) > 0;
#else
    (void)s;
    return 0;
#endif
}

void pace_sock_close(pace_sock *s) {
#ifdef PACE_HAVE_TLS
    if (s->ssl != NULL) {
        SSL_shutdown(s->ssl);
        SSL_free(s->ssl);
        s->ssl = NULL;
    }
#endif
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
