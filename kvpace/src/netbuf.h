/*
 * netbuf.h - buffered blocking connection (TCP or TLS, see sock.h) used by the synchronous text drivers.
 *
 * A connection owns a read buffer for incremental parsing of responses and a growable write buffer for
 * assembling requests. Not thread-safe; one thread owns a connection.
 */
#ifndef PACE_NETBUF_H
#define PACE_NETBUF_H

#include <stddef.h>

#include "sock.h"

#define PACE_NET_RBUF 65536
#define PACE_NET_LINE_MAX 256

typedef struct pace_net {
    pace_sock s;
    size_t rpos;
    size_t rlen;
    unsigned char rbuf[PACE_NET_RBUF];
    unsigned char *wbuf;
    size_t wcap;
} pace_net;

/* Connects over TCP with TCP_NODELAY (and TLS when configured). Returns NULL on failure. */
pace_net *pace_net_open(const char *host, const char *port);

/* Closes the connection and frees it. Safe with NULL. */
void pace_net_close(pace_net *n);

/* Ensures the write buffer holds at least cap bytes. Returns 0 or -1. */
int pace_net_reserve(pace_net *n, size_t cap);

/* Sends n bytes from buf. Returns 0 or -1. */
int pace_net_send(pace_net *n, const void *buf, size_t len);

/* Reads one CRLF-terminated line into out (CRLF removed, NUL-terminated). Returns 0 or -1. */
int pace_net_read_line(pace_net *n, char *out, size_t cap);

/* Reads exactly len bytes into dst; discards them when dst is NULL. Returns 0 or -1. */
int pace_net_read_exact(pace_net *n, void *dst, size_t len);

#endif
