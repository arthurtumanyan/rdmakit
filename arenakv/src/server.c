/*
 * server.c - key-value server core, see akvd.h.
 *
 * Threads:
 *   accept   waits in poll() on all listeners; accepts, applies max_conns, and queues each connection
 *            to a worker (round robin, an eventfd wakes the worker)
 *   workers  one epoll set each; every connection belongs to one worker for its lifetime
 *   saver    periodic snapshots (save_interval_s); background snapshots run on their own thread
 *
 * Native connections (rdmaduct): the worker drains messages with duct_recv(..., 0), executes each
 * request into the worker's batch area and sends up to BATCH_MSGS responses with one duct_send_some(),
 * so pipelined requests cost one send call per batch instead of one per response (on TCP one sendmsg).
 * Responses that the endpoint does not accept (no credit or send space) are queued in conn.pend and the
 * connection stops reading until the queue is empty, so a client that does not read its responses cannot
 * make the server buffer without bound. The worker waits on duct_ep_direct_fd() (on TCP the socket
 * itself, without the endpoint's nested epoll set); the connection is armed with duct_ep_arm() before the
 * worker waits, and its registration follows duct_ep_wait_events().
 *
 * Text connections (memcached, RESP): non-blocking sockets (optionally TLS). Input accumulates in
 * conn.in until the protocol module consumes complete commands; replies accumulate in conn.out. Reading
 * stops while conn.out holds more than OB_SOFT_LIMIT bytes. An input buffer that reaches the maximum
 * command size without a complete command closes the connection.
 *
 * Fairness: a connection is served for at most BURST requests or reads per turn. A connection that still
 * has work afterwards (messages already received by rdmaduct, complete commands in its input buffer, or
 * TLS-buffered input, none of which raise a descriptor event) is put on the worker's ready list and served
 * again in the next loop iteration, which then polls epoll without waiting. A connection closed while on
 * the ready list is released when the list is processed.
 *
 * Request handling of the native protocol: see akvproto.h. Stores answer with the new CAS version as an
 * 8-byte payload.
 *
 * Publish/subscribe (pubsub.h): a PUBLISH, on any worker, appends each frame to the inbox of the worker
 * that serves the subscriber and wakes it through its eventfd when the inbox was empty. The worker moves
 * the frames to the subscribers' output buffers, finding each connection by id in its subscriber map, and
 * disconnects a subscriber whose pending output exceeds pubsub_output_limit.
 *
 * Replication (repl.h, replication.c): the change feed also goes to the replication backlog once
 * replication is in use (a replica connected, or the server follows a primary). An AKVP_REPLSYNC request
 * hands the native endpoint over to a feed thread: the worker sends the responses before it, removes the
 * endpoint from its epoll set and forgets the connection without closing the endpoint. A read-only
 * replica refuses client writes in every protocol. RESP WAIT parks its connection until enough replicas
 * acknowledged the offset of the worker's last change, or its timeout; feeds wake the workers.
 *
 * Durability: the engine's change feed goes to the append-only log (aof.h). With fsync policy "always" a
 * connection whose commands changed data in a turn is parked: its replies stay queued and its descriptor
 * is removed from the epoll interest until the log's durable sequence number reaches the last record the
 * turn produced. The log writer syncs a batch of records at once (group commit) and then wakes the
 * workers that hold parked connections through their eventfd. Parking and waking pair a sequentially
 * consistent store of the parking flag with a load of the durable number (and the reverse on the writer
 * side), so a wake-up cannot be missed.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef AKV_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include "akvproto.h"
#include "akvd.h"
#include "srv.h"

#define READ_CHUNK 65536u
#define BURST 64               /* requests or reads per connection before other connections are served */
#define BATCH_MSGS 64          /* native responses sent with one duct_send_batch(): one turn (BURST) */
#define BATCH_BYTES (1u << 20) /* size of the native batch area, at least max_msg */
#define PREFETCH 8             /* native requests whose index buckets are prefetched together */

enum conn_kind {
    CK_NATIVE = 0,
    CK_MC = 1,
    CK_RESP = 2,
};

struct worker;
struct conn;

static void schedule(struct worker *w, struct conn *c);

struct conn {
    enum conn_kind kind;
    struct worker *w;
    struct conn *prev;
    struct conn *next;
    uint64_t last_active; /* seconds of CLOCK_MONOTONIC_COARSE */
    struct session ss;
    /* native */
    duct_ep *ep;
    struct obuf pend; /* queued responses: 4-byte length, then the message */
    /* text: the socket; native: the descriptor of duct_ep_direct_fd(), owned by the endpoint */
    int fd;
    uint32_t epmask; /* events registered for fd in the worker's epoll set */
    struct obuf in;
    struct obuf out;
#ifdef AKV_HAVE_TLS
    SSL *ssl;
#endif
    int tls_want_write; /* the TLS layer needs the socket writable to progress */
    int in_ready;       /* on the worker's ready list */
    int dead;           /* resources released; freed when it leaves the ready list */
    int parked;         /* replies wait for the append-only log (see above) */
    uint64_t hold_seq;  /* log sequence number the replies wait for */
};

struct worker {
    akvd *d;
    pthread_t thread;
    int epfd;
    int evfd;
    pthread_mutex_t qlock;
    struct conn *queue; /* connections handed over by the accept thread */
    struct conn *conns; /* connections served */
    uint8_t *resp;      /* native response batch area of resp_cap bytes, responses packed in order */
    size_t resp_cap;    /* max(BATCH_BYTES, max_msg) */
    struct duct_msg bmsg[BATCH_MSGS]; /* responses of the current batch, stored in resp */
    struct conn **ready;              /* connections with work left after their turn */
    size_t nready;
    size_t ready_cap;
    struct conn **parked; /* connections waiting for the append-only log */
    size_t nparked;
    size_t parked_cap;
    atomic_int has_parked;
    unsigned index;
    /* pub/sub: frames for subscribers (8-byte connection id, 4-byte length, frame), and the subscribers */
    pthread_mutex_t inbox_lock;
    struct obuf inbox;
    struct conn **subs; /* open addressing by connection id; NULL free, SUB_GONE removed */
    size_t subs_cap;
    size_t subs_used;
    /* RESP WAIT: connections waiting for replica acknowledgements */
    struct conn **waits;
    size_t nwaits;
    size_t waits_cap;
    atomic_int has_waits;
};

struct akvd {
    akvd_config cfg;
    akv *store;
    duct_ctx *ctx;
    duct_listener *nl;
    int mc_fd;
    int resp_fd;
    int tls_mc_fd;
    int tls_resp_fd;
    int ports[5]; /* native, memcached, resp, tls memcached, tls resp */
    pthread_t accept_thread;
    int accept_started;
    int stop_fd;
    atomic_int stopping;
    struct worker *workers;
    unsigned nworkers;
    unsigned next_worker;
    struct srv_counters counters;
    atomic_ullong next_id;
    atomic_ullong next_txn;
    /* access control: the current list, older lists (still referenced by sessions), the generation */
    _Atomic(acl *) acl;
    acl **retired;
    size_t nretired;
    atomic_ullong acl_gen;
    pthread_mutex_t acl_lock;
    pthread_mutex_t log_lock;
    FILE *log;
    struct timespec started;
    /* persistence */
    pthread_mutex_t save_lock;
    pthread_t bg_thread;
    atomic_int bg_running;
    int bg_joinable;
    atomic_ullong lastsave;
    atomic_int last_ok;
    pthread_t saver;
    int saver_started;
    pthread_mutex_t saver_lock;
    pthread_cond_t saver_cond;
    uint64_t saved_puts;
    aof *aof; /* append-only log, NULL when not configured */
    pubsub *ps;
    struct repl *repl;
    atomic_int repl_active; /* the backlog records changes */
    _Atomic(cluster *) cluster;
    cluster **old_clusters; /* replaced maps, still referenced by running commands; freed at stop */
    size_t nold_clusters;
#ifdef AKV_HAVE_TLS
    SSL_CTX *tls;
#endif
};

/* ---- output buffers ---------------------------------------------------------------------------------- */

static int ob_grow(struct obuf *b, size_t need) {
    if (b->len + need <= b->cap) {
        return 0;
    }
    if (b->off > 0) {
        memmove(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
        if (b->len + need <= b->cap) {
            return 0;
        }
    }
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + need) {
        cap *= 2;
    }
    uint8_t *p = realloc(b->p, cap);
    if (p == NULL) {
        return -ENOMEM;
    }
    b->p = p;
    b->cap = cap;
    return 0;
}

int ob_add(struct obuf *b, const void *data, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (ob_grow(b, n) != 0) {
        return -ENOMEM;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return 0;
}

int ob_printf(struct obuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[256];
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -EINVAL;
    }
    if ((size_t)n < sizeof(small)) {
        return ob_add(b, small, (size_t)n);
    }
    if (ob_grow(b, (size_t)n + 1) != 0) {
        return -ENOMEM;
    }
    va_start(ap, fmt);
    vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

uint8_t *ob_reserve(struct obuf *b, size_t n) {
    return ob_grow(b, n) == 0 ? b->p + b->len : NULL;
}

void ob_commit(struct obuf *b, size_t n) {
    b->len += n;
}

void ob_compact(struct obuf *b) {
    if (b->off == b->len) {
        b->off = 0;
        b->len = 0;
        if (b->cap > (64u << 10)) {
            free(b->p);
            b->p = NULL;
            b->cap = 0;
        }
    }
}

void ob_free(struct obuf *b) {
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/* ---- small accessors used by the protocol modules ----------------------------------------------------- */

struct srv_counters *akvd_counters(akvd *d) {
    return &d->counters;
}

int akvd_acl_enabled(const akvd *d) {
    return atomic_load(&((akvd *)d)->acl) != NULL;
}

int akvd_auth_required(const akvd *d) {
    acl *a = atomic_load(&((akvd *)d)->acl);
    return a != NULL && acl_default_login(a) == NULL;
}

uint64_t akvd_acl_gen(const akvd *d) {
    return atomic_load_explicit(&((akvd *)d)->acl_gen, memory_order_acquire);
}

const acl *akvd_acl(akvd *d) {
    return atomic_load(&d->acl);
}

void akvd_session_init(akvd *d, struct session *ss) {
    acl *a = atomic_load(&d->acl);
    ss->acl_gen = akvd_acl_gen(d);
    ss->user = a != NULL ? acl_default_login(a) : NULL;
    ss->authed = a == NULL || ss->user != NULL;
}

int akvd_login(akvd *d, struct session *ss, const char *user, size_t ulen, const char *pass, size_t plen) {
    uint64_t gen = akvd_acl_gen(d);
    acl *a = atomic_load(&d->acl);
    const acl_user *u = a != NULL ? acl_login(a, user, ulen, pass, plen) : NULL;
    if (u == NULL) {
        SRV_COUNT(d, auth_failures, 1);
        return 0;
    }
    ss->user = u;
    ss->acl_gen = gen;
    ss->authed = 1;
    return 1;
}

int akvd_session_reresolve(akvd *d, struct session *ss) {
    uint64_t gen = akvd_acl_gen(d);
    acl *a = atomic_load(&d->acl);
    const char *name = acl_user_name(ss->user);
    const acl_user *u = acl_find(a, name, strlen(name));
    ss->acl_gen = gen;
    if (u == NULL || !acl_user_enabled(u)) {
        akvd_log(d, AKVD_LOG_INFO, "closing connection %llu: user %s was removed or disabled",
                 (unsigned long long)ss->id, name);
        ss->user = NULL;
        ss->authed = 0;
        ss->close = 1;
        return -1;
    }
    ss->user = u;
    return 0;
}

int akvd_acl_reload(akvd *d, char *err, size_t errlen) {
    if (d->cfg.acl_file == NULL) {
        snprintf(err, errlen, "no ACL file is configured (aclfile option)");
        return -EINVAL;
    }
    acl *n = acl_load(d->cfg.acl_file, 0, err, errlen);
    if (n == NULL) {
        return -EBADMSG;
    }
    pthread_mutex_lock(&d->acl_lock);
    acl **r = realloc(d->retired, (d->nretired + 1) * sizeof(*r));
    if (r == NULL) {
        pthread_mutex_unlock(&d->acl_lock);
        acl_free(n);
        snprintf(err, errlen, "out of memory");
        return -ENOMEM;
    }
    d->retired = r;
    d->retired[d->nretired++] = atomic_load(&d->acl);
    atomic_store(&d->acl, n);
    atomic_fetch_add_explicit(&d->acl_gen, 1, memory_order_release);
    pthread_mutex_unlock(&d->acl_lock);
    akvd_log(d, AKVD_LOG_INFO, "access control list reloaded: %zu users", acl_nusers(n));
    return 0;
}

int akvd_no_evict(const akvd *d) {
    return d->cfg.store.no_evict != 0;
}

uint32_t akvd_max_entry(const akvd *d) {
    return d->cfg.store.max_entry ? d->cfg.store.max_entry : (1u << 20);
}

size_t akvd_max_input(const akvd *d) {
    return 2 * (size_t)akvd_max_entry(d) + (1u << 20);
}

uint64_t akvd_uptime(const akvd *d) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)(now.tv_sec - d->started.tv_sec);
}

int akvd_aof_info(akvd *d, aof_info *out) {
    if (d->aof == NULL) {
        return -EINVAL;
    }
    aof_info_get(d->aof, out);
    return 0;
}

uint64_t akvd_next_txn(akvd *d) {
    return atomic_fetch_add(&d->next_txn, 1) + 1;
}

void akvd_txn_end(akvd *d, uint64_t txn) {
    if (d->aof != NULL) {
        aof_append_txn_end(d->aof, txn);
    }
    if (atomic_load_explicit(&d->repl_active, memory_order_relaxed) && !repl_link_thread()) {
        akv_change c = {.kind = (enum akv_change_kind)AOF_TXN_END, .txn = txn};
        repl_backlog_append(repl_backlog_of(d->repl), &c);
    }
}

const cluster *akvd_cluster(akvd *d) {
    return atomic_load(&d->cluster);
}

int akvd_cluster_reload(akvd *d, char *err, size_t errlen) {
    if (d->cfg.cluster_config == NULL) {
        snprintf(err, errlen, "cluster mode is not configured (cluster-config option)");
        return -EINVAL;
    }
    cluster *n = cluster_load(d->cfg.cluster_config, d->cfg.cluster_node_id, err, errlen);
    if (n == NULL) {
        return -EBADMSG;
    }
    pthread_mutex_lock(&d->acl_lock);
    cluster **r = realloc(d->old_clusters, (d->nold_clusters + 1) * sizeof(*r));
    if (r == NULL) {
        pthread_mutex_unlock(&d->acl_lock);
        cluster_free(n);
        snprintf(err, errlen, "out of memory");
        return -ENOMEM;
    }
    d->old_clusters = r;
    d->old_clusters[d->nold_clusters++] = atomic_exchange(&d->cluster, n);
    pthread_mutex_unlock(&d->acl_lock);
    akvd_log(d, AKVD_LOG_INFO, "cluster map reloaded: %u nodes, %u slots assigned", n->n,
             cluster_assigned(n));
    return 0;
}

const akvd_config *akvd_cfg(const akvd *d) {
    return &d->cfg;
}

void akvd_duct_opts(const akvd *d, struct duct_opts *o) {
    duct_opts_default(o);
    o->transport = d->cfg.transport;
    o->wait = d->cfg.wait;
    o->max_msg = d->cfg.max_msg;
    o->recv_depth = d->cfg.recv_depth;
    o->send_depth = d->cfg.send_depth;
    if (d->cfg.native_tls) {
        o->tls = 1;
        o->tls_cert = d->cfg.tls_cert;
        o->tls_key = d->cfg.tls_key;
        o->tls_ca = d->cfg.tls_ca;
    }
}

struct repl *akvd_repl(akvd *d) {
    return d->repl;
}

int akvd_read_only(const akvd *d) {
    return d->cfg.replica_read_only && repl_is_replica(d->repl);
}

void akvd_repl_activate(akvd *d) {
    atomic_store(&d->repl_active, 1);
}

void akvd_wake_waiters(akvd *d) {
    for (unsigned i = 0; i < d->nworkers; i++) {
        if (atomic_load(&d->workers[i].has_waits)) {
            uint64_t one = 1;
            if (write(d->workers[i].evfd, &one, sizeof(one)) < 0) {
                /* the worker checks its waits at least every 100 ms */
            }
        }
    }
}

const char *akvd_appendfsync(const akvd *d) {
    static const char *const names[] = {"no", "everysec", "always"};
    return d->cfg.aof_fsync <= AOF_FSYNC_ALWAYS ? names[d->cfg.aof_fsync] : "everysec";
}

void akvd_warm_info(akvd *d, akv_warm_info *out) {
    akv_warm_get(d->store, out);
}

int akvd_aof_rewrite(akvd *d) {
    return d->aof == NULL ? -EINVAL : aof_rewrite(d->aof);
}

unsigned akvd_threads(const akvd *d) {
    return d->nworkers;
}

akv *akvd_store(akvd *d) {
    return d->store;
}

unsigned akvd_connections(const akvd *d) {
    return __atomic_load_n(&d->counters.curr_connections, __ATOMIC_RELAXED);
}

int akvd_port(const akvd *d) {
    return d->ports[0];
}

int akvd_memcached_port(const akvd *d) {
    return d->ports[1];
}

int akvd_resp_port(const akvd *d) {
    return d->ports[2];
}

int akvd_tls_memcached_port(const akvd *d) {
    return d->ports[3];
}

int akvd_tls_resp_port(const akvd *d) {
    return d->ports[4];
}

/* Thread-local value buffer of srv_fetch(). */
static __thread uint8_t *tl_val;
static __thread size_t tl_cap;

void srv_thread_exit(void) {
    free(tl_val);
    tl_val = NULL;
    tl_cap = 0;
}

int srv_fetch(akvd *d, const char *key, size_t klen, akv_meta *m, int64_t touch_ttl_ms, const uint8_t **val,
              size_t *vlen) {
    for (;;) {
        int rc = akv_get_meta(d->store, key, klen, tl_val, tl_cap, vlen, m, touch_ttl_ms);
        if (rc != -EMSGSIZE) {
            *val = tl_val;
            return rc;
        }
        size_t cap = *vlen < 4096 ? 4096 : *vlen;
        uint8_t *p = realloc(tl_val, cap);
        if (p == NULL) {
            return -ENOMEM;
        }
        tl_val = p;
        tl_cap = cap;
        touch_ttl_ms = AKV_TTL_KEEP; /* the first call already applied the touch */
    }
}

/* ---- logging ------------------------------------------------------------------------------------------ */

void akvd_log(akvd *d, enum akvd_log_level level, const char *fmt, ...) {
    /* The NULL test also tells the compiler that fmt is valid on the vfprintf path (GCC reports a null
     * format otherwise when the undefined-behavior sanitizer instruments the call). */
    if (level > d->cfg.log_level || fmt == NULL) {
        return;
    }
    static const char *names[] = {"error", "warn", "info", "debug"};
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
    pthread_mutex_lock(&d->log_lock);
    FILE *f = d->log != NULL ? d->log : stderr;
    fprintf(f, "%s [%s] ", when, names[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    pthread_mutex_unlock(&d->log_lock);
}

void akvd_reopen_log(akvd *d) {
    if (d->cfg.log_path == NULL) {
        return;
    }
    pthread_mutex_lock(&d->log_lock);
    FILE *f = fopen(d->cfg.log_path, "a");
    if (f != NULL) {
        if (d->log != NULL) {
            fclose(d->log);
        }
        d->log = f;
    }
    pthread_mutex_unlock(&d->log_lock);
}

/* ---- persistence -------------------------------------------------------------------------------------- */

static int save_now(akvd *d) {
    if (d->cfg.snapshot_path == NULL) {
        return -EINVAL;
    }
    pthread_mutex_lock(&d->save_lock);
    akv_stats st;
    akv_stats_get(d->store, &st);
    uint64_t records = 0;
    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = snap_save(d->store, d->cfg.snapshot_path, &records);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc == 0) {
        atomic_store(&d->lastsave, (unsigned long long)time(NULL));
        atomic_store(&d->last_ok, 1);
        d->saved_puts = st.puts + st.dels;
        akvd_log(d, AKVD_LOG_INFO, "snapshot written: %llu entries in %.3f s", (unsigned long long)records,
                 (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    } else {
        atomic_store(&d->last_ok, 0);
        akvd_log(d, AKVD_LOG_ERROR, "snapshot failed: %s", strerror(-rc));
    }
    pthread_mutex_unlock(&d->save_lock);
    return rc;
}

int akvd_save(akvd *d) {
    return save_now(d);
}

static void *bg_main(void *arg) {
    akvd *d = arg;
    save_now(d);
    atomic_store(&d->bg_running, 0);
    return NULL;
}

int akvd_bgsave(akvd *d) {
    if (d->cfg.snapshot_path == NULL) {
        return -EINVAL;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&d->bg_running, &expected, 1)) {
        return -EBUSY;
    }
    pthread_mutex_lock(&d->saver_lock);
    if (d->bg_joinable) {
        pthread_join(d->bg_thread, NULL);
        d->bg_joinable = 0;
    }
    int rc = pthread_create(&d->bg_thread, NULL, bg_main, d);
    if (rc == 0) {
        d->bg_joinable = 1;
    } else {
        atomic_store(&d->bg_running, 0);
    }
    pthread_mutex_unlock(&d->saver_lock);
    return rc == 0 ? 0 : -rc;
}

int akvd_bgsave_running(const akvd *d) {
    return atomic_load(&((akvd *)d)->bg_running);
}

uint64_t akvd_lastsave(const akvd *d) {
    return atomic_load(&((akvd *)d)->lastsave);
}

int akvd_last_save_ok(const akvd *d) {
    return atomic_load(&((akvd *)d)->last_ok);
}

static void *saver_main(void *arg) {
    akvd *d = arg;
    pthread_mutex_lock(&d->saver_lock);
    while (!atomic_load(&d->stopping)) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += d->cfg.save_interval_s;
        pthread_cond_timedwait(&d->saver_cond, &d->saver_lock, &until);
        if (atomic_load(&d->stopping)) {
            break;
        }
        akv_stats st;
        akv_stats_get(d->store, &st);
        if (st.puts + st.dels != d->saved_puts) {
            pthread_mutex_unlock(&d->saver_lock);
            save_now(d);
            pthread_mutex_lock(&d->saver_lock);
        }
    }
    pthread_mutex_unlock(&d->saver_lock);
    return NULL;
}

/* ---- connections -------------------------------------------------------------------------------------- */

/* Monotonic seconds for idle accounting. The coarse clock is read without the timestamp counter
 * (clock_gettime(2)); its tick resolution is far below the one-second granularity used here. */
static uint64_t mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return (uint64_t)ts.tv_sec;
}

/* ---- pub/sub ------------------------------------------------------------------------------------------ */

static struct conn sub_gone_marker;
#define SUB_GONE (&sub_gone_marker)

static size_t sub_slot(uint64_t id, size_t cap) {
    return (size_t)((id * 0x9e3779b97f4a7c15ull) >> 32) & (cap - 1);
}

static struct conn *sub_find(struct worker *w, uint64_t id) {
    if (w->subs_cap == 0) {
        return NULL;
    }
    for (size_t i = sub_slot(id, w->subs_cap);; i = (i + 1) & (w->subs_cap - 1)) {
        struct conn *c = w->subs[i];
        if (c == NULL) {
            return NULL;
        }
        if (c != SUB_GONE && c->ss.id == id) {
            return c;
        }
    }
}

/* Adds c to the subscriber map of its worker (idempotent). Returns 0 or -ENOMEM. */
static int sub_add(struct worker *w, struct conn *c) {
    if (sub_find(w, c->ss.id) == c) {
        return 0;
    }
    if ((w->subs_used + 1) * 2 > w->subs_cap) {
        size_t cap = w->subs_cap ? w->subs_cap * 2 : 64;
        struct conn **tab = calloc(cap, sizeof(*tab));
        if (tab == NULL) {
            return -ENOMEM;
        }
        size_t used = 0;
        for (size_t i = 0; i < w->subs_cap; i++) {
            struct conn *o = w->subs[i];
            if (o != NULL && o != SUB_GONE) {
                size_t k = sub_slot(o->ss.id, cap);
                while (tab[k] != NULL) {
                    k = (k + 1) & (cap - 1);
                }
                tab[k] = o;
                used++;
            }
        }
        free(w->subs);
        w->subs = tab;
        w->subs_cap = cap;
        w->subs_used = used;
    }
    size_t k = sub_slot(c->ss.id, w->subs_cap);
    while (w->subs[k] != NULL && w->subs[k] != SUB_GONE) {
        k = (k + 1) & (w->subs_cap - 1);
    }
    if (w->subs[k] == NULL) {
        w->subs_used++;
    }
    w->subs[k] = c;
    return 0;
}

static void sub_del(struct worker *w, struct conn *c) {
    for (size_t i = 0; i < w->subs_cap; i++) {
        if (w->subs[i] == c) {
            w->subs[i] = SUB_GONE;
            return;
        }
    }
}

/* pubsub_deliver_fn: appends a frame to the inbox of worker wi and wakes it when the inbox was empty. */
static int deliver(void *arg, unsigned wi, uint64_t conn_id, const char *frame, size_t n) {
    akvd *d = arg;
    struct worker *w = &d->workers[wi];
    uint8_t h[12];
    memcpy(h, &conn_id, 8);
    uint32_t len = (uint32_t)n;
    memcpy(h + 8, &len, 4);
    pthread_mutex_lock(&w->inbox_lock);
    int was_empty = ob_pending(&w->inbox) == 0;
    int rc = ob_add(&w->inbox, h, sizeof(h)) == 0 && ob_add(&w->inbox, frame, n) == 0 ? 0 : -ENOMEM;
    pthread_mutex_unlock(&w->inbox_lock);
    if (rc == 0 && was_empty) {
        uint64_t one = 1;
        if (write(w->evfd, &one, sizeof(one)) < 0) {
            /* the counter cannot overflow; the next wake-up of the worker drains the inbox */
        }
    }
    return rc;
}

static void conn_free(struct conn *c);
static int text_write(struct conn *c);

/* Moves the frames of the inbox to the subscribers' output buffers. */
static void drain_inbox(struct worker *w) {
    pthread_mutex_lock(&w->inbox_lock);
    struct obuf in = w->inbox;
    w->inbox = (struct obuf){0};
    pthread_mutex_unlock(&w->inbox_lock);
    akvd *d = w->d;
    for (size_t off = in.off; off + 12 <= in.len;) {
        uint64_t id;
        uint32_t len;
        memcpy(&id, in.p + off, 8);
        memcpy(&len, in.p + off + 8, 4);
        struct conn *c = sub_find(w, id);
        if (c != NULL && !c->dead && ob_add(&c->out, in.p + off + 12, len) == 0) {
            if (ob_pending(&c->out) > d->cfg.pubsub_output_limit) {
                akvd_log(d, AKVD_LOG_WARN,
                         "closing subscriber %llu: more than %llu bytes of messages pending",
                         (unsigned long long)id, (unsigned long long)d->cfg.pubsub_output_limit);
                conn_free(c);
            } else if (!c->parked) {
                schedule(w, c); /* a parked connection sends its output when the log is durable */
            }
        }
        off += 12 + len;
    }
    ob_free(&in);
}

int akvd_subscribe(akvd *d, struct session *ss, const char *name, size_t n, int pattern) {
    struct conn *c = ss->conn;
    if (c == NULL) {
        return -ENOTSUP;
    }
    int r = pattern ? pubsub_psubscribe(d->ps, name, n, ss->id, c->w->index, ss->proto == 3)
                    : pubsub_subscribe(d->ps, name, n, ss->id, c->w->index, ss->proto == 3);
    if (r == 1) {
        uint8_t h[5] = {(uint8_t)pattern};
        uint32_t len = (uint32_t)n;
        memcpy(h + 1, &len, 4);
        if (sub_add(c->w, c) != 0 || ob_add(&ss->subs, h, 5) != 0 || ob_add(&ss->subs, name, n) != 0) {
            if (pattern) {
                pubsub_punsubscribe(d->ps, name, n, ss->id);
            } else {
                pubsub_unsubscribe(d->ps, name, n, ss->id);
            }
            return -ENOMEM;
        }
        ss->nsubs++;
    }
    return r;
}

int akvd_unsubscribe(akvd *d, struct session *ss, const char *name, size_t n, int pattern) {
    int r =
        pattern ? pubsub_punsubscribe(d->ps, name, n, ss->id) : pubsub_unsubscribe(d->ps, name, n, ss->id);
    for (size_t off = 0; r && off < ss->subs.len;) {
        uint32_t len;
        memcpy(&len, ss->subs.p + off + 1, 4);
        if (ss->subs.p[off] == (uint8_t)pattern && len == n && memcmp(ss->subs.p + off + 5, name, n) == 0) {
            memmove(ss->subs.p + off, ss->subs.p + off + 5 + len, ss->subs.len - off - 5 - len);
            ss->subs.len -= 5 + len;
            ss->nsubs--;
            break;
        }
        off += 5 + len;
    }
    if (ss->nsubs == 0 && ss->conn != NULL) {
        sub_del(((struct conn *)ss->conn)->w, ss->conn);
    }
    return r;
}

void akvd_unsubscribe_all(akvd *d, struct session *ss) {
    for (size_t off = 0; off < ss->subs.len;) {
        uint32_t len;
        memcpy(&len, ss->subs.p + off + 1, 4);
        const char *name = (const char *)ss->subs.p + off + 5;
        if (ss->subs.p[off]) {
            pubsub_punsubscribe(d->ps, name, len, ss->id);
        } else {
            pubsub_unsubscribe(d->ps, name, len, ss->id);
        }
        off += 5 + len;
    }
    ob_free(&ss->subs);
    ss->nsubs = 0;
    if (ss->conn != NULL) {
        sub_del(((struct conn *)ss->conn)->w, ss->conn);
    }
}

size_t akvd_publish(akvd *d, const char *ch, size_t cn, const char *msg, size_t mn) {
    return pubsub_publish(d->ps, ch, cn, msg, mn);
}

pubsub *akvd_pubsub(akvd *d) {
    return d->ps;
}

static void unpark_remove(struct worker *w, struct conn *c) {
    for (size_t i = 0; i < w->nparked; i++) {
        if (w->parked[i] == c) {
            w->parked[i] = w->parked[--w->nparked];
            break;
        }
    }
    c->parked = 0;
}

static void wait_remove(struct worker *w, struct conn *c);

static void conn_free(struct conn *c) {
    struct worker *w = c->w;
    akvd *d = w->d;
    if (c->parked) {
        unpark_remove(w, c);
    }
    if (c->ss.waiting) {
        wait_remove(w, c);
    }
    if (c->ss.nsubs > 0) {
        akvd_unsubscribe_all(d, &c->ss); /* before the session's buffers, which list the subscriptions */
    }
    resp_session_free(&c->ss);
    if (c->kind == CK_NATIVE) {
        if (c->ep != NULL) {
            epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
            duct_ep_close(c->ep);
        }
        ob_free(&c->pend);
    } else {
        if (c->fd >= 0) {
            epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
#ifdef AKV_HAVE_TLS
            if (c->ssl != NULL) {
                SSL_shutdown(c->ssl);
                SSL_free(c->ssl);
            }
#endif
            close(c->fd);
        }
        ob_free(&c->in);
        ob_free(&c->out);
    }
    if (w->conns == c) {
        w->conns = c->next;
    } else if (c->prev != NULL) {
        c->prev->next = c->next;
    }
    if (c->next != NULL) {
        c->next->prev = c->prev;
    }
    __atomic_fetch_sub(&d->counters.curr_connections, 1, __ATOMIC_RELAXED);
    if (c->in_ready) {
        c->dead = 1; /* still referenced by the ready list */
        c->prev = c->next = NULL;
        return;
    }
    free(c);
}

/* Engine store mode of a native store operation. */
static enum akv_mode store_mode(uint8_t op) {
    switch (op) {
    case AKVP_ADD:
        return AKV_ADD;
    case AKVP_REPLACE:
        return AKV_REPLACE;
    case AKVP_APPEND:
        return AKV_APPEND;
    case AKVP_PREPEND:
        return AKV_PREPEND;
    case AKVP_CAS:
        return AKV_CAS;
    default:
        return AKV_SET;
    }
}

/* 1 for native operations that change data. */
static int native_writes(uint8_t op) {
    switch (op) {
    case AKVP_PUT:
    case AKVP_DEL:
    case AKVP_ADD:
    case AKVP_REPLACE:
    case AKVP_APPEND:
    case AKVP_PREPEND:
    case AKVP_CAS:
    case AKVP_INCR:
    case AKVP_INCRS:
    case AKVP_TOUCH:
    case AKVP_FLUSH:
        return 1;
    default:
        return 0;
    }
}

/* Command-table ids of the native operations (index: operation code), filled on first use. */
static int native_ids[AKVP_ROLE + 1];
static pthread_once_t native_ids_once = PTHREAD_ONCE_INIT;

static void native_ids_init(void) {
    static const char *const names[AKVP_ROLE + 1] = {
        [AKVP_GET] = "GET",           [AKVP_PUT] = "PUT",         [AKVP_DEL] = "DEL",
        [AKVP_STATS] = "STATS",       [AKVP_ADD] = "ADD",         [AKVP_REPLACE] = "REPLACE",
        [AKVP_APPEND] = "APPEND",     [AKVP_PREPEND] = "PREPEND", [AKVP_CAS] = "CAS",
        [AKVP_INCR] = "INCR",         [AKVP_INCRS] = "INCRS",     [AKVP_TOUCH] = "TOUCH",
        [AKVP_FLUSH] = "FLUSH",       [AKVP_HAS] = "HAS",         [AKVP_GETM] = "GETM",
        [AKVP_MGET] = "MGET",         [AKVP_AUTH] = "AUTH",       [AKVP_NOOP] = "NOOP",
        [AKVP_REPLSYNC] = "REPLSYNC", [AKVP_ROLE] = "ROLE",
    };
    for (unsigned i = 0; i <= AKVP_ROLE; i++) {
        native_ids[i] = names[i] != NULL ? acl_cmd_find(ACL_NATIVE, names[i], strlen(names[i])) : -1;
    }
}

/* 1 when the session's user may run native operation op on key (klen 0: no key). */
static int native_permitted(const struct session *ss, uint8_t op, const uint8_t *key, size_t klen) {
    if (ss->user == NULL || acl_unrestricted(ss->user)) {
        return 1;
    }
    pthread_once(&native_ids_once, native_ids_init);
    int id = op <= AKVP_ROLE ? native_ids[op] : -1;
    if (id < 0) {
        return 1; /* unknown operation: answered AKVP_BADREQ */
    }
    if (!acl_cmd_allowed(ss->user, (unsigned)id)) {
        return 0;
    }
    const acl_cmd *k = acl_cmd_at((unsigned)id);
    return klen == 0 || k->first == 0 || acl_key_allowed(ss->user, key, klen, k->access);
}

size_t akvd_native_execute(akvd *d, struct session *ss, const uint8_t *msg, size_t len, uint8_t *out,
                           size_t cap) {
    struct akvp_req rq;
    if (akvp_req_decode(msg, len, &rq) != 0) {
        return 0;
    }
    SRV_COUNT(d, commands, 1);
    struct akvp_ext x = {.ttl_ms = 0};
    size_t ext = (rq.flags & AKVP_F_EXT) ? AKVP_EXT : 0;
    int valid = (size_t)AKVP_REQ_HDR + ext + rq.klen + rq.vlen == len && (rq.flags & ~AKVP_F_EXT) == 0;
    if (valid && ext) {
        akvp_ext_decode(msg + AKVP_REQ_HDR, &x);
    }
    const uint8_t *key = msg + AKVP_REQ_HDR + ext;
    const uint8_t *val = key + rq.klen;
    uint8_t *pay = out + AKVP_RESP_HDR;
    size_t room = cap - AKVP_RESP_HDR;
    size_t payload = 0;
    uint8_t status = AKVP_OK;
    akv *s = d->store;
    int keyed = rq.klen > 0;

    if (!valid) {
        status = AKVP_BADREQ;
    } else if (!ss->authed && rq.op != AKVP_AUTH && rq.op != AKVP_NOOP) {
        status = AKVP_NOAUTH;
    } else if (akvd_session_refresh(d, ss) != 0) {
        status = AKVP_NOAUTH;
    } else if (rq.op != AKVP_MGET && !native_permitted(ss, rq.op, key, rq.klen)) {
        status = AKVP_NOPERM;
    } else if (akvd_read_only(d) && native_writes(rq.op)) {
        status = AKVP_READONLY;
    } else {
        switch (rq.op) {
        case AKVP_GET:
        case AKVP_GETM: {
            if (!keyed || rq.vlen != 0) {
                status = AKVP_BADREQ;
                break;
            }
            int meta = rq.op == AKVP_GETM;
            if (meta && room < AKVP_META) {
                status = AKVP_TOOBIG;
                break;
            }
            int64_t touch = meta && ext ? x.ttl_ms : AKV_TTL_KEEP;
            akv_meta m;
            size_t vlen = 0;
            SRV_COUNT(d, cmd_get, 1);
            int rc = akv_get_meta(s, key, rq.klen, pay + (meta ? AKVP_META : 0),
                                  room - (meta ? AKVP_META : 0), &vlen, &m, touch);
            if (rc == 0) {
                SRV_COUNT(d, get_hits, 1);
                payload = vlen;
                if (meta) {
                    akvp_put32(pay, m.flags);
                    akvp_put32(pay + 4, 0);
                    akvp_put64(pay + 8, m.cas);
                    akvp_put64(pay + 16, (uint64_t)m.ttl_ms);
                    payload += AKVP_META;
                }
            } else {
                if (rc == -ENOENT) {
                    SRV_COUNT(d, get_misses, 1);
                }
                status = rc == -ENOENT ? AKVP_NOTFOUND : (rc == -EMSGSIZE ? AKVP_TOOBIG : AKVP_BADREQ);
            }
            break;
        }
        case AKVP_PUT:
        case AKVP_ADD:
        case AKVP_REPLACE:
        case AKVP_APPEND:
        case AKVP_PREPEND:
        case AKVP_CAS: {
            if (!keyed) {
                status = AKVP_BADREQ;
                break;
            }
            akv_store_opts o = {
                .mode = store_mode(rq.op), .flags = x.uflags, .ttl_ms = x.ttl_ms, .cas = x.cas};
            uint64_t cas = 0;
            SRV_COUNT(d, cmd_set, 1);
            int rc = akv_store(s, key, rq.klen, val, rq.vlen, &o, &cas);
            switch (rc) {
            case 0:
                akvp_put64(pay, cas);
                payload = 8;
                break;
            case -EEXIST:
                status = AKVP_EXISTS;
                break;
            case -ENOENT:
                status = AKVP_NOTFOUND;
                break;
            case -E2BIG:
                status = AKVP_TOOBIG;
                break;
            case -ENOMEM:
                status = AKVP_NOMEM;
                break;
            case -EINVAL:
                status = AKVP_BADREQ;
                break;
            default:
                status = AKVP_ERROR;
                break;
            }
            break;
        }
        case AKVP_DEL:
        case AKVP_HAS:
            if (!keyed || rq.vlen != 0) {
                status = AKVP_BADREQ;
            } else if (rq.op == AKVP_DEL) {
                status = akv_del(s, key, rq.klen) == 0 ? AKVP_OK : AKVP_NOTFOUND;
            } else {
                status = akv_exists(s, key, rq.klen) ? AKVP_OK : AKVP_NOTFOUND;
            }
            break;
        case AKVP_INCR:
        case AKVP_INCRS: {
            if (!keyed || rq.vlen != 8) {
                status = AKVP_BADREQ;
                break;
            }
            int64_t result = 0;
            SRV_COUNT(d, cmd_set, 1);
            int rc = akv_incr(s, key, rq.klen, (int64_t)akvp_get64(val),
                              rq.op == AKVP_INCR ? AKV_INCR_UNSIGNED : AKV_INCR_SIGNED, &result, NULL);
            if (rc == 0) {
                akvp_put64(pay, (uint64_t)result);
                payload = 8;
            } else {
                status = rc == -ENOENT                      ? AKVP_NOTFOUND
                         : (rc == -EINVAL || rc == -ERANGE) ? AKVP_NOTNUM
                         : rc == -ENOMEM                    ? AKVP_NOMEM
                                                            : AKVP_ERROR;
            }
            break;
        }
        case AKVP_TOUCH:
            if (!keyed || rq.vlen != 0) {
                status = AKVP_BADREQ;
            } else {
                SRV_COUNT(d, cmd_touch, 1);
                status = akv_touch(s, key, rq.klen, x.ttl_ms) == 0 ? AKVP_OK : AKVP_NOTFOUND;
            }
            break;
        case AKVP_FLUSH:
            if (keyed || rq.vlen != 0) {
                status = AKVP_BADREQ;
            } else {
                akv_flush(s);
                SRV_COUNT(d, cmd_flush, 1);
            }
            break;
        case AKVP_MGET: {
            if (keyed) {
                status = AKVP_BADREQ;
                break;
            }
            size_t off = 0;
            size_t used = 0;
            while (off < rq.vlen && status == AKVP_OK) {
                if (rq.vlen - off < 2) {
                    status = AKVP_BADREQ;
                    break;
                }
                size_t klen = (size_t)(val[off] | (val[off + 1] << 8));
                off += 2;
                if (klen == 0 || klen > rq.vlen - off) {
                    status = AKVP_BADREQ;
                    break;
                }
                if (room - used < 8) {
                    status = AKVP_TOOBIG;
                    break;
                }
                if (!native_permitted(ss, AKVP_MGET, val + off, klen)) {
                    status = AKVP_NOPERM;
                    break;
                }
                size_t vlen = 0;
                SRV_COUNT(d, cmd_get, 1);
                int rc = akv_get(s, val + off, klen, pay + used + 8, room - used - 8, &vlen);
                if (rc == -EMSGSIZE) {
                    status = AKVP_TOOBIG;
                    break;
                }
                pay[used] = rc == 0 ? AKVP_OK : AKVP_NOTFOUND;
                pay[used + 1] = pay[used + 2] = pay[used + 3] = 0;
                akvp_put32(pay + used + 4, rc == 0 ? (uint32_t)vlen : 0);
                SRV_COUNT(d, get_hits, rc == 0);
                SRV_COUNT(d, get_misses, rc != 0);
                used += 8 + (rc == 0 ? vlen : 0);
                off += klen;
            }
            payload = status == AKVP_OK ? used : 0;
            break;
        }
        case AKVP_AUTH: {
            if (keyed) {
                status = AKVP_BADREQ;
                break;
            }
            if (!akvd_acl_enabled(d)) {
                break; /* nothing to check: accepted */
            }
            const char *cv = (const char *)val;
            const char *colon = memchr(cv, ':', rq.vlen);
            int ok = colon != NULL ? akvd_login(d, ss, cv, (size_t)(colon - cv), colon + 1,
                                                rq.vlen - (size_t)(colon - cv) - 1)
                                   : akvd_login(d, ss, NULL, 0, cv, rq.vlen);
            if (!ok) {
                status = AKVP_NOAUTH;
            }
            break;
        }
        case AKVP_NOOP:
            break;
        case AKVP_ROLE:
            pay[0] = repl_is_replica(d->repl) ? 1 : 0;
            payload = 1;
            break;
        case AKVP_REPLSYNC:
            if (keyed || rq.vlen < REPL_ID_LEN + 8) {
                status = AKVP_BADREQ;
                break;
            }
            ss->handoff.len = ss->handoff.off = 0;
            if (ob_add(&ss->handoff, val, rq.vlen) != 0) {
                status = AKVP_ERROR;
                break;
            }
            ss->handing_off = 1;
            akvd_repl_activate(d);
            return 0; /* no response: the connection becomes a replication link */
        case AKVP_STATS: {
            if (keyed || rq.vlen != 0 || room < AKVP_STATS_BYTES) {
                status = room < AKVP_STATS_BYTES ? AKVP_TOOBIG : AKVP_BADREQ;
                break;
            }
            akv_stats st;
            akv_stats_get(s, &st);
            const uint64_t f[AKVP_STATS_FIELDS] = {
                st.entries,
                st.arena_bytes,
                st.log_bytes,
                st.gets,
                st.hits,
                st.puts,
                st.overwrites,
                st.dels,
                st.evictions,
                st.reinsertions,
                st.index_evictions,
                st.rejected,
                st.expired,
                st.index_bytes,
                st.index_grows,
                __atomic_load_n(&d->counters.curr_connections, __ATOMIC_RELAXED),
                __atomic_load_n(&d->counters.total_connections, __ATOMIC_RELAXED),
                __atomic_load_n(&d->counters.commands, __ATOMIC_RELAXED),
                akvd_uptime(d),
            };
            for (unsigned i = 0; i < AKVP_STATS_FIELDS; i++) {
                akvp_put64(pay + 8 * i, f[i]);
            }
            payload = AKVP_STATS_BYTES;
            break;
        }
        default:
            status = AKVP_BADREQ;
            break;
        }
    }
    struct akvp_resp rs = {.status = status, .reqid = rq.reqid, .vlen = (uint32_t)payload};
    akvp_resp_encode(out, &rs);
    return AKVP_RESP_HDR + payload;
}

/*
 * Log sequence number the replies of a turn must wait for: the last record appended by this thread when
 * the turn appended any (aof_thread_seq() moved past `before`) and it is not yet durable; 0 otherwise.
 */
static uint64_t hold_needed(akvd *d, uint64_t before) {
    if (d->aof == NULL || d->cfg.aof_fsync != AOF_FSYNC_ALWAYS) {
        return 0;
    }
    uint64_t s = aof_thread_seq();
    return s != before && s > aof_durable(d->aof) ? s : 0;
}

/* Parks c until the log is durable up to seq: no descriptor events, no output. */
static void park(struct worker *w, struct conn *c, uint64_t seq) {
    if (w->nparked == w->parked_cap) {
        size_t cap = w->parked_cap ? w->parked_cap * 2 : 64;
        struct conn **p = realloc(w->parked, cap * sizeof(*p));
        if (p == NULL) {
            schedule(w, c); /* cannot park: retry the turn (replies stay queued) */
            return;
        }
        w->parked = p;
        w->parked_cap = cap;
    }
    c->parked = 1;
    c->hold_seq = seq;
    w->parked[w->nparked++] = c;
    if (c->epmask != 0) {
        struct epoll_event ev = {.events = 0, .data.ptr = c};
        epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->epmask = 0;
    }
    atomic_store(&w->has_parked, 1);
    aof_kick(w->d->aof); /* the writer does not wake on appends */
    if (aof_durable(w->d->aof) >= seq) {
        /* became durable meanwhile: the writer may have looked at has_parked before the store above */
        uint64_t one = 1;
        if (write(w->evfd, &one, sizeof(one)) < 0) {
            /* the counter cannot overflow; the next loop iteration releases the connection anyway */
        }
    }
}

/* Parks a connection whose RESP WAIT cannot be answered yet. */
static void wait_park(struct worker *w, struct conn *c) {
    if (w->nwaits == w->waits_cap) {
        size_t cap = w->waits_cap ? w->waits_cap * 2 : 16;
        struct conn **p = realloc(w->waits, cap * sizeof(*p));
        if (p == NULL) {
            ob_printf(&c->out, ":%zu\r\n", repl_acked(w->d->repl, c->ss.wait_off));
            c->ss.waiting = 0;
            schedule(w, c);
            return;
        }
        w->waits = p;
        w->waits_cap = cap;
    }
    w->waits[w->nwaits++] = c;
    if (c->epmask != 0) {
        struct epoll_event ev = {.events = 0, .data.ptr = c};
        epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->epmask = 0;
    }
    atomic_store(&w->has_waits, 1);
}

/* Answers the WAITs that are satisfied or timed out. */
static void wait_check(struct worker *w) {
    if (w->nwaits == 0) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    for (size_t i = 0; i < w->nwaits;) {
        struct conn *c = w->waits[i];
        size_t have = repl_acked(w->d->repl, c->ss.wait_off);
        if ((long long)have >= c->ss.wait_n ||
            (c->ss.wait_deadline_ms != 0 && now >= c->ss.wait_deadline_ms)) {
            ob_printf(&c->out, ":%zu\r\n", have);
            c->ss.waiting = 0;
            w->waits[i] = w->waits[--w->nwaits];
            schedule(w, c);
        } else {
            i++;
        }
    }
    if (w->nwaits == 0) {
        atomic_store(&w->has_waits, 0);
    }
}

static void wait_remove(struct worker *w, struct conn *c) {
    for (size_t i = 0; i < w->nwaits; i++) {
        if (w->waits[i] == c) {
            w->waits[i] = w->waits[--w->nwaits];
            break;
        }
    }
}

/* Schedules parked connections whose replies are durable now. */
static void unpark_ready(struct worker *w) {
    if (w->nparked == 0) {
        return;
    }
    uint64_t durable = aof_durable(w->d->aof);
    for (size_t i = 0; i < w->nparked;) {
        struct conn *c = w->parked[i];
        if (c->hold_seq <= durable) {
            w->parked[i] = w->parked[--w->nparked];
            c->parked = 0;
            schedule(w, c);
        } else {
            i++;
        }
    }
    if (w->nparked == 0) {
        atomic_store(&w->has_parked, 0);
    }
}

/* Log writer callback: wakes the workers that hold parked connections. */
static void on_durable(void *arg) {
    akvd *d = arg;
    for (unsigned i = 0; i < d->nworkers; i++) {
        if (atomic_load(&d->workers[i].has_parked)) {
            uint64_t one = 1;
            if (write(d->workers[i].evfd, &one, sizeof(one)) < 0) {
                /* see park() */
            }
        }
    }
}

/* Engine change feed: forwards every record to the append-only log. */
static void on_change(void *arg, const akv_change *c) {
    akvd *d = arg;
    if (d->aof != NULL) {
        aof_append(d->aof, c);
    }
    if (atomic_load_explicit(&d->repl_active, memory_order_relaxed) && !repl_link_thread()) {
        repl_backlog_append(repl_backlog_of(d->repl), c);
    }
}

static void aof_log_sink(void *arg, int level, const char *msg) {
    akvd_log(arg, level == 0 ? AKVD_LOG_ERROR : level == 1 ? AKVD_LOG_WARN : AKVD_LOG_INFO, "%s", msg);
}

static int native_flush(struct conn *c);

/*
 * Turns a native connection into a replication link: sends the responses queued before the request
 * (blocking, with a timeout), removes the endpoint from the worker and starts a feed that owns it.
 */
static void hand_off(struct worker *w, struct conn *c, unsigned nb) {
    akvd *d = w->d;
    int rc = nb > 0 ? duct_send_batch(c->ep, w->bmsg, nb, 10000) : 0;
    while (rc == 0 && ob_pending(&c->pend) > 0) {
        const uint8_t *p = c->pend.p + c->pend.off;
        uint32_t len = akvp_get32(p);
        rc = duct_send(c->ep, p + 4, len, 10000);
        c->pend.off += 4 + (size_t)len;
    }
    epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    duct_ep *ep = c->ep;
    c->ep = NULL;
    if (rc == 0) {
        rc = repl_feed_start(d->repl, ep, c->ss.handoff.p, c->ss.handoff.len);
    }
    if (rc != 0) {
        akvd_log(d, AKVD_LOG_WARN, "replication request of connection %llu failed: %s",
                 (unsigned long long)c->ss.id, duct_strerror(rc));
        duct_ep_close(ep);
    }
    conn_free(c);
}

/* Sends queued native responses. Returns 0 (queue empty), 1 (blocked), or a negative error. */
static int native_flush(struct conn *c) {
    while (ob_pending(&c->pend) > 0) {
        const uint8_t *p = c->pend.p + c->pend.off;
        uint32_t n = akvp_get32(p);
        int rc = duct_send(c->ep, p + 4, n, 0);
        if (rc == -ETIMEDOUT) {
            return 1;
        }
        if (rc != 0) {
            return rc;
        }
        c->pend.off += 4 + (size_t)n;
    }
    ob_compact(&c->pend);
    return 0;
}

/*
 * Sends the responses w->bmsg[0..n), as many as the endpoint accepts now (duct_send_some), and queues
 * the rest in c->pend in order. Returns 0 (all sent), 1 (some queued), or a negative error.
 */
static int native_send_batch(struct worker *w, struct conn *c, unsigned n) {
    int sent = duct_send_some(c->ep, w->bmsg, n);
    if (sent < 0) {
        return sent;
    }
    for (unsigned j = (unsigned)sent; j < n; j++) {
        uint8_t len4[4];
        akvp_put32(len4, (uint32_t)w->bmsg[j].len);
        if (ob_add(&c->pend, len4, 4) != 0 || ob_add(&c->pend, w->bmsg[j].data, w->bmsg[j].len) != 0) {
            return -ENOMEM;
        }
    }
    return (unsigned)sent < n ? 1 : 0;
}

/*
 * Prefetch stage of a native request: stage 0 the index bucket (akv_prefetch), stage 1 the entry
 * (akv_prefetch_entry). Ignores malformed input.
 */
static void native_prefetch(akvd *d, const uint8_t *msg, size_t len, int stage) {
    struct akvp_req rq;
    if (akvp_req_decode(msg, len, &rq) != 0 || rq.klen == 0) {
        return;
    }
    size_t off = AKVP_REQ_HDR + ((rq.flags & AKVP_F_EXT) ? AKVP_EXT : 0);
    if (off + rq.klen > len) {
        return;
    }
    if (stage == 0) {
        akv_prefetch(d->store, msg + off, rq.klen);
    } else {
        akv_prefetch_entry(d->store, msg + off, rq.klen);
    }
}

/* Serves a native connection for one turn. Returns 0 (armed, waiting), 1 (work left), or -1 (closed). */
static int native_serve(struct worker *w, struct conn *c) {
    akvd *d = w->d;
    uint64_t before = aof_thread_seq();
    /* At most recv_depth messages can be held at once (duct_recv). */
    unsigned prefetch = d->cfg.recv_depth < PREFETCH ? d->cfg.recv_depth : PREFETCH;
    size_t cap = d->cfg.max_msg;
    uint32_t peer = duct_ep_peer_max_msg(c->ep);
    if (cap > peer) {
        cap = peer; /* the peer cannot receive larger responses; oversize values answer AKVP_TOOBIG */
    }
    for (;;) {
        int rc = native_flush(c);
        if (rc < 0) {
            conn_free(c);
            return -1;
        }
        int handled = 0;
        while (rc == 0 && handled < BURST) {
            unsigned nb = 0;
            size_t used = 0;
            int more = 1;
            /* Execute requests into the batch area until it is full (the next response may need cap
             * bytes), the burst ends, or no message waits. Requests are taken in groups of up to
             * PREFETCH: the index buckets of a group are prefetched before its requests execute, so that
             * their cache misses overlap instead of following one another. */
            while (more && nb < BATCH_MSGS && handled < BURST && used + cap <= w->resp_cap) {
                struct duct_rx rx[PREFETCH];
                unsigned k = 0;
                while (k < prefetch && nb + k < BATCH_MSGS && handled + (int)k < BURST &&
                       used + (k + 1) * cap <= w->resp_cap) {
                    int r = duct_recv(c->ep, &rx[k], 0);
                    if (r == -ETIMEDOUT) {
                        more = 0;
                        break;
                    }
                    if (r != 0) {
                        conn_free(c); /* closing the endpoint releases the held messages */
                        return -1;
                    }
                    native_prefetch(d, rx[k].data, rx[k].len, 0);
                    k++;
                }
                for (unsigned j = 0; k > 1 && j < k; j++) {
                    native_prefetch(d, rx[j].data, rx[j].len, 1);
                }
                for (unsigned j = 0; j < k; j++) {
                    uint8_t *out = w->resp + used;
                    size_t n = akvd_native_execute(d, &c->ss, rx[j].data, rx[j].len, out, cap);
                    if (c->ss.close) {
                        n = 0; /* the user was removed or disabled by an ACL reload */
                    }
                    duct_rx_release(c->ep, &rx[j]);
                    if (n == 0 && c->ss.handing_off) {
                        for (unsigned q = j + 1; q < k; q++) {
                            duct_rx_release(c->ep, &rx[q]);
                        }
                        hand_off(w, c, nb);
                        return -1;
                    }
                    if (n == 0) {
                        conn_free(c);
                        return -1;
                    }
                    w->bmsg[nb++] = (struct duct_msg){.data = out, .len = n};
                    used += n;
                    handled++;
                }
            }
            if (nb > 0) {
                uint64_t hold = hold_needed(d, before);
                if (hold != 0) {
                    /* Queue the whole batch; it is sent when the log is durable. */
                    for (unsigned j = 0; j < nb; j++) {
                        uint8_t len4[4];
                        akvp_put32(len4, (uint32_t)w->bmsg[j].len);
                        if (ob_add(&c->pend, len4, 4) != 0 ||
                            ob_add(&c->pend, w->bmsg[j].data, w->bmsg[j].len) != 0) {
                            conn_free(c);
                            return -1;
                        }
                    }
                    c->last_active = mono_s();
                    park(w, c, hold);
                    return 0;
                }
                rc = native_send_batch(w, c, nb);
                if (rc < 0) {
                    conn_free(c);
                    return -1;
                }
            }
            if (!more) {
                break;
            }
        }
        if (handled > 0) {
            c->last_active = mono_s(); /* once per turn, not per request */
        }
        if (handled == BURST) {
            return 1;
        }
        int a = duct_ep_arm(c->ep);
        if (a == 0) {
            uint32_t want = duct_ep_wait_events(c->ep);
            if (want != c->epmask) {
                struct epoll_event ev = {.events = want, .data.ptr = c};
                if (epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0) {
                    conn_free(c);
                    return -1;
                }
                c->epmask = want;
            }
            return 0;
        }
        if (a != -EAGAIN) {
            conn_free(c);
            return -1;
        }
        if (rc == 1) {
            return 1; /* responses wait for send space; retry on the next turn */
        }
    }
}

/* ---- text connections --------------------------------------------------------------------------------- */

/* Reads available bytes into c->in. Returns > 0 bytes, 0 no data, -1 closed or failed. */
static ssize_t text_read(struct conn *c, uint8_t *buf, size_t n) {
#ifdef AKV_HAVE_TLS
    if (c->ssl != NULL) {
        int r = SSL_read(c->ssl, buf, (int)(n > INT32_MAX ? INT32_MAX : n));
        if (r > 0) {
            return r;
        }
        int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_WANT_READ) {
            return 0;
        }
        if (e == SSL_ERROR_WANT_WRITE) {
            c->tls_want_write = 1;
            return 0;
        }
        return -1;
    }
#endif
    for (;;) {
        ssize_t r = recv(c->fd, buf, n, 0);
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

/* Writes pending output. Returns 0 (all written or blocked), -1 on failure. */
static int text_write(struct conn *c) {
    while (ob_pending(&c->out) > 0) {
        size_t n = ob_pending(&c->out);
        ssize_t w;
#ifdef AKV_HAVE_TLS
        if (c->ssl != NULL) {
            int r = SSL_write(c->ssl, c->out.p + c->out.off, (int)(n > INT32_MAX ? INT32_MAX : n));
            if (r > 0) {
                c->out.off += (size_t)r;
                continue;
            }
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_WRITE) {
                c->tls_want_write = 1;
                return 0;
            }
            if (e == SSL_ERROR_WANT_READ) {
                return 0;
            }
            return -1;
        }
#endif
        w = send(c->fd, c->out.p + c->out.off, n, MSG_NOSIGNAL);
        if (w > 0) {
            c->out.off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    ob_compact(&c->out);
    return 0;
}

static void text_mask(struct worker *w, struct conn *c, int reading) {
    uint32_t m = (reading ? EPOLLIN : 0) | (ob_pending(&c->out) > 0 || c->tls_want_write ? EPOLLOUT : 0);
    if (m != c->epmask) {
        struct epoll_event ev = {.events = m, .data.ptr = c};
        epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->epmask = m;
    }
}

/* Returns 1 when the TLS layer holds decrypted input that raises no socket event. */
static int tls_buffered(const struct conn *c) {
#ifdef AKV_HAVE_TLS
    return c->ssl != NULL && SSL_pending(c->ssl) > 0;
#else
    (void)c;
    return 0;
#endif
}

/* Serves a text connection for one turn. Returns 0 (waiting), 1 (work left), or -1 (closed). */
static int text_serve(struct worker *w, struct conn *c) {
    akvd *d = w->d;
    size_t max_in = akvd_max_input(d);
    uint64_t before = aof_thread_seq();
    for (int round = 0; round < BURST; round++) {
        c->tls_want_write = 0;
        if (text_write(c) != 0) {
            conn_free(c);
            return -1;
        }
        if (c->ss.close) {
            if (ob_pending(&c->out) == 0) {
                conn_free(c);
                return -1;
            }
            text_mask(w, c, 0);
            return 0;
        }
        if (ob_pending(&c->out) > OB_SOFT_LIMIT) {
            text_mask(w, c, 0); /* backpressure: wait until the client reads */
            return 0;
        }
        /* Read once, then execute what is complete. */
        if (c->in.len - c->in.off >= max_in) {
            conn_free(c); /* a single command larger than the limit */
            return -1;
        }
        size_t want = READ_CHUNK;
        if (c->in.len - c->in.off + want > max_in) {
            want = max_in - (c->in.len - c->in.off);
        }
        uint8_t *dst = ob_reserve(&c->in, want);
        if (dst == NULL) {
            conn_free(c);
            return -1;
        }
        ssize_t r = text_read(c, dst, want);
        if (r < 0) {
            conn_free(c);
            return -1;
        }
        if (r > 0) {
            ob_commit(&c->in, (size_t)r);
            c->last_active = mono_s();
        }
        size_t avail = c->in.len - c->in.off;
        if (avail > 0) {
            const uint8_t *in = c->in.p + c->in.off;
            size_t used = c->kind == CK_MC ? mc_execute(d, &c->ss, in, avail, &c->out)
                                           : resp_execute(d, &c->ss, in, avail, &c->out);
            c->in.off += used;
            ob_compact(&c->in);
            uint64_t hold = hold_needed(d, before);
            if (hold != 0) {
                park(w, c, hold);
                return 0;
            }
            if (c->ss.waiting) {
                wait_park(w, c);
                return 0;
            }
            if (used > 0) {
                continue; /* replies may be pending: write them, then read again */
            }
        }
        if (r == 0) {
            if (text_write(c) != 0) {
                conn_free(c);
                return -1;
            }
            text_mask(w, c, ob_pending(&c->out) <= OB_SOFT_LIMIT && !c->ss.close);
            return tls_buffered(c) ? 1 : 0;
        }
    }
    text_mask(w, c, 1);
    return 1; /* turn exhausted: input may be left in the buffer or the TLS layer */
}

/* ---- workers ------------------------------------------------------------------------------------------ */

/* Puts c on the ready list (once). */
static void schedule(struct worker *w, struct conn *c) {
    if (c->in_ready) {
        return;
    }
    if (w->nready == w->ready_cap) {
        size_t cap = w->ready_cap ? w->ready_cap * 2 : 64;
        struct conn **r = realloc(w->ready, cap * sizeof(*r));
        if (r == NULL) {
            return; /* the connection is served again on its next descriptor event or the next scan */
        }
        w->ready = r;
        w->ready_cap = cap;
    }
    w->ready[w->nready++] = c;
    c->in_ready = 1;
}

/* Serves c for one turn. Returns the turn result (1 = work left); c may be freed when it returns -1. */
static int serve_turn(struct worker *w, struct conn *c) {
    if (c->dead) {
        return -1;
    }
    return c->kind == CK_NATIVE ? native_serve(w, c) : text_serve(w, c);
}

/* Serves c after a descriptor event and schedules it when work is left. */
static void serve(struct worker *w, struct conn *c) {
    if (serve_turn(w, c) == 1) {
        schedule(w, c);
    }
}

/*
 * Serves the connections on the ready list once. While a connection is served from the list its
 * in_ready flag stays set, so that conn_free() defers the release; the list frees such connections.
 */
static void run_ready(struct worker *w) {
    size_t n = w->nready;
    struct conn **list = w->ready;
    w->ready = NULL;
    w->nready = 0;
    w->ready_cap = 0;
    for (size_t i = 0; i < n; i++) {
        struct conn *c = list[i];
        int rc = serve_turn(w, c);
        if (c->dead) {
            free(c);
            continue;
        }
        c->in_ready = 0;
        if (rc == 1) {
            schedule(w, c);
        }
    }
    free(list);
}

/* Takes over queued connections: registers them and serves them once. */
static void adopt(struct worker *w) {
    pthread_mutex_lock(&w->qlock);
    struct conn *q = w->queue;
    w->queue = NULL;
    pthread_mutex_unlock(&w->qlock);
    while (q != NULL) {
        struct conn *c = q;
        q = c->next;
        c->prev = NULL;
        c->next = w->conns;
        if (w->conns != NULL) {
            w->conns->prev = c;
        }
        w->conns = c;
        c->w = w;
        c->ss.conn = c;
        c->ss.worker = w->index;
        c->last_active = mono_s();
        if (c->kind == CK_NATIVE) {
            c->fd = duct_ep_direct_fd(c->ep);
        }
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
        c->epmask = EPOLLIN;
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, c->fd, &ev) != 0) {
            conn_free(c);
            continue;
        }
        serve(w, c);
    }
}

static void close_idle(struct worker *w) {
    unsigned limit = w->d->cfg.idle_timeout_s;
    if (limit == 0) {
        return;
    }
    uint64_t now = mono_s();
    struct conn *c = w->conns;
    while (c != NULL) {
        struct conn *next = c->next;
        if (now - c->last_active >= limit && !c->in_ready) {
            akvd_log(w->d, AKVD_LOG_DEBUG, "closing idle connection %llu", (unsigned long long)c->ss.id);
            conn_free(c);
        }
        c = next;
    }
}

static void *worker_main(void *arg) {
    struct worker *w = arg;
    akvd *d = w->d;
    struct epoll_event evs[128];
    uint64_t last_scan = mono_s();
    while (!atomic_load(&d->stopping)) {
        int n = epoll_wait(w->epfd, evs, 128, w->nready > 0 ? 0 : w->nwaits > 0 ? 100 : 1000);
        for (int i = 0; i < n; i++) {
            if (evs[i].data.ptr == NULL) {
                uint64_t v;
                if (read(w->evfd, &v, sizeof(v)) < 0) {
                    /* counter already reset by a concurrent read */
                }
                adopt(w);
                unpark_ready(w);
                drain_inbox(w);
                continue;
            }
            serve(w, evs[i].data.ptr);
        }
        wait_check(w);
        if (w->nready > 0) {
            run_ready(w);
        }
        uint64_t now = mono_s();
        if (now != last_scan) {
            close_idle(w);
            last_scan = now;
        }
    }
    adopt(w); /* connections queued during shutdown */
    for (size_t i = 0; i < w->nready; i++) {
        w->ready[i]->in_ready = 0;
        if (w->ready[i]->dead) {
            free(w->ready[i]);
        }
    }
    free(w->ready);
    w->ready = NULL;
    w->nready = 0;
    free(w->parked);
    w->parked = NULL;
    w->nparked = 0;
    free(w->waits);
    w->waits = NULL;
    w->nwaits = 0;
    drain_inbox(w);
    while (w->conns != NULL) {
        struct conn *c = w->conns;
        w->conns = c->next; /* unlinked here, so conn_free() has no list to update */
        if (c->next != NULL) {
            c->next->prev = NULL;
        }
        c->prev = c->next = NULL;
        if (c->kind != CK_NATIVE) {
            text_write(c); /* best effort: deliver replies already produced */
        }
        conn_free(c);
    }
    srv_thread_exit();
    resp_thread_exit();
    return NULL;
}

/* ---- accepting ---------------------------------------------------------------------------------------- */

static void hand_over(akvd *d, struct conn *c) {
    unsigned cur = __atomic_add_fetch(&d->counters.curr_connections, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&d->counters.total_connections, 1, __ATOMIC_RELAXED);
    c->ss.id = atomic_fetch_add(&d->next_id, 1) + 1;
    akvd_session_init(d, &c->ss);
    if (d->cfg.max_conns != 0 && cur > d->cfg.max_conns) {
        __atomic_fetch_add(&d->counters.rejected_connections, 1, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&d->counters.curr_connections, 1, __ATOMIC_RELAXED);
        if (c->kind == CK_NATIVE) {
            duct_ep_close(c->ep);
        } else {
#ifdef AKV_HAVE_TLS
            if (c->ssl != NULL) {
                SSL_free(c->ssl);
            }
#endif
            close(c->fd);
        }
        free(c);
        akvd_log(d, AKVD_LOG_WARN, "connection rejected: limit of %u reached", d->cfg.max_conns);
        return;
    }
    struct worker *w = &d->workers[d->next_worker++ % d->nworkers];
    pthread_mutex_lock(&w->qlock);
    c->next = w->queue;
    w->queue = c;
    pthread_mutex_unlock(&w->qlock);
    uint64_t one = 1;
    if (write(w->evfd, &one, sizeof(one)) < 0) {
        /* the counter cannot overflow at this rate; a failure leaves the connection queued */
    }
}

static void accept_text(akvd *d, int lfd, enum conn_kind kind, int tls) {
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct conn *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->kind = kind;
        c->fd = fd;
#ifdef AKV_HAVE_TLS
        if (tls) {
            c->ssl = SSL_new(d->tls);
            if (c->ssl == NULL || SSL_set_fd(c->ssl, fd) != 1) {
                if (c->ssl != NULL) {
                    SSL_free(c->ssl);
                }
                close(fd);
                free(c);
                continue;
            }
            SSL_set_accept_state(c->ssl);
        }
#else
        (void)tls;
#endif
        hand_over(d, c);
    }
}

static void *accept_main(void *arg) {
    akvd *d = arg;
    struct pollfd p[6];
    int kinds[6];
    int n = 0;
    p[n] = (struct pollfd){.fd = d->stop_fd, .events = POLLIN};
    kinds[n++] = -1;
    if (d->nl != NULL) {
        p[n] = (struct pollfd){.fd = duct_listener_fd(d->nl), .events = POLLIN};
        kinds[n++] = 0;
    }
    int fds[4] = {d->mc_fd, d->resp_fd, d->tls_mc_fd, d->tls_resp_fd};
    for (int i = 0; i < 4; i++) {
        if (fds[i] >= 0) {
            p[n] = (struct pollfd){.fd = fds[i], .events = POLLIN};
            kinds[n++] = 1 + i;
        }
    }
    while (!atomic_load(&d->stopping)) {
        if (poll(p, (nfds_t)n, 1000) <= 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (!(p[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                continue;
            }
            switch (kinds[i]) {
            case -1:
                break;
            case 0: {
                duct_ep *ep;
                while (duct_accept(d->nl, 0, &ep) == 0) {
                    struct conn *c = calloc(1, sizeof(*c));
                    if (c == NULL) {
                        duct_ep_close(ep);
                        break;
                    }
                    c->kind = CK_NATIVE;
                    c->ep = ep;
                    c->fd = -1;
                    hand_over(d, c);
                }
                break;
            }
            case 1:
                accept_text(d, d->mc_fd, CK_MC, 0);
                break;
            case 2:
                accept_text(d, d->resp_fd, CK_RESP, 0);
                break;
            case 3:
                accept_text(d, d->tls_mc_fd, CK_MC, 1);
                break;
            case 4:
                accept_text(d, d->tls_resp_fd, CK_RESP, 1);
                break;
            }
        }
    }
    return NULL;
}

/* Opens a non-blocking TCP listener. Returns the descriptor and the bound port, or a negative error. */
static int tcp_listen(const char *addr, const char *port, int *bound) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
    struct addrinfo *res = NULL;
    if (getaddrinfo(addr, port, &hints, &res) != 0) {
        return -EINVAL;
    }
    int fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, res->ai_protocol);
    int rc = 0;
    if (fd < 0) {
        rc = errno != 0 ? -errno : -EIO;
    } else {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 1024) != 0) {
            rc = errno != 0 ? -errno : -EIO;
            close(fd);
        }
    }
    freeaddrinfo(res);
    if (rc != 0) {
        return rc;
    }
    struct sockaddr_in sa = {0}; /* initialized: the analyzer does not model getsockname() */
    socklen_t sl = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
        rc = -errno;
        close(fd);
        return rc;
    }
    *bound = ntohs(sa.sin_port);
    return fd;
}

/* ---- lifecycle ---------------------------------------------------------------------------------------- */

const char *akvd_version(void) {
    return AKVD_VERSION;
}

void akvd_config_default(akvd_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    akv_config_default(&cfg->store);
    cfg->transport = DUCT_TRANSPORT_AUTO;
    cfg->wait = DUCT_WAIT_EVENT;
    cfg->port = "7600";
    cfg->max_msg = 64u * 1024u;
    cfg->recv_depth = 16;
    cfg->send_depth = 16;
    cfg->log_level = AKVD_LOG_INFO;
    cfg->aof_fsync = AOF_FSYNC_EVERYSEC;
    cfg->replica_read_only = 1;
    cfg->aof_rewrite_percentage = 100;
    cfg->aof_rewrite_min_size = 64ull << 20; /* Valkey's auto-aof-rewrite-min-size default */
}

#ifdef AKV_HAVE_TLS
static SSL_CTX *tls_create(akvd *d) {
    SSL_CTX *t = SSL_CTX_new(TLS_server_method());
    if (t == NULL) {
        return NULL;
    }
    SSL_CTX_set_min_proto_version(t, TLS1_2_VERSION);
    SSL_CTX_set_mode(t, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (SSL_CTX_use_certificate_chain_file(t, d->cfg.tls_cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(t, d->cfg.tls_key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(t) != 1) {
        akvd_log(d, AKVD_LOG_ERROR, "TLS certificate or key cannot be loaded");
        SSL_CTX_free(t);
        return NULL;
    }
    if (d->cfg.tls_ca != NULL) {
        if (SSL_CTX_load_verify_locations(t, d->cfg.tls_ca, NULL) != 1) {
            akvd_log(d, AKVD_LOG_ERROR, "TLS CA file cannot be loaded");
            SSL_CTX_free(t);
            return NULL;
        }
        SSL_CTX_set_verify(t, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    }
    return t;
}
#endif

static void free_all(akvd *d) {
    if (d->nl != NULL) {
        duct_listener_close(d->nl);
    }
    int fds[5] = {d->mc_fd, d->resp_fd, d->tls_mc_fd, d->tls_resp_fd, d->stop_fd};
    for (int i = 0; i < 5; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    for (unsigned i = 0; d->workers != NULL && i < d->nworkers; i++) {
        struct worker *w = &d->workers[i];
        if (w->epfd >= 0) {
            close(w->epfd);
        }
        if (w->evfd >= 0) {
            close(w->evfd);
        }
        pthread_mutex_destroy(&w->qlock);
        pthread_mutex_destroy(&w->inbox_lock);
        ob_free(&w->inbox);
        free(w->subs);
        free(w->resp);
    }
    free(d->workers);
    duct_ctx_destroy(d->ctx);
    if (d->aof != NULL) {
        aof_close(d->aof);
    }
    pubsub_destroy(d->ps);
    repl_destroy(d->repl);
    if (d->store != NULL && d->cfg.store.shm_path != NULL) {
        akv_detach(d->store); /* keeps restored memory when the start fails afterwards */
    } else {
        akv_destroy(d->store);
    }
    cluster_free(atomic_load(&d->cluster));
    for (size_t i = 0; i < d->nold_clusters; i++) {
        cluster_free(d->old_clusters[i]);
    }
    free(d->old_clusters);
    acl_free(atomic_load(&d->acl));
    for (size_t i = 0; i < d->nretired; i++) {
        acl_free(d->retired[i]);
    }
    free(d->retired);
    pthread_mutex_destroy(&d->acl_lock);
#ifdef AKV_HAVE_TLS
    if (d->tls != NULL) {
        SSL_CTX_free(d->tls);
    }
#endif
    if (d->log != NULL) {
        fclose(d->log);
    }
    pthread_mutex_destroy(&d->log_lock);
    pthread_mutex_destroy(&d->save_lock);
    pthread_mutex_destroy(&d->saver_lock);
    pthread_cond_destroy(&d->saver_cond);
    free(d);
}

akvd *akvd_start(const akvd_config *cfg, int *err) {
    int dummy;
    if (err == NULL) {
        err = &dummy;
    }
    *err = 0;
    struct akvd *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        *err = -ENOMEM;
        return NULL;
    }
    d->cfg = *cfg;
    d->mc_fd = d->resp_fd = d->tls_mc_fd = d->tls_resp_fd = d->stop_fd = -1;
    pthread_mutex_init(&d->log_lock, NULL);
    pthread_mutex_init(&d->acl_lock, NULL);
    pthread_mutex_init(&d->save_lock, NULL);
    pthread_mutex_init(&d->saver_lock, NULL);
    pthread_cond_init(&d->saver_cond, NULL);
    atomic_store(&d->last_ok, 1);
    clock_gettime(CLOCK_MONOTONIC, &d->started);
    if (d->cfg.max_msg == 0) {
        d->cfg.max_msg = 64u * 1024u;
    }
    if (d->cfg.recv_depth == 0) {
        d->cfg.recv_depth = 16;
    }
    if (d->cfg.send_depth == 0) {
        d->cfg.send_depth = 16;
    }
    if (d->cfg.threads == 0) {
        /* CPUs the process may run on (taskset, container cpuset), not all online CPUs: more workers than
         * usable CPUs only add context switches. */
        cpu_set_t set;
        long n =
            sched_getaffinity(0, sizeof(set), &set) == 0 ? CPU_COUNT(&set) : sysconf(_SC_NPROCESSORS_ONLN);
        d->cfg.threads = n > 0 ? (unsigned)n : 1;
    }
    d->nworkers = d->cfg.threads;
    if (d->cfg.log_path != NULL) {
        d->log = fopen(d->cfg.log_path, "a");
        if (d->log == NULL) {
            *err = -errno;
            goto fail;
        }
    }
#ifndef AKV_HAVE_TLS
    if (d->cfg.tls_memcached_port != NULL || d->cfg.tls_resp_port != NULL) {
        *err = -ENOTSUP;
        goto fail;
    }
#else
    if (d->cfg.tls_memcached_port != NULL || d->cfg.tls_resp_port != NULL) {
        if (d->cfg.tls_cert == NULL || d->cfg.tls_key == NULL || (d->tls = tls_create(d)) == NULL) {
            *err = -EINVAL;
            goto fail;
        }
    }
#endif
    if (d->cfg.auth_file != NULL || d->cfg.acl_file != NULL) {
        char msg[512];
        if (d->cfg.auth_file != NULL && d->cfg.acl_file != NULL) {
            akvd_log(d, AKVD_LOG_ERROR, "the credentials file and the ACL file exclude each other");
            *err = -EINVAL;
            goto fail;
        }
        acl *a = d->cfg.acl_file != NULL ? acl_load(d->cfg.acl_file, 0, msg, sizeof(msg))
                                         : acl_load(d->cfg.auth_file, 1, msg, sizeof(msg));
        if (a == NULL) {
            akvd_log(d, AKVD_LOG_ERROR, "access control list: %s", msg);
            *err = -EINVAL;
            goto fail;
        }
        atomic_store(&d->acl, a);
    }
    if (d->cfg.cluster_config != NULL) {
        char msg[512];
        cluster *cl = cluster_load(d->cfg.cluster_config, d->cfg.cluster_node_id, msg, sizeof(msg));
        if (cl == NULL) {
            akvd_log(d, AKVD_LOG_ERROR, "cluster map: %s", msg);
            *err = -EINVAL;
            goto fail;
        }
        atomic_store(&d->cluster, cl);
        if (d->cfg.replicaof_host == NULL && cl->self >= 0 && !cl->nodes[cl->self].primary) {
            static char port[16];
            const struct cluster_node *pn = &cl->nodes[cl->nodes[cl->self].primary_of];
            snprintf(port, sizeof(port), "%d", pn->native_port);
            d->cfg.replicaof_host = pn->host;
            d->cfg.replicaof_port = port;
        }
    }
    d->cfg.store.on_change = on_change;
    d->cfg.store.on_change_arg = d;
    errno = 0;
    d->store = akv_create(&d->cfg.store);
    if (d->store == NULL) {
        *err = d->cfg.store.shm_path != NULL && errno != 0 ? -errno : -EINVAL;
        if (d->cfg.store.shm_path != NULL) {
            akvd_log(d, AKVD_LOG_ERROR, "warm-restart file %s: %s", d->cfg.store.shm_path, strerror(-*err));
        }
        goto fail;
    }
    akv_warm_info wi;
    akv_warm_get(d->store, &wi);
    static const char *const warm_names[] = {
        "none",   "created", "restored", "configuration changed", "previous run did not stop cleanly",
        "damaged"};
    if (wi.state == AKV_WARM_RESTORED) {
        akvd_log(d, AKVD_LOG_INFO,
                 "warm restart: %llu entries restored in %.3f s (%llu expired while stopped)",
                 (unsigned long long)wi.entries, (double)wi.rebuild_us / 1e6, (unsigned long long)wi.expired);
    } else if (wi.state != AKV_WARM_NONE && wi.state != AKV_WARM_CREATED) {
        akvd_log(d, AKVD_LOG_WARN, "warm restart: memory discarded (%s)", warm_names[wi.state]);
    }
    int restored = wi.state == AKV_WARM_RESTORED;
    int have_log = 0;
    if (d->cfg.aof_dir != NULL) {
        char mpath[4096];
        struct stat mst;
        snprintf(mpath, sizeof(mpath), "%s/manifest", d->cfg.aof_dir);
        have_log = stat(mpath, &mst) == 0;
    }
    if (d->cfg.snapshot_path != NULL && (restored || have_log)) {
        akvd_log(d, AKVD_LOG_INFO, "snapshot not loaded: the %s is newer",
                 restored ? "warm-restart memory" : "append-only log");
    }
    if (d->cfg.snapshot_path != NULL && !restored && !have_log) {
        uint64_t loaded = 0;
        int rc = snap_load(d->store, d->cfg.snapshot_path, &loaded);
        if (rc == 0) {
            akvd_log(d, AKVD_LOG_INFO, "snapshot loaded: %llu entries", (unsigned long long)loaded);
        } else if (rc != -ENOENT) {
            akvd_log(d, AKVD_LOG_ERROR, "snapshot %s cannot be loaded (%s); move it away to start empty",
                     d->cfg.snapshot_path, strerror(-rc));
            *err = rc;
            goto fail;
        }
    }
    if (d->cfg.aof_dir != NULL) {
        aof_config ac = {
            .dir = d->cfg.aof_dir,
            .fsync = (enum aof_fsync)d->cfg.aof_fsync,
            .rewrite_percentage = d->cfg.aof_rewrite_percentage,
            .rewrite_min_size = d->cfg.aof_rewrite_min_size,
            .load_damaged = d->cfg.aof_load_damaged,
            .log = aof_log_sink,
            .log_arg = d,
            .durable = on_durable,
            .durable_arg = d,
        };
        aof_load_info li;
        int rc = aof_open(&ac, d->store, !restored, &d->aof, &li);
        if (rc != 0) {
            akvd_log(d, AKVD_LOG_ERROR,
                     "append-only log in %s cannot be opened (%s); see the operations guide for damaged logs",
                     d->cfg.aof_dir, strerror(-rc));
            *err = rc;
            goto fail;
        }
        if (li.created) {
            akvd_log(d, AKVD_LOG_INFO, "append-only log created in %s", d->cfg.aof_dir);
        } else if (!restored) {
            akvd_log(d, AKVD_LOG_INFO, "append-only log loaded: %llu records from %llu files",
                     (unsigned long long)li.records, (unsigned long long)li.files);
        }
        if (li.truncated != 0) {
            akvd_log(d, AKVD_LOG_WARN, "append-only log: %llu bytes of an incomplete last write removed",
                     (unsigned long long)li.truncated);
        }
        if (li.dropped_txn != 0) {
            akvd_log(d, AKVD_LOG_WARN,
                     "append-only log: %llu records of an incomplete transaction not applied",
                     (unsigned long long)li.dropped_txn);
        }
        if (li.damaged) {
            akvd_log(d, AKVD_LOG_WARN,
                     "append-only log: started with the records before the damage; rewriting");
        }
    }
    {
        akv_stats st;
        akv_stats_get(d->store, &st);
        d->saved_puts = st.puts + st.dels;
    }
    if (d->cfg.port != NULL) {
        struct duct_opts o;
        akvd_duct_opts(d, &o);
        if ((*err = duct_ctx_create(&o, &d->ctx)) != 0) {
            akvd_log(d, AKVD_LOG_ERROR, "native transport cannot be set up: %s", duct_strerror(*err));
            goto fail;
        }
        if ((*err = duct_listen(d->ctx, d->cfg.addr, d->cfg.port, &d->nl)) != 0) {
            akvd_log(d, AKVD_LOG_ERROR, "native listener on %s port %s: %s", d->cfg.addr ? d->cfg.addr : "*",
                     d->cfg.port, duct_strerror(*err));
            goto fail;
        }
        d->ports[0] = duct_listener_port(d->nl);
    }
    const char *tports[4] = {d->cfg.memcached_port, d->cfg.resp_port, d->cfg.tls_memcached_port,
                             d->cfg.tls_resp_port};
    int *tfds[4] = {&d->mc_fd, &d->resp_fd, &d->tls_mc_fd, &d->tls_resp_fd};
    for (int i = 0; i < 4; i++) {
        if (tports[i] == NULL) {
            continue;
        }
        int fd = tcp_listen(d->cfg.text_addr, tports[i], &d->ports[1 + i]);
        if (fd < 0) {
            static const char *const names[4] = {"memcached", "RESP", "TLS memcached", "TLS RESP"};
            akvd_log(d, AKVD_LOG_ERROR, "%s listener on %s port %s: %s", names[i],
                     d->cfg.text_addr ? d->cfg.text_addr : "*", tports[i], strerror(-fd));
            *err = fd;
            goto fail;
        }
        *tfds[i] = fd;
    }
    if (d->cfg.pubsub_output_limit == 0) {
        d->cfg.pubsub_output_limit = 32ull << 20;
    }
    d->ps = pubsub_create(deliver, d);
    d->repl = repl_create(d, d->cfg.repl_backlog_size != 0 ? d->cfg.repl_backlog_size : (64u << 20));
    d->stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    d->workers = calloc(d->nworkers, sizeof(*d->workers));
    if (d->stop_fd < 0 || d->workers == NULL || d->ps == NULL || d->repl == NULL) {
        *err = -ENOMEM;
        goto fail;
    }
    for (unsigned i = 0; i < d->nworkers; i++) {
        struct worker *w = &d->workers[i];
        w->d = d;
        w->index = i;
        pthread_mutex_init(&w->inbox_lock, NULL);
        w->epfd = epoll_create1(EPOLL_CLOEXEC);
        w->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        pthread_mutex_init(&w->qlock, NULL);
        w->resp_cap = d->cfg.max_msg > BATCH_BYTES ? d->cfg.max_msg : BATCH_BYTES;
        w->resp = malloc(w->resp_cap);
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
        if (w->epfd < 0 || w->evfd < 0 || w->resp == NULL ||
            epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->evfd, &ev) != 0) {
            *err = -ENOMEM;
            d->nworkers = i + 1;
            goto fail;
        }
    }
    for (unsigned i = 0; i < d->nworkers; i++) {
        if (pthread_create(&d->workers[i].thread, NULL, worker_main, &d->workers[i]) != 0) {
            atomic_store(&d->stopping, 1);
            for (unsigned k = 0; k < i; k++) {
                uint64_t one = 1;
                if (write(d->workers[k].evfd, &one, sizeof(one)) < 0) {
                    /* the worker notices the stop flag within its poll timeout */
                }
                pthread_join(d->workers[k].thread, NULL);
            }
            *err = -EAGAIN;
            goto fail;
        }
    }
    if (pthread_create(&d->accept_thread, NULL, accept_main, d) != 0) {
        *err = -EAGAIN;
        atomic_store(&d->stopping, 1);
        for (unsigned k = 0; k < d->nworkers; k++) {
            pthread_join(d->workers[k].thread, NULL);
        }
        goto fail;
    }
    d->accept_started = 1;
    if (d->cfg.replicaof_host != NULL) {
        akvd_repl_activate(d);
        if ((*err = repl_follow(d->repl, d->cfg.replicaof_host, d->cfg.replicaof_port)) != 0) {
            akvd_log(d, AKVD_LOG_ERROR, "replication cannot start: %s", duct_strerror(*err));
        }
    }
    if (d->cfg.snapshot_path != NULL && d->cfg.save_interval_s > 0 &&
        pthread_create(&d->saver, NULL, saver_main, d) == 0) {
        d->saver_started = 1;
    }
    return d;
fail:
    free_all(d);
    return NULL;
}

void akvd_stop(akvd *d) {
    if (d == NULL) {
        return;
    }
    atomic_store(&d->stopping, 1);
    uint64_t one = 1;
    if (write(d->stop_fd, &one, sizeof(one)) < 0) {
        /* the accept thread notices the stop flag within its poll timeout */
    }
    if (d->accept_started) {
        pthread_join(d->accept_thread, NULL);
    }
    for (unsigned i = 0; i < d->nworkers; i++) {
        if (write(d->workers[i].evfd, &one, sizeof(one)) < 0) {
            /* see above */
        }
        pthread_join(d->workers[i].thread, NULL);
    }
    if (d->saver_started) {
        pthread_mutex_lock(&d->saver_lock);
        pthread_cond_broadcast(&d->saver_cond);
        pthread_mutex_unlock(&d->saver_lock);
        pthread_join(d->saver, NULL);
    }
    pthread_mutex_lock(&d->saver_lock);
    if (d->bg_joinable) {
        pthread_join(d->bg_thread, NULL);
        d->bg_joinable = 0;
    }
    pthread_mutex_unlock(&d->saver_lock);
    /* Replication threads use the engine and the log: end them first. */
    repl_destroy(d->repl);
    d->repl = NULL;
    if (d->aof != NULL) {
        if (aof_close(d->aof) != 0) {
            akvd_log(d, AKVD_LOG_ERROR, "append-only log: the last write failed before the stop");
        }
        d->aof = NULL;
    }
    if (d->cfg.snapshot_path != NULL && d->cfg.save_on_stop) {
        save_now(d);
    }
    if (d->cfg.store.shm_path != NULL) {
        akvd_log(d, AKVD_LOG_INFO, "cache memory kept in %s for a warm restart", d->cfg.store.shm_path);
    }
    free_all(d);
}
