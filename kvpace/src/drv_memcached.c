/*
 * drv_memcached.c - memcached text protocol driver.
 *
 * Commands used:
 *   set <key> 0 0 <bytes>\r\n<data>\r\n    reply: STORED
 *   get <key> [<key> ...]\r\n              reply: VALUE <key> <flags> <bytes>\r\n<data>\r\n ... END\r\n
 *   delete <key>\r\n                       reply: DELETED or NOT_FOUND
 *   ma <key> N0 J1\r\n                     meta arithmetic: increment; a missing counter is created at 1 (the value RESP INCR gives); reply HD
 *   authentication (memcached -Y)          set of "<user> <password>" after connecting, reply STORED
 * Any other reply (ERROR, SERVER_ERROR ..., CLIENT_ERROR ...) maps to PACE_ERR. Keys must not contain
 * whitespace or control characters and are limited to 250 bytes (not checked here). INCR needs a server
 * with meta commands (memcached 1.6 or newer, arenakv).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abuf.h"
#include "driver.h"
#include "netbuf.h"

struct pace_conn {
    pace_net *net;
};

/* Sends memcached ASCII authentication when credentials are configured. Returns 0 or -1. */
static int session_setup(pace_net *n) {
    const char *secret = pace_driver_auth();
    if (secret == NULL) {
        return 0;
    }
    char data[512];
    const char *colon = strchr(secret, ':');
    int dl = colon != NULL ? snprintf(data, sizeof(data), "%.*s %s", (int)(colon - secret), secret, colon + 1)
                           : snprintf(data, sizeof(data), "default %s", secret);
    if (dl < 0 || (size_t)dl >= sizeof(data) || pace_net_reserve(n, (size_t)dl + 64) != 0) {
        return -1;
    }
    int off = snprintf((char *)n->wbuf, n->wcap, "set auth 0 0 %d\r\n%s\r\n", dl, data);
    char line[PACE_NET_LINE_MAX];
    if (pace_net_send(n, n->wbuf, (size_t)off) != 0 || pace_net_read_line(n, line, sizeof(line)) != 0 ||
        strcmp(line, "STORED") != 0) {
        fprintf(stderr, "memcached authentication failed\n");
        return -1;
    }
    return 0;
}

static pace_net *open_session(const char *host, const char *port) {
    pace_net *n = pace_net_open(host, port);
    if (n != NULL && session_setup(n) != 0) {
        pace_net_close(n);
        return NULL;
    }
    return n;
}

static pace_conn *mc_connect(const char *host, const char *port) {
    pace_net *n = open_session(host, port);
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

/* Writes "<verb> <key><suffix>\r\n" (and the data block for set) into the write buffer. Returns its length. */
static size_t build(pace_net *n, const char *verb, const void *key, size_t klen, const char *suffix,
                    const void *val, size_t vlen, int with_data) {
    size_t off = (size_t)snprintf((char *)n->wbuf, n->wcap, "%s ", verb);
    memcpy(n->wbuf + off, key, klen);
    off += klen;
    if (with_data) {
        off += (size_t)snprintf((char *)n->wbuf + off, n->wcap - off, " 0 0 %zu\r\n", vlen);
        memcpy(n->wbuf + off, val, vlen);
        off += vlen;
    } else {
        off += (size_t)snprintf((char *)n->wbuf + off, n->wcap - off, "%s", suffix);
    }
    n->wbuf[off++] = '\r';
    n->wbuf[off++] = '\n';
    return off;
}

static int mc_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + vlen + 64) != 0) {
        return PACE_ERR;
    }
    size_t off = build(n, "set", key, klen, "", val, vlen, 1);
    char line[PACE_NET_LINE_MAX];
    if (pace_net_send(n, n->wbuf, off) != 0 || pace_net_read_line(n, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    return strcmp(line, "STORED") == 0 ? PACE_OK : PACE_ERR;
}

/* Reads "VALUE ..." blocks until END. Copies the first value into buf when buf is not NULL. */
static int read_values(pace_net *n, void *buf, size_t cap, size_t *vlen, unsigned *count) {
    char line[PACE_NET_LINE_MAX];
    *count = 0;
    int too_big = 0;
    for (;;) {
        if (pace_net_read_line(n, line, sizeof(line)) != 0) {
            return PACE_ERR;
        }
        if (strcmp(line, "END") == 0) {
            return too_big ? PACE_ERR : PACE_OK;
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
        int copy = buf != NULL && *count == 0;
        if (copy && ulen > cap) {
            too_big = 1;
            copy = 0;
        }
        char crlf[2];
        if (pace_net_read_exact(n, copy ? buf : NULL, ulen) != 0 || pace_net_read_exact(n, crlf, 2) != 0) {
            return PACE_ERR;
        }
        if (copy && vlen != NULL) {
            *vlen = ulen;
        }
        (*count)++;
    }
}

static int mc_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + 16) != 0) {
        return PACE_ERR;
    }
    size_t off = build(n, "get", key, klen, "", NULL, 0, 0);
    unsigned count;
    if (pace_net_send(n, n->wbuf, off) != 0) {
        return PACE_ERR;
    }
    int rc = read_values(n, buf, cap, vlen, &count);
    if (rc != PACE_OK) {
        return rc;
    }
    return count > 0 ? PACE_OK : PACE_NOTFOUND;
}

static int mc_del(pace_conn *c, const void *key, size_t klen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + 16) != 0) {
        return PACE_ERR;
    }
    size_t off = build(n, "delete", key, klen, "", NULL, 0, 0);
    char line[PACE_NET_LINE_MAX];
    if (pace_net_send(n, n->wbuf, off) != 0 || pace_net_read_line(n, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    return strcmp(line, "DELETED") == 0 ? PACE_OK : strcmp(line, "NOT_FOUND") == 0 ? PACE_NOTFOUND : PACE_ERR;
}

static int mc_incr(pace_conn *c, const void *key, size_t klen) {
    pace_net *n = c->net;
    if (pace_net_reserve(n, klen + 32) != 0) {
        return PACE_ERR;
    }
    size_t off = build(n, "ma", key, klen, " N0 J1", NULL, 0, 0);
    char line[PACE_NET_LINE_MAX];
    if (pace_net_send(n, n->wbuf, off) != 0 || pace_net_read_line(n, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    return strncmp(line, "HD", 2) == 0 ? PACE_OK : PACE_ERR;
}

static int mc_mget(pace_conn *c, const char *const *keys, const size_t *klens, unsigned nk, unsigned *hits) {
    pace_net *n = c->net;
    size_t need = 8;
    for (unsigned i = 0; i < nk; i++) {
        need += klens[i] + 1;
    }
    if (pace_net_reserve(n, need) != 0) {
        return PACE_ERR;
    }
    size_t off = 3;
    memcpy(n->wbuf, "get", 3);
    for (unsigned i = 0; i < nk; i++) {
        n->wbuf[off++] = ' ';
        memcpy(n->wbuf + off, keys[i], klens[i]);
        off += klens[i];
    }
    n->wbuf[off++] = '\r';
    n->wbuf[off++] = '\n';
    if (pace_net_send(n, n->wbuf, off) != 0) {
        return PACE_ERR;
    }
    return read_values(n, NULL, 0, NULL, hits);
}

/* ---- asynchronous ------------------------------------------------------------------------------------ */

struct pace_aconn {
    pace_abuf b;
};

static pace_aconn *amc_connect(const char *host, const char *port) {
    pace_net *n = open_session(host, port);
    if (n == NULL) {
        return NULL;
    }
    pace_aconn *c = calloc(1, sizeof(*c));
    if (c == NULL || pace_abuf_adopt(&c->b, n) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

static int amc_fd(pace_aconn *c) {
    return c->b.s.fd;
}

static unsigned amc_window(pace_aconn *c) {
    (void)c;
    return 0;
}

static int amc_submit(pace_aconn *c, const pace_req *r) {
    pace_abuf *b = &c->b;
    int rc = 0;
    switch (r->type) {
    case PACE_OP_GET:
        rc = pace_abuf_add(b, "get ", 4) | pace_abuf_add(b, r->key, r->klen) | pace_abuf_add(b, "\r\n", 2);
        break;
    case PACE_OP_SET:
        rc = pace_abuf_add(b, "set ", 4) | pace_abuf_add(b, r->key, r->klen) |
             pace_abuf_printf(b, " 0 0 %zu\r\n", r->vlen) | pace_abuf_add(b, r->val, r->vlen) |
             pace_abuf_add(b, "\r\n", 2);
        break;
    case PACE_OP_DEL:
        rc = pace_abuf_add(b, "delete ", 7) | pace_abuf_add(b, r->key, r->klen) | pace_abuf_add(b, "\r\n", 2);
        break;
    case PACE_OP_INCR:
        rc = pace_abuf_add(b, "ma ", 3) | pace_abuf_add(b, r->key, r->klen) |
             pace_abuf_add(b, " N0 J1\r\n", 8);
        break;
    case PACE_OP_MGET:
        rc = pace_abuf_add(b, "get", 3);
        for (unsigned i = 0; i < r->nkeys; i++) {
            rc |= pace_abuf_add(b, " ", 1) | pace_abuf_add(b, r->keys[i], r->klens[i]);
        }
        rc |= pace_abuf_add(b, "\r\n", 2);
        break;
    default:
        return PACE_ERR;
    }
    return rc == 0 && pace_abuf_push(b, r->type) == 0 ? PACE_OK : PACE_ERR;
}

static int amc_io(pace_aconn *c) {
    return pace_abuf_io(&c->b) == 0 ? PACE_OK : PACE_ERR;
}

/* Length of the line at p including its terminator, 0 when incomplete. */
static size_t line_len(const uint8_t *p, size_t n) {
    const uint8_t *nl = memchr(p, '\n', n);
    return nl == NULL ? 0 : (size_t)(nl - p) + 1;
}

static int line_is(const uint8_t *p, size_t ll, const char *s) {
    size_t n = strlen(s);
    return ll == n + 2 && memcmp(p, s, n) == 0 && p[n] == '\r';
}

static int amc_next(pace_aconn *c, pace_reply *out) {
    int type = pace_abuf_front(&c->b);
    if (type < 0) {
        return 0;
    }
    const uint8_t *p = pace_abuf_in(&c->b);
    size_t n = pace_abuf_avail(&c->b);
    size_t used = 0;
    out->hits = 0;
    if (type == PACE_OP_GET || type == PACE_OP_MGET) {
        for (;;) {
            size_t ll = line_len(p + used, n - used);
            if (ll == 0) {
                return 0;
            }
            if (line_is(p + used, ll, "END")) {
                used += ll;
                break;
            }
            if (ll < 7 || memcmp(p + used, "VALUE ", 6) != 0) {
                return PACE_ERR;
            }
            /* the byte count is the last field of the line */
            size_t e = ll - 2;
            size_t s = e;
            while (s > 0 && p[used + s - 1] != ' ') {
                s--;
            }
            size_t bytes = (size_t)strtoul((const char *)p + used + s, NULL, 10);
            if (used + ll + bytes + 2 > n) {
                return 0;
            }
            used += ll + bytes + 2;
            out->hits++;
        }
        out->status = out->hits > 0 ? PACE_OK : PACE_NOTFOUND;
        if (type == PACE_OP_MGET) {
            out->status = PACE_OK;
        }
    } else {
        size_t ll = line_len(p, n);
        if (ll == 0) {
            return 0;
        }
        used = ll;
        if (type == PACE_OP_SET) {
            out->status = line_is(p, ll, "STORED") ? PACE_OK : PACE_ERR;
        } else if (type == PACE_OP_DEL) {
            out->status = line_is(p, ll, "DELETED")     ? PACE_OK
                          : line_is(p, ll, "NOT_FOUND") ? PACE_NOTFOUND
                                                        : PACE_ERR;
        } else {
            out->status = ll >= 4 && memcmp(p, "HD", 2) == 0 ? PACE_OK : PACE_ERR;
        }
    }
    pace_abuf_consume(&c->b, used);
    pace_abuf_pop(&c->b);
    return 1;
}

static unsigned amc_events(pace_aconn *c) {
    return pace_abuf_events(&c->b);
}

static int amc_arm(pace_aconn *c) {
    return pace_abuf_ready(&c->b);
}

static void amc_close(pace_aconn *c) {
    if (c != NULL) {
        pace_abuf_close(&c->b);
        free(c);
    }
}

static const pace_adriver mc_async = {
    .connect = amc_connect,
    .fd = amc_fd,
    .window = amc_window,
    .submit = amc_submit,
    .io = amc_io,
    .next = amc_next,
    .events = amc_events,
    .arm = amc_arm,
    .close = amc_close,
};

const pace_driver pace_driver_memcached = {
    .name = "memcached",
    .connect = mc_connect,
    .put = mc_put,
    .get = mc_get,
    .close = mc_close,
    .del = mc_del,
    .incr = mc_incr,
    .mget = mc_mget,
    .async = &mc_async,
};
