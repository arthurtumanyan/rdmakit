/*
 * drv_resp.c - RESP2 driver (Redis serialization protocol, version 2).
 *
 * Commands used:
 *   SET key value      reply: +OK
 *   GET key            reply: $<len>\r\n<bytes>\r\n or $-1\r\n (missing key)
 * Error replies (-ERR ...) and unexpected reply types map to PACE_ERR.
 *
 * Each request is assembled in the connection's write buffer and sent with one send().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"
#include "netbuf.h"

struct pace_conn {
    pace_net *net;
};

static pace_conn *resp_connect(const char *host, const char *port) {
    pace_net *n = pace_net_open(host, port);
    if (n == NULL) {
        return NULL;
    }
    pace_conn *c = malloc(sizeof(*c));
    if (c == NULL) {
        pace_net_close(n);
        return NULL;
    }
    c->net = n;
    return c;
}

static void resp_close(pace_conn *c) {
    if (c == NULL) {
        return;
    }
    pace_net_close(c->net);
    free(c);
}

static int resp_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + vlen + 96) != 0) {
        return PACE_ERR;
    }
    int w = snprintf((char *)n->wbuf, n->wcap, "*3\r\n$3\r\nSET\r\n$%zu\r\n", klen);
    size_t off = (size_t)w;
    memcpy(n->wbuf + off, key, klen);
    off += klen;
    w = snprintf((char *)n->wbuf + off, n->wcap - off, "\r\n$%zu\r\n", vlen);
    off += (size_t)w;
    memcpy(n->wbuf + off, val, vlen);
    off += vlen;
    n->wbuf[off++] = '\r';
    n->wbuf[off++] = '\n';
    if (pace_net_send(n, n->wbuf, off) != 0) {
        return PACE_ERR;
    }
    char line[PACE_NET_LINE_MAX];
    if (pace_net_read_line(n, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    return strcmp(line, "+OK") == 0 ? PACE_OK : PACE_ERR;
}

static int resp_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + 64) != 0) {
        return PACE_ERR;
    }
    int w = snprintf((char *)n->wbuf, n->wcap, "*2\r\n$3\r\nGET\r\n$%zu\r\n", klen);
    size_t off = (size_t)w;
    memcpy(n->wbuf + off, key, klen);
    off += klen;
    n->wbuf[off++] = '\r';
    n->wbuf[off++] = '\n';
    if (pace_net_send(n, n->wbuf, off) != 0) {
        return PACE_ERR;
    }
    char line[PACE_NET_LINE_MAX];
    if (pace_net_read_line(n, line, sizeof(line)) != 0 || line[0] != '$') {
        return PACE_ERR;
    }
    long len = strtol(line + 1, NULL, 10);
    if (len < 0) {
        return PACE_NOTFOUND;
    }
    size_t ulen = (size_t)len;
    int too_big = ulen > cap;
    if (pace_net_read_exact(n, too_big ? NULL : buf, ulen) != 0) {
        return PACE_ERR;
    }
    char crlf[2];
    if (pace_net_read_exact(n, crlf, 2) != 0) {
        return PACE_ERR;
    }
    if (too_big) {
        return PACE_ERR;
    }
    *vlen = ulen;
    return PACE_OK;
}

const pace_driver pace_driver_resp = {
    .name = "resp",
    .connect = resp_connect,
    .put = resp_put,
    .get = resp_get,
    .close = resp_close,
};
