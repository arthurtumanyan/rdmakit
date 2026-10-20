/*
 * sock.h - TCP connection with optional TLS, used by the text-protocol drivers.
 *
 * pace_sock_connect() connects with TCP_NODELAY and, when TLS is configured (pace_tls_setup), performs
 * the TLS handshake, both blocking. Afterwards the socket is blocking or non-blocking as requested.
 * TLS needs a build with TLS=1 (OpenSSL); without it pace_tls_setup() fails.
 *
 * Read and write return the byte count (> 0), 0 when a non-blocking socket would block, or -1 on an
 * error or the end of the stream. A TLS read may need the socket to become writable (renegotiation,
 * key update) and vice versa; pace_sock_want_write() reports that.
 */
#ifndef PACE_SOCK_H
#define PACE_SOCK_H

#include <stddef.h>
#include <sys/types.h>

typedef struct pace_tls_cfg {
    int enabled;
    const char *cacert; /* CA bundle for server verification; NULL = system default paths */
    const char *cert;   /* client certificate (PEM), optional */
    const char *key;    /* client key (PEM), optional */
    const char *sni;    /* server name for SNI and verification; NULL = the host argument */
    int skip_verify;    /* accept any server certificate */
} pace_tls_cfg;

typedef struct pace_sock {
    int fd;
    void *ssl;
    int want_write;
} pace_sock;

/* Configures TLS for all later connections, replacing an earlier configuration; existing connections keep
 * theirs. Not thread-safe: called before connections are opened. Returns 0, or -1 with a message on stderr. */
int pace_tls_setup(const pace_tls_cfg *cfg);

/* Returns 1 when TLS is configured. */
int pace_tls_enabled(void);

/* Connects to host:port. Returns 0 or -1. */
int pace_sock_connect(pace_sock *s, const char *host, const char *port, int nonblock);

ssize_t pace_sock_read(pace_sock *s, void *buf, size_t n);
ssize_t pace_sock_write(pace_sock *s, const void *buf, size_t n);

/* Returns 1 when TLS has buffered input that a read returns without the socket becoming readable. */
int pace_sock_pending(const pace_sock *s);

/* Closes the connection (TLS close notify first). Safe on a closed socket. */
void pace_sock_close(pace_sock *s);

#endif
