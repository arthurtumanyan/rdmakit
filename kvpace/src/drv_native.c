/*
 * drv_native.c - driver for the arenakv server protocol (akvproto) over rdmaduct.
 *
 * Options (--driver-opt key=value):
 *   transport   tcp | rdma | auto (default: tcp)
 *   wait        poll | event (default: event)
 *   max-msg     largest message in bytes (default: 65536)
 *   depth       send and receive buffers per connection (default: 8)
 *   pipeline    requests in flight per connection, set by kvpace from --pipeline: raises depth to
 *               pipeline + 1 (the client window is depth - 1, see akvclient.h) unless depth was given
 * Operations map to AKVP_PUT, AKVP_GET, AKVP_DEL, AKVP_INCRS (counters are created at 0) and AKVP_MGET; a
 * missing key is PACE_NOTFOUND. Transport errors and unexpected statuses map to PACE_ERR. Credentials
 * (driver.h) are sent with AKVP_AUTH after connecting. The asynchronous interface posts requests with
 * akvc_post() within the client's window, sends the staged requests with akvc_push() in io(), and takes
 * responses with akvc_next(..., 0); the endpoint's rdmaduct descriptor drives the event loop.
 * With --tls (kvpace, arenakv and rdmaduct built with TLS=1) connections are encrypted rdmaduct endpoints
 * (server started with --native-tls): --cacert verifies the server (required unless --tls-skip-verify),
 * --cert and --key send a client certificate, --sni names the expected server name.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#include "akvclient.h"
#include "driver.h"
#include "sock.h"

static struct duct_opts g_opts;
static int g_opts_init;
static int g_depth_given; /* depth set explicitly: pipeline does not change it */

static void opts_init(void) {
    if (!g_opts_init) {
        duct_opts_default(&g_opts);
        g_opts.transport = DUCT_TRANSPORT_TCP;
        g_opts.wait = DUCT_WAIT_EVENT;
        g_opts.recv_depth = 8;
        g_opts.send_depth = 8;
        g_opts_init = 1;
    }
}

static int native_set_option(const char *key, const char *value) {
    opts_init();
    if (strcmp(key, "transport") == 0) {
        if (strcmp(value, "tcp") == 0) {
            g_opts.transport = DUCT_TRANSPORT_TCP;
            g_opts.wait = DUCT_WAIT_EVENT;
        } else if (strcmp(value, "rdma") == 0) {
            g_opts.transport = DUCT_TRANSPORT_RDMA;
        } else if (strcmp(value, "auto") == 0) {
            g_opts.transport = DUCT_TRANSPORT_AUTO;
        } else {
            return -1;
        }
    } else if (strcmp(key, "wait") == 0) {
        if (strcmp(value, "poll") == 0) {
            g_opts.wait = DUCT_WAIT_POLL;
        } else if (strcmp(value, "event") == 0) {
            g_opts.wait = DUCT_WAIT_EVENT;
        } else {
            return -1;
        }
    } else if (strcmp(key, "max-msg") == 0) {
        long v = strtol(value, NULL, 10);
        if (v < 64 || v > (1 << 26)) {
            return -1;
        }
        g_opts.max_msg = (uint32_t)v;
    } else if (strcmp(key, "depth") == 0) {
        long v = strtol(value, NULL, 10);
        if (v < 2 || v > 4096) {
            return -1;
        }
        g_opts.recv_depth = g_opts.send_depth = (uint32_t)v;
        g_depth_given = 1;
    } else if (strcmp(key, "pipeline") == 0) {
        long v = strtol(value, NULL, 10);
        if (v < 1 || v > 4095) {
            return -1;
        }
        if (!g_depth_given && (uint32_t)v + 1 > g_opts.send_depth) {
            g_opts.recv_depth = g_opts.send_depth = (uint32_t)v + 1;
        }
    } else {
        return -1;
    }
    return 0;
}

struct pace_conn {
    akvc *c;
};

/* Takes the encryption settings of the global TLS configuration (main.c) once. Returns 0 or -1. */
static int tls_opts(void) {
    static int done;
    static int result;
    if (done) {
        return result;
    }
    done = 1;
    const pace_tls_cfg *t = pace_tls_config();
    if (!pace_tls_enabled()) {
        return 0;
    }
    if (t->cacert == NULL && !t->skip_verify) {
        fprintf(stderr, "native driver: --tls needs --cacert (or --tls-skip-verify)\n");
        result = -1;
        return result;
    }
    g_opts.tls = 1;
    g_opts.tls_ca = t->skip_verify ? NULL : t->cacert;
    g_opts.tls_cert = t->cert;
    g_opts.tls_key = t->key;
    g_opts.tls_server_name = t->sni;
    return 0;
}

static pace_conn *native_connect(const char *host, const char *port) {
    opts_init();
    if (tls_opts() != 0) {
        return NULL;
    }
    pace_conn *conn = malloc(sizeof(*conn));
    if (conn == NULL) {
        return NULL;
    }
    if (akvc_connect(&conn->c, &g_opts, host, port) != 0) {
        free(conn);
        return NULL;
    }
    if (pace_driver_auth() != NULL && akvc_auth(conn->c, pace_driver_auth()) != AKVP_OK) {
        fprintf(stderr, "native authentication failed\n");
        akvc_close(conn->c);
        free(conn);
        return NULL;
    }
    return conn;
}

static void native_close(pace_conn *conn) {
    if (conn == NULL) {
        return;
    }
    akvc_close(conn->c);
    free(conn);
}

static int native_put(pace_conn *conn, const void *key, size_t klen, const void *val, size_t vlen) {
    return akvc_put(conn->c, key, klen, val, vlen) == AKVP_OK ? PACE_OK : PACE_ERR;
}

static int native_get(pace_conn *conn, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    int rc = akvc_get(conn->c, key, klen, buf, cap, vlen);
    if (rc == AKVP_OK) {
        return PACE_OK;
    }
    return rc == AKVP_NOTFOUND ? PACE_NOTFOUND : PACE_ERR;
}

static int status_of(int rc) {
    return rc == AKVP_OK ? PACE_OK : rc == AKVP_NOTFOUND ? PACE_NOTFOUND : PACE_ERR;
}

static int native_del(pace_conn *conn, const void *key, size_t klen) {
    return status_of(akvc_del(conn->c, key, klen));
}

static int native_incr(pace_conn *conn, const void *key, size_t klen) {
    int64_t result;
    return status_of(akvc_incr(conn->c, key, klen, 1, 1, &result));
}

/* Builds the MGET value (2-byte length and key, repeated) into buf; returns its length or 0 when too large. */
static size_t mget_value(uint8_t *buf, size_t cap, const char *const *keys, const size_t *klens, unsigned n) {
    size_t off = 0;
    for (unsigned i = 0; i < n; i++) {
        if (off + 2 + klens[i] > cap) {
            return 0;
        }
        buf[off] = (uint8_t)klens[i];
        buf[off + 1] = (uint8_t)(klens[i] >> 8);
        memcpy(buf + off + 2, keys[i], klens[i]);
        off += 2 + klens[i];
    }
    return off;
}

/* Counts the keys found in an MGET response payload. */
static unsigned mget_hits(const uint8_t *p, size_t len) {
    unsigned hits = 0;
    size_t off = 0;
    while (off + 8 <= len) {
        uint32_t vlen = akvp_get32(p + off + 4);
        hits += p[off] == AKVP_OK;
        off += 8 + (p[off] == AKVP_OK ? vlen : 0);
    }
    return hits;
}

static int native_mget(pace_conn *conn, const char *const *keys, const size_t *klens, unsigned n,
                       unsigned *hits) {
    uint8_t buf[PACE_MGET_MAX * (PACE_KEY_MAX + 2)];
    size_t vl = mget_value(buf, sizeof(buf), keys, klens, n);
    struct akvc_resp r;
    if (vl == 0 || akvc_post(conn->c, AKVP_MGET, NULL, 0, buf, vl, NULL, NULL) != 0 ||
        akvc_next(conn->c, &r, 10000) != 0 || r.status != AKVP_OK) {
        return PACE_ERR;
    }
    *hits = mget_hits(r.payload, r.len);
    return PACE_OK;
}

/* ---- asynchronous ------------------------------------------------------------------------------------ */

struct pace_aconn {
    pace_conn conn;
    uint8_t types[4096]; /* operation types of outstanding requests (window is below 4096) */
    unsigned head;
    unsigned count;
};

static pace_aconn *anative_connect(const char *host, const char *port) {
    pace_conn *c = native_connect(host, port);
    if (c == NULL) {
        return NULL;
    }
    pace_aconn *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        native_close(c);
        return NULL;
    }
    a->conn = *c;
    free(c);
    return a;
}

static int anative_fd(pace_aconn *a) {
    return akvc_direct_fd(a->conn.c);
}

static unsigned anative_window(pace_aconn *a) {
    unsigned w = akvc_window(a->conn.c);
    return w < 4096 ? w : 4095;
}

static int anative_submit(pace_aconn *a, const pace_req *r) {
    uint8_t buf[PACE_MGET_MAX * (PACE_KEY_MAX + 2)];
    uint8_t delta[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    int rc;
    switch (r->type) {
    case PACE_OP_GET:
        rc = akvc_post(a->conn.c, AKVP_GET, r->key, r->klen, NULL, 0, NULL, NULL);
        break;
    case PACE_OP_SET:
        rc = akvc_post(a->conn.c, AKVP_PUT, r->key, r->klen, r->val, r->vlen, NULL, NULL);
        break;
    case PACE_OP_DEL:
        rc = akvc_post(a->conn.c, AKVP_DEL, r->key, r->klen, NULL, 0, NULL, NULL);
        break;
    case PACE_OP_INCR:
        rc = akvc_post(a->conn.c, AKVP_INCRS, r->key, r->klen, delta, 8, NULL, NULL);
        break;
    case PACE_OP_MGET: {
        size_t vl = mget_value(buf, sizeof(buf), r->keys, r->klens, r->nkeys);
        rc = vl == 0 ? -1 : akvc_post(a->conn.c, AKVP_MGET, NULL, 0, buf, vl, NULL, NULL);
        break;
    }
    default:
        return PACE_ERR;
    }
    if (rc != 0) {
        return PACE_ERR;
    }
    a->types[(a->head + a->count) % 4096] = (uint8_t)r->type;
    a->count++;
    return PACE_OK;
}

static int anative_io(pace_aconn *a) {
    return akvc_push(a->conn.c) == 0 ? PACE_OK : PACE_ERR; /* responses are taken by next() */
}

static int anative_next(pace_aconn *a, pace_reply *out) {
    if (a->count == 0) {
        return 0;
    }
    struct akvc_resp r;
    int rc = akvc_next(a->conn.c, &r, 0);
    if (rc == -ETIMEDOUT) {
        return 0;
    }
    if (rc != 0) {
        return PACE_ERR;
    }
    uint8_t type = a->types[a->head];
    a->head = (a->head + 1) % 4096;
    a->count--;
    out->hits = 0;
    out->status = status_of(r.status);
    if (type == PACE_OP_MGET && r.status == AKVP_OK) {
        out->hits = mget_hits(r.payload, r.len);
    }
    if (type == PACE_OP_GET) {
        out->hits = r.status == AKVP_OK;
    }
    return 1;
}

static unsigned anative_events(pace_aconn *a) {
    return akvc_wait_events(a->conn.c);
}

static int anative_arm(pace_aconn *a) {
    int rc = akvc_arm(a->conn.c);
    return rc == 0 ? 0 : 1;
}

static void anative_close(pace_aconn *a) {
    if (a != NULL) {
        akvc_close(a->conn.c);
        free(a);
    }
}

static const pace_adriver native_async = {
    .connect = anative_connect,
    .fd = anative_fd,
    .window = anative_window,
    .submit = anative_submit,
    .io = anative_io,
    .next = anative_next,
    .events = anative_events,
    .arm = anative_arm,
    .close = anative_close,
};

const pace_driver pace_driver_native = {
    .name = "native",
    .connect = native_connect,
    .put = native_put,
    .get = native_get,
    .close = native_close,
    .set_option = native_set_option,
    .del = native_del,
    .incr = native_incr,
    .mget = native_mget,
    .async = &native_async,
};
