/*
 * drv_resp.c - RESP2 driver (Redis serialization protocol, version 2).
 *
 * Commands used:
 *   SET key value      reply: +OK
 *   GET key            reply: $<len>\r\n<bytes>\r\n or $-1\r\n (missing key)
 *   DEL key            reply: :<n> (0 = missing)
 *   INCR key           reply: :<n>
 *   MGET k1 ... kn     reply: *n followed by n bulk strings or nulls
 *   AUTH, SELECT       after connecting, when configured (driver.h)
 * Error replies (-ERR ...) and unexpected reply types map to PACE_ERR.
 *
 * Synchronous requests are assembled in the connection's write buffer and sent with one write. The
 * asynchronous parser works on the input buffer without copying and leaves an incomplete reply in place.
 *
 * resp-cluster (synchronous only): connects to a node of a Redis Cluster, reads CLUSTER SLOTS, connects to
 * every primary and sends each request to the primary of its key's slot (CRC16 with hash tags, as in the
 * Redis Cluster specification). A MOVED reply reloads the slot map from the node that sent it and the
 * request is sent again once. MGET is split into one MGET per slot, because a cluster refuses keys of
 * several slots in one command.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abuf.h"
#include "driver.h"
#include "netbuf.h"

struct pace_conn {
    pace_net *net;
};

/* Appends a RESP array of bulk strings to the write buffer at *off. Returns 0 or -1. */
static int encode(pace_net *n, size_t *off, unsigned argc, const char *const *argv, const size_t *lens) {
    size_t need = 16;
    for (unsigned i = 0; i < argc; i++) {
        need += lens[i] + 24;
    }
    if (pace_net_reserve(n, *off + need) != 0) {
        return -1;
    }
    *off += (size_t)snprintf((char *)n->wbuf + *off, n->wcap - *off, "*%u\r\n", argc);
    for (unsigned i = 0; i < argc; i++) {
        *off += (size_t)snprintf((char *)n->wbuf + *off, n->wcap - *off, "$%zu\r\n", lens[i]);
        memcpy(n->wbuf + *off, argv[i], lens[i]);
        *off += lens[i];
        n->wbuf[(*off)++] = '\r';
        n->wbuf[(*off)++] = '\n';
    }
    return 0;
}

/* The last error reply of this thread (resp-cluster looks for MOVED in it). */
static _Thread_local char last_error[128];

static int call_line(pace_net *n, unsigned argc, const char *const *argv, const size_t *lens, char *line,
                     size_t cap) {
    size_t off = 0;
    if (encode(n, &off, argc, argv, lens) != 0 || pace_net_send(n, n->wbuf, off) != 0) {
        return -1;
    }
    int rc = pace_net_read_line(n, line, cap);
    if (rc == 0 && line[0] == '-') {
        snprintf(last_error, sizeof(last_error), "%s", line);
    }
    return rc;
}

/* Authenticates and selects the database as configured. Returns 0 or -1. */
static int session_setup(pace_net *n) {
    char line[PACE_NET_LINE_MAX];
    const char *secret = pace_driver_auth();
    if (secret != NULL) {
        const char *colon = strchr(secret, ':');
        if (colon != NULL) {
            const char *argv[3] = {"AUTH", secret, colon + 1};
            size_t lens[3] = {4, (size_t)(colon - secret), strlen(colon + 1)};
            if (call_line(n, 3, argv, lens, line, sizeof(line)) != 0 || strcmp(line, "+OK") != 0) {
                fprintf(stderr, "AUTH failed: %s\n", line);
                return -1;
            }
        } else {
            const char *argv[2] = {"AUTH", secret};
            size_t lens[2] = {4, strlen(secret)};
            if (call_line(n, 2, argv, lens, line, sizeof(line)) != 0 || strcmp(line, "+OK") != 0) {
                fprintf(stderr, "AUTH failed: %s\n", line);
                return -1;
            }
        }
    }
    if (pace_driver_db() != 0) {
        char db[16];
        snprintf(db, sizeof(db), "%u", pace_driver_db());
        const char *argv[2] = {"SELECT", db};
        size_t lens[2] = {6, strlen(db)};
        if (call_line(n, 2, argv, lens, line, sizeof(line)) != 0 || strcmp(line, "+OK") != 0) {
            fprintf(stderr, "SELECT failed: %s\n", line);
            return -1;
        }
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

static pace_conn *resp_connect(const char *host, const char *port) {
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

static void resp_close(pace_conn *c) {
    if (c == NULL) {
        return;
    }
    pace_net_close(c->net);
    free(c);
}

static int resp_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    const char *argv[3] = {"SET", key, val};
    size_t lens[3] = {3, klen, vlen};
    char line[PACE_NET_LINE_MAX];
    if (call_line(c->net, 3, argv, lens, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    return strcmp(line, "+OK") == 0 ? PACE_OK : PACE_ERR;
}

/* Reads a bulk string reply after its header line. */
static int read_bulk(pace_net *n, const char *line, void *buf, size_t cap, size_t *vlen) {
    if (line[0] != '$') {
        return PACE_ERR;
    }
    long len = strtol(line + 1, NULL, 10);
    if (len < 0) {
        return PACE_NOTFOUND;
    }
    size_t ulen = (size_t)len;
    int too_big = ulen > cap;
    char crlf[2];
    if (pace_net_read_exact(n, too_big ? NULL : buf, ulen) != 0 || pace_net_read_exact(n, crlf, 2) != 0) {
        return PACE_ERR;
    }
    if (too_big) {
        return PACE_ERR;
    }
    if (vlen != NULL) {
        *vlen = ulen;
    }
    return PACE_OK;
}

static int resp_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    const char *argv[2] = {"GET", key};
    size_t lens[2] = {3, klen};
    char line[PACE_NET_LINE_MAX];
    if (call_line(c->net, 2, argv, lens, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    return read_bulk(c->net, line, buf, cap, vlen);
}

static int resp_del(pace_conn *c, const void *key, size_t klen) {
    const char *argv[2] = {"DEL", key};
    size_t lens[2] = {3, klen};
    char line[PACE_NET_LINE_MAX];
    if (call_line(c->net, 2, argv, lens, line, sizeof(line)) != 0 || line[0] != ':') {
        return PACE_ERR;
    }
    return strtol(line + 1, NULL, 10) > 0 ? PACE_OK : PACE_NOTFOUND;
}

static int resp_incr(pace_conn *c, const void *key, size_t klen) {
    const char *argv[2] = {"INCR", key};
    size_t lens[2] = {4, klen};
    char line[PACE_NET_LINE_MAX];
    if (call_line(c->net, 2, argv, lens, line, sizeof(line)) != 0) {
        return PACE_ERR;
    }
    return line[0] == ':' ? PACE_OK : PACE_ERR;
}

static int resp_mget(pace_conn *c, const char *const *keys, const size_t *klens, unsigned n, unsigned *hits) {
    const char *argv[PACE_MGET_MAX + 1];
    size_t lens[PACE_MGET_MAX + 1];
    argv[0] = "MGET";
    lens[0] = 4;
    for (unsigned i = 0; i < n; i++) {
        argv[i + 1] = keys[i];
        lens[i + 1] = klens[i];
    }
    char line[PACE_NET_LINE_MAX];
    if (call_line(c->net, n + 1, argv, lens, line, sizeof(line)) != 0 || line[0] != '*' ||
        strtol(line + 1, NULL, 10) != (long)n) {
        return PACE_ERR;
    }
    *hits = 0;
    for (unsigned i = 0; i < n; i++) {
        if (pace_net_read_line(c->net, line, sizeof(line)) != 0) {
            return PACE_ERR;
        }
        int rc = read_bulk(c->net, line, NULL, SIZE_MAX, NULL);
        if (rc == PACE_ERR) {
            return PACE_ERR;
        }
        *hits += rc == PACE_OK;
    }
    return PACE_OK;
}

/* ---- cluster ------------------------------------------------------------------------------------------ */

#define SLOTS 16384u

struct cnode {
    char host[256];
    char port[16];
    pace_net *net;
};

struct cluster_conn {
    struct cnode *nodes;
    unsigned n;
    int16_t owner[SLOTS]; /* node index per slot, -1 unknown */
};

/* CRC16 of the Redis Cluster specification (XMODEM: polynomial 0x1021, initial value 0). */
static uint16_t crc16(const void *data, size_t n) {
    const uint8_t *p = data;
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static unsigned keyslot(const void *key, size_t n) {
    const char *k = key;
    const char *open = memchr(k, '{', n);
    if (open != NULL) {
        const char *close = memchr(open + 1, '}', n - (size_t)(open - k) - 1);
        if (close != NULL && close > open + 1) {
            return crc16(open + 1, (size_t)(close - open - 1)) & (SLOTS - 1);
        }
    }
    return crc16(k, n) & (SLOTS - 1);
}

/* Index of node host:port, connecting to it when it is new. -1 on failure. */
static int node_of(struct cluster_conn *cc, const char *host, const char *port) {
    for (unsigned i = 0; i < cc->n; i++) {
        if (strcmp(cc->nodes[i].host, host) == 0 && strcmp(cc->nodes[i].port, port) == 0) {
            return (int)i;
        }
    }
    if (strlen(host) >= sizeof(cc->nodes[0].host) || strlen(port) >= sizeof(cc->nodes[0].port)) {
        return -1;
    }
    struct cnode *nn = realloc(cc->nodes, (cc->n + 1) * sizeof(*nn));
    if (nn == NULL) {
        return -1;
    }
    cc->nodes = nn;
    struct cnode *nd = &cc->nodes[cc->n];
    snprintf(nd->host, sizeof(nd->host), "%s", host);
    snprintf(nd->port, sizeof(nd->port), "%s", port);
    nd->net = open_session(host, port);
    if (nd->net == NULL) {
        fprintf(stderr, "resp-cluster: cannot connect to %s:%s\n", host, port);
        return -1;
    }
    return (int)cc->n++;
}

/* Reads an integer reply line (":n"). */
static int read_int(pace_net *n, long long *v) {
    char line[PACE_NET_LINE_MAX];
    if (pace_net_read_line(n, line, sizeof(line)) != 0 || line[0] != ':') {
        return -1;
    }
    *v = strtoll(line + 1, NULL, 10);
    return 0;
}

/* Reads a bulk string reply into buf (NUL-terminated). */
static int read_str(pace_net *n, char *buf, size_t cap) {
    char line[PACE_NET_LINE_MAX];
    size_t len = 0;
    if (pace_net_read_line(n, line, sizeof(line)) != 0 || read_bulk(n, line, buf, cap - 1, &len) != PACE_OK) {
        return -1;
    }
    buf[len] = '\0';
    return 0;
}

/* Skips one reply of any type. */
static int skip_reply(pace_net *n) {
    char line[PACE_NET_LINE_MAX];
    if (pace_net_read_line(n, line, sizeof(line)) != 0) {
        return -1;
    }
    if (line[0] == '$') {
        return read_bulk(n, line, NULL, SIZE_MAX, NULL) == PACE_ERR ? -1 : 0;
    }
    if (line[0] == '*') {
        long k = strtol(line + 1, NULL, 10);
        for (long i = 0; i < k; i++) {
            if (skip_reply(n) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* Loads the slot map with CLUSTER SLOTS through node `from`. Returns 0 or -1. */
static int load_slots(struct cluster_conn *cc, unsigned from) {
    const char *argv[2] = {"CLUSTER", "SLOTS"};
    size_t lens[2] = {7, 5};
    char line[PACE_NET_LINE_MAX];
    pace_net *n = cc->nodes[from].net;
    if (call_line(n, 2, argv, lens, line, sizeof(line)) != 0 || line[0] != '*') {
        fprintf(stderr, "resp-cluster: CLUSTER SLOTS failed: %s\n", line);
        return -1;
    }
    long ranges = strtol(line + 1, NULL, 10);
    struct {
        long long lo;
        long long hi;
        char host[256];
        char port[16];
    } *r = calloc(ranges > 0 ? (size_t)ranges : 1, sizeof(*r));
    if (r == NULL) {
        return -1;
    }
    int rc = 0;
    for (long i = 0; i < ranges && rc == 0; i++) {
        long long port;
        if (pace_net_read_line(n, line, sizeof(line)) != 0 || line[0] != '*') {
            rc = -1;
            break;
        }
        long parts = strtol(line + 1, NULL, 10);
        if (parts < 3 || read_int(n, &r[i].lo) != 0 || read_int(n, &r[i].hi) != 0 ||
            pace_net_read_line(n, line, sizeof(line)) != 0 || line[0] != '*') {
            rc = -1;
            break;
        }
        long fields = strtol(line + 1, NULL, 10);
        if (fields < 2 || read_str(n, r[i].host, sizeof(r[i].host)) != 0 || read_int(n, &port) != 0) {
            rc = -1;
            break;
        }
        snprintf(r[i].port, sizeof(r[i].port), "%lld", port);
        for (long f = 2; f < fields && rc == 0; f++) {
            rc = skip_reply(n);
        }
        for (long p = 3; p < parts && rc == 0; p++) {
            rc = skip_reply(n); /* replicas */
        }
    }
    for (long i = 0; i < ranges && rc == 0; i++) {
        int node = node_of(cc, r[i].host, r[i].port);
        if (node < 0 || r[i].lo < 0 || r[i].hi >= (long long)SLOTS || r[i].lo > r[i].hi) {
            rc = -1;
            break;
        }
        for (long long s = r[i].lo; s <= r[i].hi; s++) {
            cc->owner[s] = (int16_t)node;
        }
    }
    free(r);
    return rc;
}

static void cluster_close(pace_conn *c) {
    struct cluster_conn *cc = (struct cluster_conn *)c;
    if (cc == NULL) {
        return;
    }
    for (unsigned i = 0; i < cc->n; i++) {
        pace_net_close(cc->nodes[i].net);
    }
    free(cc->nodes);
    free(cc);
}

static pace_conn *cluster_connect(const char *host, const char *port) {
    struct cluster_conn *cc = calloc(1, sizeof(*cc));
    if (cc == NULL) {
        return NULL;
    }
    memset(cc->owner, 0xff, sizeof(cc->owner));
    int seed = node_of(cc, host, port);
    if (seed < 0 || load_slots(cc, (unsigned)seed) != 0) {
        cluster_close((pace_conn *)cc);
        return NULL;
    }
    return (pace_conn *)cc;
}

/* Connection of the primary of slot (the seed node for a slot without an owner). */
static struct pace_conn conn_for(struct cluster_conn *cc, unsigned slot) {
    int i = cc->owner[slot] >= 0 ? cc->owner[slot] : 0;
    return (struct pace_conn){.net = cc->nodes[i].net};
}

/* After PACE_ERR: when the error was MOVED, reloads the map from the node that sent it. Returns 1 to retry. */
static int moved(struct cluster_conn *cc, unsigned slot) {
    if (strncmp(last_error, "-MOVED ", 7) != 0) {
        return 0;
    }
    char host[256];
    char port[16];
    unsigned s;
    if (sscanf(last_error, "-MOVED %u %255[^:]:%15s", &s, host, port) != 3 || s >= SLOTS) {
        return 0;
    }
    int node = node_of(cc, host, port);
    if (node < 0) {
        return 0;
    }
    cc->owner[slot] = (int16_t)node;
    load_slots(cc, (unsigned)node);
    last_error[0] = '\0';
    return 1;
}

#define CLUSTER_CALL(cc, slot, call)                                                                         \
    do {                                                                                                     \
        for (int attempt = 0; attempt < 2; attempt++) {                                                      \
            struct pace_conn target = conn_for(cc, slot);                                                    \
            last_error[0] = '\0';                                                                            \
            int rc_ = call;                                                                                  \
            if (rc_ != PACE_ERR || !moved(cc, slot)) {                                                       \
                return rc_;                                                                                  \
            }                                                                                                \
        }                                                                                                    \
        return PACE_ERR;                                                                                     \
    } while (0)

static int cluster_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    struct cluster_conn *cc = (struct cluster_conn *)c;
    CLUSTER_CALL(cc, keyslot(key, klen), resp_put(&target, key, klen, val, vlen));
}

static int cluster_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    struct cluster_conn *cc = (struct cluster_conn *)c;
    CLUSTER_CALL(cc, keyslot(key, klen), resp_get(&target, key, klen, buf, cap, vlen));
}

static int cluster_del(pace_conn *c, const void *key, size_t klen) {
    struct cluster_conn *cc = (struct cluster_conn *)c;
    CLUSTER_CALL(cc, keyslot(key, klen), resp_del(&target, key, klen));
}

static int cluster_incr(pace_conn *c, const void *key, size_t klen) {
    struct cluster_conn *cc = (struct cluster_conn *)c;
    CLUSTER_CALL(cc, keyslot(key, klen), resp_incr(&target, key, klen));
}

static int cluster_mget_slot(struct cluster_conn *cc, unsigned slot, const char *const *keys,
                             const size_t *klens, unsigned n, unsigned *hits) {
    CLUSTER_CALL(cc, slot, resp_mget(&target, keys, klens, n, hits));
}

static int cluster_mget(pace_conn *c, const char *const *keys, const size_t *klens, unsigned n,
                        unsigned *hits) {
    struct cluster_conn *cc = (struct cluster_conn *)c;
    const char *group[PACE_MGET_MAX];
    size_t glens[PACE_MGET_MAX];
    unsigned slot_of[PACE_MGET_MAX];
    unsigned char done[PACE_MGET_MAX] = {0};
    for (unsigned i = 0; i < n; i++) {
        slot_of[i] = keyslot(keys[i], klens[i]);
    }
    *hits = 0;
    for (unsigned i = 0; i < n; i++) {
        if (done[i]) {
            continue;
        }
        unsigned g = 0;
        for (unsigned j = i; j < n; j++) {
            if (!done[j] && slot_of[j] == slot_of[i]) {
                group[g] = keys[j];
                glens[g++] = klens[j];
                done[j] = 1;
            }
        }
        unsigned h = 0;
        if (cluster_mget_slot(cc, slot_of[i], group, glens, g, &h) != PACE_OK) {
            return PACE_ERR;
        }
        *hits += h;
    }
    return PACE_OK;
}

const pace_driver pace_driver_resp_cluster = {
    .name = "resp-cluster",
    .connect = cluster_connect,
    .put = cluster_put,
    .get = cluster_get,
    .close = cluster_close,
    .del = cluster_del,
    .incr = cluster_incr,
    .mget = cluster_mget,
    .async = NULL, /* replies would come from several sockets: the synchronous runner only */
};

/* ---- asynchronous ------------------------------------------------------------------------------------ */

struct pace_aconn {
    pace_abuf b;
};

static pace_aconn *aresp_connect(const char *host, const char *port) {
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

static int aresp_fd(pace_aconn *c) {
    return c->b.s.fd;
}

static unsigned aresp_window(pace_aconn *c) {
    (void)c;
    return 0;
}

static int add_array(pace_abuf *b, unsigned argc, const char *const *argv, const size_t *lens) {
    if (pace_abuf_printf(b, "*%u\r\n", argc) != 0) {
        return -1;
    }
    for (unsigned i = 0; i < argc; i++) {
        if (pace_abuf_printf(b, "$%zu\r\n", lens[i]) != 0 || pace_abuf_add(b, argv[i], lens[i]) != 0 ||
            pace_abuf_add(b, "\r\n", 2) != 0) {
            return -1;
        }
    }
    return 0;
}

static int aresp_submit(pace_aconn *c, const pace_req *r) {
    const char *argv[PACE_MGET_MAX + 1];
    size_t lens[PACE_MGET_MAX + 1];
    unsigned argc = 2;
    argv[1] = r->key;
    lens[1] = r->klen;
    switch (r->type) {
    case PACE_OP_GET:
        argv[0] = "GET";
        lens[0] = 3;
        break;
    case PACE_OP_SET:
        argv[0] = "SET";
        lens[0] = 3;
        argv[2] = r->val;
        lens[2] = r->vlen;
        argc = 3;
        break;
    case PACE_OP_DEL:
        argv[0] = "DEL";
        lens[0] = 3;
        break;
    case PACE_OP_INCR:
        argv[0] = "INCR";
        lens[0] = 4;
        break;
    case PACE_OP_MGET:
        argv[0] = "MGET";
        lens[0] = 4;
        for (unsigned i = 0; i < r->nkeys; i++) {
            argv[i + 1] = r->keys[i];
            lens[i + 1] = r->klens[i];
        }
        argc = r->nkeys + 1;
        break;
    default:
        return PACE_ERR;
    }
    return add_array(&c->b, argc, argv, lens) == 0 && pace_abuf_push(&c->b, r->type) == 0 ? PACE_OK
                                                                                          : PACE_ERR;
}

static int aresp_io(pace_aconn *c) {
    return pace_abuf_io(&c->b) == 0 ? PACE_OK : PACE_ERR;
}

/* Parses "<prefix>...\r\n" at p: returns the line length including CRLF, 0 when incomplete. */
static size_t line_len(const uint8_t *p, size_t n) {
    const uint8_t *nl = memchr(p, '\n', n);
    return nl == NULL ? 0 : (size_t)(nl - p) + 1;
}

/*
 * Parses one bulk string or null at p. Returns the bytes used (> 0), 0 when incomplete, or -1 for a
 * type error; *found is 1 for a string and 0 for null.
 */
static long parse_bulk(const uint8_t *p, size_t n, int *found) {
    size_t ll = line_len(p, n);
    if (ll == 0) {
        return 0;
    }
    if (p[0] != '$') {
        return -1;
    }
    long len = strtol((const char *)p + 1, NULL, 10);
    if (len < 0) {
        *found = 0;
        return (long)ll;
    }
    if (ll + (size_t)len + 2 > n) {
        return 0;
    }
    *found = 1;
    return (long)(ll + (size_t)len + 2);
}

static int aresp_next(pace_aconn *c, pace_reply *out) {
    int type = pace_abuf_front(&c->b);
    if (type < 0) {
        return 0;
    }
    const uint8_t *p = pace_abuf_in(&c->b);
    size_t n = pace_abuf_avail(&c->b);
    if (n == 0) {
        return 0;
    }
    out->hits = 0;
    size_t used;
    if (p[0] == '-') {
        used = line_len(p, n);
        if (used == 0) {
            return 0;
        }
        out->status = PACE_ERR;
    } else if (type == PACE_OP_GET) {
        int found = 0;
        long u = parse_bulk(p, n, &found);
        if (u <= 0) {
            return (int)u == 0 ? 0 : PACE_ERR;
        }
        used = (size_t)u;
        out->status = found ? PACE_OK : PACE_NOTFOUND;
        out->hits = (unsigned)found;
    } else if (type == PACE_OP_SET) {
        used = line_len(p, n);
        if (used == 0) {
            return 0;
        }
        out->status = used == 5 && memcmp(p, "+OK\r\n", 5) == 0 ? PACE_OK : PACE_ERR;
    } else if (type == PACE_OP_DEL || type == PACE_OP_INCR) {
        used = line_len(p, n);
        if (used == 0) {
            return 0;
        }
        if (p[0] != ':') {
            return PACE_ERR;
        }
        long v = strtol((const char *)p + 1, NULL, 10);
        out->status = type == PACE_OP_INCR || v > 0 ? PACE_OK : PACE_NOTFOUND;
    } else if (type == PACE_OP_MGET) {
        size_t ll = line_len(p, n);
        if (ll == 0) {
            return 0;
        }
        if (p[0] != '*') {
            return PACE_ERR;
        }
        long count = strtol((const char *)p + 1, NULL, 10);
        used = ll;
        for (long i = 0; i < count; i++) {
            int found = 0;
            long u = parse_bulk(p + used, n - used, &found);
            if (u <= 0) {
                return (int)u == 0 ? 0 : PACE_ERR;
            }
            used += (size_t)u;
            out->hits += (unsigned)found;
        }
        out->status = PACE_OK;
    } else {
        return PACE_ERR;
    }
    pace_abuf_consume(&c->b, used);
    pace_abuf_pop(&c->b);
    return 1;
}

static unsigned aresp_events(pace_aconn *c) {
    return pace_abuf_events(&c->b);
}

static int aresp_arm(pace_aconn *c) {
    return pace_abuf_ready(&c->b);
}

static void aresp_close(pace_aconn *c) {
    if (c != NULL) {
        pace_abuf_close(&c->b);
        free(c);
    }
}

static const pace_adriver resp_async = {
    .connect = aresp_connect,
    .fd = aresp_fd,
    .window = aresp_window,
    .submit = aresp_submit,
    .io = aresp_io,
    .next = aresp_next,
    .events = aresp_events,
    .arm = aresp_arm,
    .close = aresp_close,
};

const pace_driver pace_driver_resp = {
    .name = "resp",
    .connect = resp_connect,
    .put = resp_put,
    .get = resp_get,
    .close = resp_close,
    .del = resp_del,
    .incr = resp_incr,
    .mget = resp_mget,
    .async = &resp_async,
};
