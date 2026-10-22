/*
 * replication.c - primary and replica sides of replication over the native transport, see repl.h for
 * the stream and akvd.h for the configuration.
 *
 * Primary: a replica's AKVP_REPLSYNC request hands its endpoint over to a feed thread, which sends either
 * the continuation or a full copy (akv_foreach) and then the stream from the backlog, and reads the
 * replica's acknowledgements. A replica that falls more than the backlog size behind is disconnected and
 * resynchronizes fully when it reconnects.
 *
 * Replica: one link thread connects to the primary, authenticates, requests the stream from its own
 * (replication id, offset), applies the records with akv_apply() in follower mode, appends the raw stream to
 * its own backlog (so that its replicas and, after a promotion, the replicas of the old primary see the
 * offsets of the primary), and acknowledges the applied offset. Records of a transaction are held back until
 * its end record and applied with every shard locked, so that readers of the replica never observe part of
 * a transaction. The link reconnects after an error.
 *
 * Changes made by the link thread reach the append-only log through the change feed as usual, but not the
 * backlog: repl_link_thread() tells the server's change feed to skip it.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "akvproto.h"
#include "srv.h"

static _Thread_local int on_link;

int repl_link_thread(void) {
    return on_link;
}

struct feed {
    struct repl *r;
    duct_ep *ep;
    uint64_t id;
    char addr[64];
    int port; /* listening port the replica reported */
    _Atomic uint64_t acked;
    atomic_int online; /* the copy is complete: streaming */
    atomic_int done;
    pthread_t th;
    struct feed *next;
};

struct repl {
    akvd *d;
    repl_backlog *b;
    pthread_mutex_t m;
    struct feed *feeds;
    uint64_t next_feed;
    /* replica side */
    char host[256];
    char port[16];
    int replica;       /* following a primary */
    atomic_int stop;   /* the link thread must end */
    atomic_int linked; /* the link is up and the copy (if any) is complete */
    atomic_llong last_io;
    pthread_t link;
    int link_started;
    duct_ctx *ctx; /* client context of the link (the server's options) */
};

static int64_t mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec;
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = v << 8 | p[i];
    }
    return v;
}

/* ---- primary side -------------------------------------------------------------------------------------- */

struct base_ctx {
    duct_ep *ep;
    struct obuf buf;
    size_t chunk;
    int64_t now_ms;
    int err;
};

/* Sends the buffered copy bytes as REPL_BASE messages of at most chunk bytes. */
static int base_flush(struct base_ctx *b, int all) {
    while (b->buf.len - b->buf.off >= b->chunk || (all && b->buf.len > b->buf.off)) {
        size_t n = b->buf.len - b->buf.off < b->chunk ? b->buf.len - b->buf.off : b->chunk;
        uint8_t *msg = malloc(n + 1);
        if (msg == NULL) {
            return -ENOMEM;
        }
        msg[0] = REPL_BASE;
        memcpy(msg + 1, b->buf.p + b->buf.off, n);
        int rc = duct_send(b->ep, msg, n + 1, 30000);
        free(msg);
        if (rc != 0) {
            return rc;
        }
        b->buf.off += n;
    }
    ob_compact(&b->buf);
    return 0;
}

static int base_add(void *arg, const akv_record *rec) {
    struct base_ctx *b = arg;
    if (rec->ttl_ms == 0) {
        return 0;
    }
    akv_change c = {.kind = AKV_CHG_PUT,
                    .key = rec->key,
                    .klen = rec->klen,
                    .val = rec->val,
                    .vlen = rec->vlen,
                    .flags = rec->flags,
                    .expire_unix_ms = rec->ttl_ms < 0 ? 0 : b->now_ms + rec->ttl_ms};
    if (aof_record_encode(&b->buf, &c) != 0) {
        return -ENOMEM;
    }
    return base_flush(b, 0);
}

/* Reads the acknowledgements a replica sent so far. Returns 0, or a transport error. */
static int read_acks(struct feed *f) {
    for (;;) {
        struct duct_rx rx;
        int rc = duct_recv(f->ep, &rx, 0);
        if (rc == -ETIMEDOUT) {
            return 0;
        }
        if (rc != 0) {
            return rc;
        }
        const uint8_t *p = rx.data;
        if (rx.len == 9 && p[0] == REPL_ACK) {
            uint64_t a = get64(p + 1);
            if (atomic_exchange(&f->acked, a) != a) {
                akvd_wake_waiters(f->r->d); /* WAIT may be satisfied now */
            }
        }
        duct_rx_release(f->ep, &rx);
    }
}

struct feed_start {
    struct feed *f;
    char rid[REPL_ID_LEN];
    uint64_t roff;
};

static void *feed_run(void *arg) {
    struct feed_start *fs = arg;
    struct feed *f = fs->f;
    struct repl *r = f->r;
    akvd *d = r->d;
    size_t cap = duct_ep_peer_max_msg(f->ep);
    uint32_t own = akvd_cfg(d)->max_msg;
    if (cap > own) {
        cap = own;
    }
    uint8_t *msg = malloc(cap);
    int rc = msg == NULL ? -ENOMEM : 0;
    uint64_t off = 0;
    if (rc == 0) {
        char id[REPL_ID_LEN];
        char id2[REPL_ID_LEN];
        uint64_t second;
        int partial = repl_backlog_plan(r->b, fs->rid, fs->roff, &off);
        repl_backlog_ids(r->b, id, id2, &second);
        if (partial) {
            msg[0] = REPL_CONTINUE;
            memcpy(msg + 1, id, REPL_ID_LEN);
            rc = duct_send(f->ep, msg, 1 + REPL_ID_LEN, 30000);
            akvd_log(d, AKVD_LOG_INFO, "replica %s: continuing at offset %llu", f->addr,
                     (unsigned long long)off);
        } else {
            msg[0] = REPL_FULL;
            memcpy(msg + 1, id, REPL_ID_LEN);
            put64(msg + 1 + REPL_ID_LEN, off);
            rc = duct_send(f->ep, msg, 1 + REPL_ID_LEN + 8, 30000);
            akvd_log(d, AKVD_LOG_INFO, "replica %s: full copy, stream from offset %llu", f->addr,
                     (unsigned long long)off);
            if (rc == 0) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                struct base_ctx b = {.ep = f->ep,
                                     .chunk = cap - 1,
                                     .now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000};
                rc = akv_foreach(akvd_store(d), base_add, &b);
                if (rc == 0) {
                    rc = base_flush(&b, 1);
                }
                ob_free(&b.buf);
                if (rc == 0) {
                    msg[0] = REPL_BASE_END;
                    rc = duct_send(f->ep, msg, 1, 30000);
                }
            }
        }
    }
    free(fs);
    if (rc == 0) {
        atomic_store(&f->online, 1);
    }
    int64_t last_ping = mono_s();
    while (rc == 0 && !atomic_load(&f->done)) {
        long n = repl_backlog_read(r->b, off, msg + 9, cap - 9, 200);
        if (n == -ERANGE) {
            akvd_log(d, AKVD_LOG_WARN,
                     "replica %s: more than the backlog size behind; disconnected (it resynchronizes fully)",
                     f->addr);
            break;
        }
        if (n > 0) {
            msg[0] = REPL_STREAM;
            put64(msg + 1, off);
            rc = duct_send(f->ep, msg, 9 + (size_t)n, 10000);
            off += (uint64_t)n;
        } else if (mono_s() != last_ping) {
            msg[0] = REPL_PING;
            put64(msg + 1, repl_backlog_end(r->b));
            rc = duct_send(f->ep, msg, 9, 30000);
            last_ping = mono_s();
        }
        if (rc == 0) {
            rc = read_acks(f);
        }
    }
    if (rc != 0 && rc != DUCT_ECLOSED) {
        akvd_log(d, AKVD_LOG_INFO, "replica %s: link ended: %s", f->addr, duct_strerror(rc));
    }
    free(msg);
    duct_ep_close(f->ep);
    atomic_store(&f->done, 1); /* after the close: reap() may free the feed from now on */
    akvd_wake_waiters(d);
    return NULL;
}

/* Joins and frees the feeds whose threads have ended (r->m held). */
static void reap(struct repl *r) {
    struct feed **pp = &r->feeds;
    while (*pp != NULL) {
        struct feed *f = *pp;
        if (atomic_load(&f->done)) {
            pthread_join(f->th, NULL);
            *pp = f->next;
            free(f);
        } else {
            pp = &f->next;
        }
    }
}

int repl_feed_start(struct repl *r, duct_ep *ep, const uint8_t *req, size_t len) {
    if (len < REPL_ID_LEN + 8) {
        return -EINVAL;
    }
    struct feed *f = calloc(1, sizeof(*f));
    struct feed_start *fs = calloc(1, sizeof(*fs));
    if (f == NULL || fs == NULL) {
        free(f);
        free(fs);
        return -ENOMEM;
    }
    f->r = r;
    f->ep = ep;
    if (duct_ep_peer_addr(ep, f->addr, sizeof(f->addr)) != 0) {
        snprintf(f->addr, sizeof(f->addr), "?");
    }
    char *colon = strrchr(f->addr, ':');
    if (colon != NULL) {
        *colon = '\0'; /* the connection's own port; the replica reports its listening port */
    }
    f->port = len >= REPL_ID_LEN + 10 ? (int)(req[REPL_ID_LEN + 8] | (req[REPL_ID_LEN + 9] << 8)) : 0;
    fs->f = f;
    memcpy(fs->rid, req, REPL_ID_LEN);
    fs->roff = get64(req + REPL_ID_LEN);
    pthread_mutex_lock(&r->m);
    reap(r);
    f->id = ++r->next_feed;
    f->next = r->feeds;
    r->feeds = f;
    int rc = pthread_create(&f->th, NULL, feed_run, fs) == 0 ? 0 : -EAGAIN;
    if (rc != 0) {
        r->feeds = f->next;
        free(f);
        free(fs);
    }
    pthread_mutex_unlock(&r->m);
    return rc;
}

size_t repl_acked(struct repl *r, uint64_t off) {
    size_t n = 0;
    pthread_mutex_lock(&r->m);
    for (struct feed *f = r->feeds; f != NULL; f = f->next) {
        n += !atomic_load(&f->done) && atomic_load(&f->online) && atomic_load(&f->acked) >= off;
    }
    pthread_mutex_unlock(&r->m);
    return n;
}

/* ---- replica side -------------------------------------------------------------------------------------- */

struct apply_ctx {
    akvd *d;
    /* records of open transactions: 8-byte id, then the records (AOF encoding) */
    struct obuf held;
    int err;
};

static int apply_one(void *arg, const akv_change *c) {
    struct apply_ctx *a = arg;
    akv *s = akvd_store(a->d);
    if (c->kind == (enum akv_change_kind)AOF_TXN_END) {
        /* Apply the held records of this transaction with every shard locked. */
        akv_lockset set;
        akv_lock_all(s, c->txn, &set);
        size_t w = 0;
        for (size_t off = 0; off < a->held.len;) {
            uint64_t txn;
            memcpy(&txn, a->held.p + off, 8);
            size_t sz = aof_record_size(a->held.p + off + 8);
            if (txn == c->txn) {
                akv_change r;
                if (aof_record_decode(a->held.p + off + 8, &r) == 0) {
                    akv_apply(s, &r);
                }
            } else {
                memmove(a->held.p + w, a->held.p + off, 8 + sz);
                w += 8 + sz;
            }
            off += 8 + sz;
        }
        a->held.len = w;
        akvd_txn_end(a->d, c->txn);
        akv_unlock(s, &set);
        return 0;
    }
    if (c->txn != 0) {
        uint8_t id[8];
        memcpy(id, &c->txn, 8);
        if (ob_add(&a->held, id, 8) != 0 || aof_record_encode(&a->held, c) != 0) {
            return -ENOMEM;
        }
        return 0;
    }
    akv_apply(s, c);
    return 0;
}

static int send_req(duct_ep *ep, uint8_t op, const void *val, size_t vlen, int timeout_ms, uint8_t *status) {
    uint8_t buf[512];
    if (AKVP_REQ_HDR + vlen > sizeof(buf)) {
        return -E2BIG;
    }
    struct akvp_req rq = {.op = op, .reqid = 1, .vlen = (uint32_t)vlen};
    akvp_req_encode(buf, &rq);
    memcpy(buf + AKVP_REQ_HDR, val, vlen);
    int rc = duct_send(ep, buf, AKVP_REQ_HDR + vlen, timeout_ms);
    if (rc != 0 || status == NULL) {
        return rc;
    }
    struct duct_rx rx;
    rc = duct_recv(ep, &rx, timeout_ms);
    if (rc != 0) {
        return rc;
    }
    struct akvp_resp rs = {.status = AKVP_ERROR};
    rc = akvp_resp_decode(rx.data, rx.len, &rs) == 0 ? 0 : DUCT_EPROTO;
    *status = rc == 0 ? rs.status : AKVP_ERROR;
    duct_rx_release(ep, &rx);
    return rc;
}

static void *link_main(void *arg) {
    struct repl *r = arg;
    akvd *d = r->d;
    akv *s = akvd_store(d);
    on_link = 1;
    while (!atomic_load(&r->stop)) {
        duct_ep *ep = NULL;
        int rc = duct_connect(r->ctx, r->host, r->port, &ep);
        if (rc != 0) {
            akvd_log(d, AKVD_LOG_WARN, "replication: cannot connect to %s port %s: %s", r->host, r->port,
                     duct_strerror(rc));
        }
        uint8_t status = AKVP_OK;
        const akvd_config *cfg = akvd_cfg(d);
        if (rc == 0 && cfg->primary_auth != NULL) {
            rc = send_req(ep, AKVP_AUTH, cfg->primary_auth, strlen(cfg->primary_auth), 10000, &status);
            if (rc == 0 && status != AKVP_OK) {
                akvd_log(d, AKVD_LOG_ERROR, "replication: the primary refused the credentials");
                rc = DUCT_EPROTO;
            }
        }
        if (rc == 0) {
            uint8_t req[REPL_ID_LEN + 10];
            char id2[REPL_ID_LEN];
            uint64_t second;
            repl_backlog_ids(r->b, (char *)req, id2, &second);
            put64(req + REPL_ID_LEN, repl_backlog_end(r->b));
            int lp = akvd_port(d);
            req[REPL_ID_LEN + 8] = (uint8_t)lp;
            req[REPL_ID_LEN + 9] = (uint8_t)(lp >> 8);
            rc = send_req(ep, AKVP_REPLSYNC, req, sizeof(req), 10000, NULL);
        }
        repl_parser parser = {0};
        struct apply_ctx actx = {.d = d};
        int64_t last_ack = 0;
        uint64_t acked = 0;
        atomic_store(&r->last_io, mono_s());
        while (rc == 0 && !atomic_load(&r->stop)) {
            struct duct_rx rx;
            rc = duct_recv(ep, &rx, 1000);
            if (rc == -ETIMEDOUT) {
                rc = mono_s() - atomic_load(&r->last_io) > 60 ? -ETIMEDOUT : 0;
            } else if (rc == 0) {
                atomic_store(&r->last_io, mono_s());
                const uint8_t *p = rx.data;
                size_t n = rx.len;
                if (n >= 1 + REPL_ID_LEN + 8 && p[0] == REPL_FULL) {
                    akv_flush(s);
                    repl_parser_free(&parser);
                    actx.held.len = 0;
                    repl_backlog_reset(r->b, (const char *)p + 1, get64(p + 1 + REPL_ID_LEN));
                    akvd_log(d, AKVD_LOG_INFO, "replication: full copy from %s port %s", r->host, r->port);
                } else if (n >= 1 + REPL_ID_LEN && p[0] == REPL_CONTINUE) {
                    repl_backlog_switch_id(r->b, (const char *)p + 1);
                    atomic_store(&r->linked, 1);
                    akvd_log(d, AKVD_LOG_INFO, "replication: continuing from %s port %s", r->host, r->port);
                } else if (n >= 1 && p[0] == REPL_BASE) {
                    rc = repl_parse(&parser, p + 1, n - 1, apply_one, &actx) == 0 ? 0 : DUCT_EPROTO;
                } else if (n == 1 && p[0] == REPL_BASE_END) {
                    rc = parser.len == 0 ? 0 : DUCT_EPROTO;
                    atomic_store(&r->linked, 1);
                    akvd_log(d, AKVD_LOG_INFO, "replication: copy complete, streaming");
                } else if (n >= 9 && p[0] == REPL_STREAM) {
                    if (get64(p + 1) != repl_backlog_end(r->b)) {
                        rc = DUCT_EPROTO; /* a gap: resynchronize */
                    } else {
                        repl_backlog_append_raw(r->b, p + 9, n - 9);
                        rc = repl_parse(&parser, p + 9, n - 9, apply_one, &actx) == 0 ? 0 : DUCT_EPROTO;
                    }
                } else if (n == 9 && p[0] == REPL_PING) {
                    /* keep-alive */
                } else {
                    rc = DUCT_EPROTO;
                }
                duct_rx_release(ep, &rx);
            }
            /* The applied offset: the stream end minus the bytes of an incomplete record. */
            uint64_t applied = repl_backlog_end(r->b) - parser.len;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (rc == 0 && atomic_load(&r->linked) && (applied != acked || now_ms - last_ack >= 1000) &&
                now_ms - last_ack >= 20) {
                uint8_t ack[9] = {REPL_ACK};
                put64(ack + 1, applied);
                rc = duct_send(ep, ack, sizeof(ack), 10000);
                acked = applied;
                last_ack = now_ms;
            }
        }
        atomic_store(&r->linked, 0);
        repl_parser_free(&parser);
        ob_free(&actx.held);
        duct_ep_close(ep);
        if (!atomic_load(&r->stop)) {
            if (rc != 0 && rc != -ETIMEDOUT) {
                akvd_log(d, AKVD_LOG_WARN, "replication: link to %s port %s lost (%s); reconnecting", r->host,
                         r->port, duct_strerror(rc));
            }
            for (int i = 0; i < 10 && !atomic_load(&r->stop); i++) {
                struct timespec ts = {.tv_nsec = 100000000};
                nanosleep(&ts, NULL);
            }
        }
    }
    return NULL;
}

/* ---- control ------------------------------------------------------------------------------------------- */

struct repl *repl_create(akvd *d, size_t backlog) {
    struct repl *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->d = d;
    r->b = repl_backlog_create(backlog);
    if (r->b == NULL) {
        free(r);
        return NULL;
    }
    pthread_mutex_init(&r->m, NULL);
    return r;
}

repl_backlog *repl_backlog_of(struct repl *r) {
    return r->b;
}

static void stop_link(struct repl *r) {
    if (r->link_started) {
        atomic_store(&r->stop, 1);
        pthread_join(r->link, NULL);
        r->link_started = 0;
        atomic_store(&r->stop, 0);
    }
}

int repl_follow(struct repl *r, const char *host, const char *port) {
    akvd *d = r->d;
    pthread_mutex_lock(&r->m);
    if (host == NULL) {
        int was = r->replica;
        pthread_mutex_unlock(&r->m);
        stop_link(r);
        pthread_mutex_lock(&r->m);
        if (was) {
            r->replica = 0;
            repl_backlog_promote(r->b);
            akv_set_follower(akvd_store(d), 0);
            akvd_log(d, AKVD_LOG_INFO, "replication: now a primary (REPLICAOF NO ONE)");
        }
        pthread_mutex_unlock(&r->m);
        return 0;
    }
    if (strlen(host) >= sizeof(r->host) || strlen(port) >= sizeof(r->port)) {
        pthread_mutex_unlock(&r->m);
        return -EINVAL;
    }
    pthread_mutex_unlock(&r->m);
    stop_link(r);
    pthread_mutex_lock(&r->m);
    snprintf(r->host, sizeof(r->host), "%s", host);
    snprintf(r->port, sizeof(r->port), "%s", port);
    int rc = 0;
    if (r->ctx == NULL) {
        struct duct_opts o;
        akvd_duct_opts(d, &o);
        rc = duct_ctx_create(&o, &r->ctx);
    }
    if (rc == 0) {
        r->replica = 1;
        akv_set_follower(akvd_store(d), 1);
        rc = pthread_create(&r->link, NULL, link_main, r) == 0 ? 0 : -EAGAIN;
        r->link_started = rc == 0;
        akvd_log(d, AKVD_LOG_INFO, "replication: following %s port %s", host, port);
    }
    pthread_mutex_unlock(&r->m);
    return rc;
}

int repl_is_replica(struct repl *r) {
    return r != NULL && __atomic_load_n(&r->replica, __ATOMIC_RELAXED);
}

void repl_destroy(struct repl *r) {
    if (r == NULL) {
        return;
    }
    stop_link(r);
    pthread_mutex_lock(&r->m);
    for (struct feed *f = r->feeds; f != NULL; f = f->next) {
        atomic_store(&f->done, 1);
    }
    pthread_mutex_unlock(&r->m);
    for (struct feed *f = r->feeds; f != NULL;) {
        struct feed *next = f->next;
        pthread_join(f->th, NULL);
        free(f);
        f = next;
    }
    duct_ctx_destroy(r->ctx);
    repl_backlog_destroy(r->b);
    pthread_mutex_destroy(&r->m);
    free(r);
}

void repl_info(struct repl *r, struct obuf *o) {
    char id[REPL_ID_LEN + 1] = {0};
    char id2[REPL_ID_LEN + 1] = {0};
    uint64_t second;
    repl_backlog_ids(r->b, id, id2, &second);
    uint64_t first;
    uint64_t held;
    repl_backlog_range(r->b, &first, &held);
    uint64_t end = repl_backlog_end(r->b);
    pthread_mutex_lock(&r->m);
    reap(r);
    if (r->replica) {
        ob_printf(o,
                  "role:slave\r\nmaster_host:%s\r\nmaster_port:%s\r\nmaster_link_status:%s\r\n"
                  "master_last_io_seconds_ago:%lld\r\nslave_repl_offset:%llu\r\nslave_read_only:%d\r\n",
                  r->host, r->port, atomic_load(&r->linked) ? "up" : "down",
                  (long long)(mono_s() - atomic_load(&r->last_io)), (unsigned long long)(end - 1),
                  akvd_cfg(r->d)->replica_read_only);
    } else {
        ob_printf(o, "role:master\r\n");
    }
    unsigned n = 0;
    for (struct feed *f = r->feeds; f != NULL; f = f->next) {
        n += !atomic_load(&f->done);
    }
    ob_printf(o, "connected_slaves:%u\r\n", n);
    unsigned i = 0;
    for (struct feed *f = r->feeds; f != NULL; f = f->next) {
        if (!atomic_load(&f->done)) {
            ob_printf(o, "slave%u:ip=%s,port=%d,state=%s,offset=%llu,lag=%llu\r\n", i++, f->addr, f->port,
                      atomic_load(&f->online) ? "online" : "wait_bgsave",
                      (unsigned long long)atomic_load(&f->acked) - 1,
                      (unsigned long long)(end - atomic_load(&f->acked)));
        }
    }
    pthread_mutex_unlock(&r->m);
    ob_printf(
        o,
        "master_replid:%s\r\nmaster_replid2:%s\r\nmaster_repl_offset:%llu\r\nsecond_repl_offset:%lld\r\n"
        "repl_backlog_active:1\r\nrepl_backlog_size:%zu\r\nrepl_backlog_first_byte_offset:%llu\r\n"
        "repl_backlog_histlen:%llu\r\n",
        id, id2, (unsigned long long)(end - 1), second ? (long long)second - 1 : -1LL,
        akvd_cfg(r->d)->repl_backlog_size, (unsigned long long)first, (unsigned long long)held);
}
