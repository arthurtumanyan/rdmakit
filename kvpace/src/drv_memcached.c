/*
 * drv_memcached.c - memcached text protocol driver.
 *
 * Commands used:
 *   set <key> 0 0 <bytes>\r\n<data>\r\n    reply: STORED
 *   get <key>\r\n                          reply: VALUE <key> <flags> <bytes>\r\n<data>\r\nEND\r\n  or  END\r\n
 * Any other reply (ERROR, SERVER_ERROR ..., CLIENT_ERROR ...) maps to PACE_ERR. Keys must not contain
 * whitespace or control characters and are limited to 250 bytes (not checked here).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"
#include "netbuf.h"

struct pace_conn {
    pace_net *net;
};

static pace_conn *mc_connect(const char *host, const char *port) {
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

static void mc_close(pace_conn *c) {
    if (c == NULL) {
        return;
    }
    pace_net_close(c->net);
    free(c);
}

static int mc_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + vlen + 64) != 0) {
        return PACE_ERR;
    }
    memcpy(n->wbuf, "set ", 4);
    size_t off = 4;
    memcpy(n->wbuf + off, key, klen);
    off += klen;
    off += (size_t)snprintf((char *)n->wbuf + off, n->wcap - off, " 0 0 %zu\r\n", vlen);
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
    return strcmp(line, "STORED") == 0 ? PACE_OK : PACE_ERR;
}

static int mc_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + 16) != 0) {
        return PACE_ERR;
    }
    memcpy(n->wbuf, "get ", 4);
    size_t off = 4;
    memcpy(n->wbuf + off, key, klen);
    off += klen;
    n->wbuf[off++] = '\r';
    n->wbuf[off++] = '\n';
    if (pace_net_send(n, n->wbuf, off) != 0) {
        return PACE_ERR;
    }
    char line[PACE_NET_LINE_MAX];
    if (pace_net_read_line(n, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    if (strcmp(line, "END") == 0) {
        return PACE_NOTFOUND;
    }
    /* VALUE <key> <flags> <bytes> */
    if (strncmp(line, "VALUE ", 6) != 0) {
        return PACE_ERR;
    }
    char *p = strrchr(line, ' ');
    if (p == NULL) {
        return PACE_ERR;
    }
    size_t ulen = (size_t)strtoul(p + 1, NULL, 10);
    int too_big = ulen > cap;
    if (pace_net_read_exact(n, too_big ? NULL : buf, ulen) != 0) {
        return PACE_ERR;
    }
    char crlf[2];
    if (pace_net_read_exact(n, crlf, 2) != 0 || pace_net_read_line(n, line, sizeof(line)) != 0 ||
        strcmp(line, "END") != 0) {
        return PACE_ERR;
    }
    if (too_big) {
        return PACE_ERR;
    }
    *vlen = ulen;
    return PACE_OK;
}

const pace_driver pace_driver_memcached = {
    .name = "memcached",
    .connect = mc_connect,
    .put = mc_put,
    .get = mc_get,
    .close = mc_close,
};
