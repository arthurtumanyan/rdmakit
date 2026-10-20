/*
 * drv_native.c - driver for the arenakv server protocol (akvproto) over rdmaduct.
 *
 * Options (--driver-opt key=value):
 *   transport   tcp | rdma | auto (default: tcp)
 *   wait        poll | event (default: event)
 *   max-msg     largest message in bytes (default: 65536)
 *   depth       send and receive buffers per connection (default: 8)
 * Operations map to AKVP_PUT and AKVP_GET; a missing key is PACE_NOTFOUND. Transport errors and unexpected
 * statuses map to PACE_ERR.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"
#include "akvclient.h"

static struct duct_opts g_opts;
static int g_opts_init;

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
    } else {
        return -1;
    }
    return 0;
}

struct pace_conn {
    akvc *c;
};

static pace_conn *native_connect(const char *host, const char *port) {
    opts_init();
    pace_conn *conn = malloc(sizeof(*conn));
    if (conn == NULL) {
        return NULL;
    }
    if (akvc_connect(&conn->c, &g_opts, host, port) != 0) {
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

const pace_driver pace_driver_native = {
    .name = "native",
    .connect = native_connect,
    .put = native_put,
    .get = native_get,
    .close = native_close,
    .set_option = native_set_option,
};
