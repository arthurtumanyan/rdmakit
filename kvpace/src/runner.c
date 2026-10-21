/* runner.c - benchmark runner, see runner.h. */
#include "runner.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include "proc.h"
#include "rng.h"

/* State shared by all worker threads. */
struct shared {
    pace_config *cfg;
    atomic_int measuring;
    atomic_uint_fast64_t measure_start_ns;
    atomic_int stop;
    atomic_int failed;
    atomic_uint ready;
    atomic_int go;
    atomic_uint finished; /* count mode: workers done */
};

struct worker {
    struct shared *sh;
    unsigned id;
    pthread_t thread;
    pace_hist lat[PACE_OP_COUNT];
    uint64_t ops_of[PACE_OP_COUNT];
    pace_hist svc_lat;
    pace_hist req_lat;
    pace_kvcache_stats akv;
    uint64_t misses;
    uint64_t mget_keys;
    uint64_t mget_hits;
    uint64_t errors;
    uint64_t max_lag_ns;
    uint64_t series[PACE_MAX_SERIES];
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_s(double s) {
    struct timespec ts = {.tv_sec = (time_t)s, .tv_nsec = (long)((s - (double)(time_t)s) * 1e9)};
    nanosleep(&ts, NULL);
}

uint64_t pace_result_ops(const pace_result *r) {
    uint64_t n = 0;
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        n += r->ops_of[i];
    }
    return n;
}

void pace_config_default(pace_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->driver = &pace_driver_resp;
    cfg->host = "127.0.0.1";
    cfg->port = "6379";
    cfg->threads = 1;
    cfg->conns_per_thread = 1;
    cfg->pipeline = 1;
    cfg->warmup_s = 2.0;
    cfg->duration_s = 10.0;
    pace_wl_default(&cfg->wl);
    cfg->seed = 1;
    cfg->preload = 1;
}

size_t pace_key_text(const pace_config *cfg, uint64_t idx, int counter, char *out) {
    size_t n = pace_wl_key_text(&cfg->wl, idx, out);
    if (counter) {
        /* prefix, 'c', digits: a namespace the other operations never touch */
        memmove(out + cfg->wl.prefix_len + 1, out + cfg->wl.prefix_len, n - cfg->wl.prefix_len);
        out[cfg->wl.prefix_len] = 'c';
        n++;
    }
    return n;
}

void pace_make_value(uint64_t idx, unsigned size, unsigned char *buf) {
    uint64_t state = idx;
    for (unsigned i = 0; i < size; i += 8) {
        uint64_t v = pace_splitmix64(&state);
        unsigned n = size - i < 8 ? size - i : 8;
        memcpy(buf + i, &v, n);
    }
}

/*
 * Waits until the monotonic time reaches target_ns. Long waits use nanosleep in slices of at most 50 ms
 * (so a stop request is noticed); the final 100 us are spun for accuracy. With cfg->spin the whole wait
 * is spun, which removes sleep wake-up jitter at the cost of one busy CPU per thread.
 */
static void wait_until(const struct shared *sh, uint64_t target_ns) {
    for (;;) {
        uint64_t now = now_ns();
        if (now >= target_ns || atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
            return;
        }
        uint64_t left = target_ns - now;
        if (left > 200000 && !sh->cfg->spin) {
            uint64_t nap = left - 100000;
            if (nap > 50000000) {
                nap = 50000000;
            }
            struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)nap};
            nanosleep(&ts, NULL);
        }
    }
}

static void pin_self(const pace_config *cfg, unsigned id) {
    if (cfg->npin == 0) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cfg->pin_cpus[id % cfg->npin], &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* Records a completed unit of work. */
static void record(struct worker *w, pace_op_type type, int rc, uint64_t intended, uint64_t sent,
                   uint64_t done, unsigned hits, unsigned nkeys) {
    struct shared *sh = w->sh;
    if (!atomic_load_explicit(&sh->measuring, memory_order_relaxed)) {
        return;
    }
    if (rc == PACE_ERR) {
        w->errors++;
        return;
    }
    pace_hist_record(&w->svc_lat, done - sent);
    if (sent > intended && sent - intended > w->max_lag_ns) {
        w->max_lag_ns = sent - intended;
    }
    uint64_t start = atomic_load_explicit(&sh->measure_start_ns, memory_order_relaxed);
    if (done >= start && (done - start) / 1000000000ull < PACE_MAX_SERIES) {
        w->series[(done - start) / 1000000000ull]++;
    }
    pace_hist_record(&w->lat[type], done - intended);
    w->ops_of[type]++;
    if (type == PACE_OP_GET && rc == PACE_NOTFOUND) {
        w->misses++;
    }
    if (type == PACE_OP_MGET) {
        w->mget_keys += nkeys;
        w->mget_hits += hits;
    }
}

/* Next intended send time of the open loop. */
static uint64_t next_arrival(const pace_config *cfg, pace_rng *rng, double mean_interval_ns) {
    double step = mean_interval_ns;
    if (cfg->arrival == PACE_ARRIVAL_POISSON) {
        step = -log(1.0 - pace_rng_double(rng)) * mean_interval_ns;
    }
    return (uint64_t)step;
}

/* Preloads the share of thread id: keys id, id + threads, id + 2 * threads, ... Returns 0 or -1. */
static int preload(struct worker *w, pace_conn *conn, unsigned char *vbuf) {
    const pace_config *cfg = w->sh->cfg;
    char key[PACE_KEY_MAX + 1];
    unsigned vsize = pace_wl_vsize_max(&cfg->wl);
    pace_rng rng;
    pace_rng_seed(&rng, cfg->seed ^ 0x5bd1e995u ^ w->id);
    for (uint64_t k = w->id; k < cfg->wl.nkeys; k += cfg->threads) {
        size_t kl = pace_key_text(cfg, k, 0, key);
        unsigned vs = cfg->wl.vmode == PACE_VSIZE_FIXED ? vsize : pace_wl_vsize(&cfg->wl, &rng);
        pace_make_value(k, vs, vbuf);
        if (cfg->driver->put(conn, key, kl, vbuf, vs) != PACE_OK) {
            fprintf(stderr, "worker %u: preload of key %llu failed\n", w->id, (unsigned long long)k);
            return -1;
        }
    }
    return 0;
}

static int wait_for_go(struct shared *sh) {
    atomic_fetch_add(&sh->ready, 1);
    while (!atomic_load(&sh->go) && !atomic_load(&sh->stop)) {
        sleep_s(0.001);
    }
    return !atomic_load(&sh->stop);
}

/* ---- one connection per thread, synchronous interface -------------------------------------------------- */

static int sync_op(const pace_config *cfg, pace_conn *conn, const pace_op *op, unsigned char *vbuf,
                   unsigned char *rbuf, size_t rcap, unsigned *hits) {
    const pace_driver *drv = cfg->driver;
    char key[PACE_KEY_MAX + 1];
    size_t kl = pace_key_text(cfg, op->key, op->type == PACE_OP_INCR, key);
    size_t vlen = 0;
    switch (op->type) {
    case PACE_OP_GET:
        return drv->get(conn, key, kl, rbuf, rcap, &vlen);
    case PACE_OP_SET:
    case PACE_OP_INSERT:
        pace_make_value(op->key, op->value_size, vbuf);
        return drv->put(conn, key, kl, vbuf, op->value_size);
    case PACE_OP_DEL:
        return drv->del(conn, key, kl);
    case PACE_OP_INCR:
        return drv->incr(conn, key, kl);
    case PACE_OP_RMW: {
        int rc = drv->get(conn, key, kl, rbuf, rcap, &vlen);
        if (rc == PACE_ERR) {
            return rc;
        }
        pace_make_value(op->key, op->value_size, vbuf);
        return drv->put(conn, key, kl, vbuf, op->value_size);
    }
    case PACE_OP_MGET: {
        static __thread char keys[PACE_MGET_MAX][PACE_KEY_MAX + 1];
        const char *kp[PACE_MGET_MAX];
        size_t kls[PACE_MGET_MAX];
        for (unsigned i = 0; i < op->nkeys; i++) {
            kls[i] = pace_key_text(cfg, op->keys[i], 0, keys[i]);
            kp[i] = keys[i];
        }
        return drv->mget(conn, kp, kls, op->nkeys, hits);
    }
    default:
        return PACE_ERR;
    }
}

static void *sync_main(void *arg) {
    struct worker *w = arg;
    struct shared *sh = w->sh;
    pace_config *cfg = sh->cfg;
    const pace_driver *drv = cfg->driver;
    pin_self(cfg, w->id);

    pace_conn *conn = drv->connect(cfg->host, cfg->port);
    if (conn == NULL) {
        fprintf(stderr, "worker %u: connect to %s:%s failed\n", w->id, cfg->host, cfg->port);
        atomic_store(&sh->failed, 1);
    }
    unsigned vmax = pace_wl_vsize_max(&cfg->wl);
    size_t rcap = (size_t)vmax + 64;
    unsigned char *vbuf = malloc(vmax);
    unsigned char *rbuf = malloc(rcap);
    if (vbuf == NULL || rbuf == NULL) {
        atomic_store(&sh->failed, 1);
    }
    pace_kvcache_state *kvc = NULL;
    if (cfg->scenario == PACE_SCENARIO_KVCACHE) {
        kvc = pace_kvcache_create(&cfg->kvcache, w->id, cfg->seed + w->id * 0x2545f4914f6cdd1dull);
        if (kvc == NULL) {
            fprintf(stderr, "worker %u: invalid kvcache parameters\n", w->id);
            atomic_store(&sh->failed, 1);
        }
    }
    if (conn != NULL && vbuf != NULL && cfg->preload && kvc == NULL && preload(w, conn, vbuf) != 0) {
        atomic_store(&sh->failed, 1);
    }
    int ok = wait_for_go(sh);

    pace_rng rng;
    pace_rng_seed(&rng, cfg->seed + w->id * 0x9e3779b97f4a7c15ull);
    pace_wl_thread wt;
    pace_wl_thread_init(&cfg->wl, &wt, w->id, cfg->threads);
    int open_loop = cfg->rate > 0.0;
    double mean_interval_ns = open_loop ? 1e9 * (double)cfg->threads / cfg->rate : 0.0;
    uint64_t next_intended = 0;
    if (open_loop) {
        /* Stagger thread schedules so that arrivals are spread over one interval. */
        next_intended = now_ns() + (uint64_t)(mean_interval_ns * (double)w->id / (double)cfg->threads);
    }
    uint64_t done = 0;
    pace_op op;
    while (ok && conn != NULL && vbuf != NULL && (cfg->scenario != PACE_SCENARIO_KVCACHE || kvc != NULL) &&
           !atomic_load(&sh->failed) && !atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
        if (cfg->requests > 0 && done >= cfg->requests) {
            break;
        }
        pace_wl_next(&cfg->wl, &wt, &rng, &op);
        uint64_t intended;
        if (open_loop) {
            intended = next_intended;
            next_intended += next_arrival(cfg, &rng, mean_interval_ns);
            wait_until(sh, intended);
            if (atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
                break;
            }
        }
        uint64_t t0 = now_ns();
        if (!open_loop) {
            intended = t0;
        }
        unsigned hits = 0;
        if (kvc != NULL) {
            pace_kvcache_stats req_stats = {0};
            int rc = pace_kvcache_request(kvc, drv, conn, &req_stats);
            uint64_t t1 = now_ns();
            done++;
            if (!atomic_load_explicit(&sh->measuring, memory_order_relaxed)) {
                continue;
            }
            if (rc == PACE_ERR) {
                w->errors++;
                continue;
            }
            pace_hist_record(&w->svc_lat, t1 - t0);
            uint64_t start = atomic_load_explicit(&sh->measure_start_ns, memory_order_relaxed);
            if (t1 >= start && (t1 - start) / 1000000000ull < PACE_MAX_SERIES) {
                w->series[(t1 - start) / 1000000000ull]++;
            }
            if (t0 > intended && t0 - intended > w->max_lag_ns) {
                w->max_lag_ns = t0 - intended;
            }
            pace_hist_record(&w->req_lat, t1 - intended);
            w->akv.requests += req_stats.requests;
            w->akv.block_gets += req_stats.block_gets;
            w->akv.block_hits += req_stats.block_hits;
            w->akv.block_puts += req_stats.block_puts;
            w->akv.bytes_get += req_stats.bytes_get;
            w->akv.bytes_put += req_stats.bytes_put;
            w->akv.corrupt += req_stats.corrupt;
            continue;
        }
        int rc = sync_op(cfg, conn, &op, vbuf, rbuf, rcap, &hits);
        uint64_t t1 = now_ns();
        done++;
        record(w, op.type, rc, intended, t0, t1, hits, op.nkeys);
    }
    if (cfg->requests > 0) {
        atomic_fetch_add(&sh->finished, 1);
    }
    if (conn != NULL) {
        drv->close(conn);
    }
    pace_kvcache_destroy(kvc);
    free(vbuf);
    free(rbuf);
    return NULL;
}

/* ---- many connections per thread, asynchronous interface ----------------------------------------------- */

struct inflight {
    uint64_t intended;
    uint64_t sent;
    uint8_t type;
    uint8_t parts; /* driver replies still expected (RMW: 2) */
    uint16_t nkeys;
    unsigned hits;
    int rc;
};

struct aslot {
    pace_aconn *c;
    struct inflight *ring;
    unsigned head;
    unsigned count;
    unsigned depth;
    uint64_t issued;
    uint64_t completed;
    unsigned events; /* registered epoll events */
};

/* Encodes one operation on connection s. Returns 0 or -1. */
static int async_submit(const pace_config *cfg, struct aslot *s, const pace_op *op, unsigned char *vbuf,
                        uint64_t intended) {
    const pace_adriver *ad = cfg->driver->async;
    char key[PACE_KEY_MAX + 1];
    size_t kl = pace_key_text(cfg, op->key, op->type == PACE_OP_INCR, key);
    pace_req r = {.key = key, .klen = kl};
    uint8_t parts = 1;
    int rc;
    switch (op->type) {
    case PACE_OP_SET:
    case PACE_OP_INSERT:
        pace_make_value(op->key, op->value_size, vbuf);
        r.type = PACE_OP_SET;
        r.val = vbuf;
        r.vlen = op->value_size;
        rc = ad->submit(s->c, &r);
        break;
    case PACE_OP_RMW:
        r.type = PACE_OP_GET;
        rc = ad->submit(s->c, &r);
        if (rc == PACE_OK) {
            pace_make_value(op->key, op->value_size, vbuf);
            r.type = PACE_OP_SET;
            r.val = vbuf;
            r.vlen = op->value_size;
            rc = ad->submit(s->c, &r);
            parts = 2;
        }
        break;
    case PACE_OP_MGET: {
        static __thread char keys[PACE_MGET_MAX][PACE_KEY_MAX + 1];
        const char *kp[PACE_MGET_MAX];
        size_t kls[PACE_MGET_MAX];
        for (unsigned i = 0; i < op->nkeys; i++) {
            kls[i] = pace_key_text(cfg, op->keys[i], 0, keys[i]);
            kp[i] = keys[i];
        }
        r.type = PACE_OP_MGET;
        r.keys = kp;
        r.klens = kls;
        r.nkeys = op->nkeys;
        rc = ad->submit(s->c, &r);
        break;
    }
    default:
        r.type = op->type;
        rc = ad->submit(s->c, &r);
        break;
    }
    if (rc != PACE_OK) {
        return -1;
    }
    struct inflight *f = &s->ring[(s->head + s->count) % s->depth];
    *f = (struct inflight){.intended = intended,
                           .sent = now_ns(),
                           .type = (uint8_t)op->type,
                           .parts = parts,
                           .nkeys = (uint16_t)op->nkeys,
                           .rc = PACE_OK};
    s->count++;
    s->issued++;
    return 0;
}

/* Takes available replies of s and records completed operations. Returns 0 or -1. */
static int async_drain(struct worker *w, struct aslot *s) {
    const pace_adriver *ad = w->sh->cfg->driver->async;
    for (;;) {
        pace_reply rep;
        int n = ad->next(s->c, &rep);
        if (n == 0) {
            return 0;
        }
        if (n < 0 || s->count == 0) {
            return -1;
        }
        struct inflight *f = &s->ring[s->head];
        if (rep.status == PACE_ERR) {
            f->rc = PACE_ERR;
        } else if (f->rc != PACE_ERR && f->parts == 1) {
            f->rc = rep.status;
        }
        f->hits += rep.hits;
        if (--f->parts > 0) {
            continue;
        }
        uint64_t t = now_ns();
        record(w, (pace_op_type)f->type, f->rc, f->intended, f->sent, t, f->hits, f->nkeys);
        s->head = (s->head + 1) % s->depth;
        s->count--;
        s->completed++;
    }
}

struct backlog {
    uint64_t *t;
    size_t head;
    size_t count;
    size_t cap;
};

static int backlog_push(struct backlog *b, uint64_t t) {
    if (b->count == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        uint64_t *n = malloc(cap * sizeof(*n));
        if (n == NULL) {
            return -1;
        }
        for (size_t i = 0; i < b->count; i++) {
            n[i] = b->t[(b->head + i) % b->cap];
        }
        free(b->t);
        b->t = n;
        b->head = 0;
        b->cap = cap;
    }
    b->t[(b->head + b->count) % b->cap] = t;
    b->count++;
    return 0;
}

static void *async_main(void *arg) {
    struct worker *w = arg;
    struct shared *sh = w->sh;
    pace_config *cfg = sh->cfg;
    const pace_adriver *ad = cfg->driver->async;
    pin_self(cfg, w->id);
    unsigned nconn = cfg->conns_per_thread ? cfg->conns_per_thread : 1;
    struct aslot *slots = calloc(nconn, sizeof(*slots));
    unsigned vmax = pace_wl_vsize_max(&cfg->wl);
    unsigned char *vbuf = malloc(vmax);
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct backlog bl = {0};
    int fail = slots == NULL || vbuf == NULL || epfd < 0;
    int needs_two = cfg->wl.weight[PACE_OP_RMW] > 0.0;
    for (unsigned i = 0; i < nconn && !fail; i++) {
        struct aslot *s = &slots[i];
        s->c = ad->connect(cfg->host, cfg->port);
        if (s->c == NULL) {
            fprintf(stderr, "worker %u: connect %u to %s:%s failed\n", w->id, i, cfg->host, cfg->port);
            fail = 1;
            break;
        }
        unsigned depth = cfg->pipeline ? cfg->pipeline : 1;
        unsigned win = ad->window(s->c);
        if (win != 0) {
            unsigned lim = needs_two ? win / 2 : win;
            if (lim == 0) {
                lim = 1;
            }
            depth = depth < lim ? depth : lim;
        }
        s->depth = depth;
        s->ring = calloc(depth, sizeof(*s->ring));
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = i};
        if (s->ring == NULL || epoll_ctl(epfd, EPOLL_CTL_ADD, ad->fd(s->c), &ev) != 0) {
            fail = 1;
        }
        s->events = EPOLLIN;
    }
    /* Preload over a synchronous connection, which every driver offers. */
    if (!fail && cfg->preload) {
        pace_conn *pc = cfg->driver->connect(cfg->host, cfg->port);
        if (pc == NULL || preload(w, pc, vbuf) != 0) {
            fail = 1;
        }
        if (pc != NULL) {
            cfg->driver->close(pc);
        }
    }
    if (fail) {
        atomic_store(&sh->failed, 1);
    }
    int ok = wait_for_go(sh) && !fail;

    pace_rng rng;
    pace_rng_seed(&rng, cfg->seed + w->id * 0x9e3779b97f4a7c15ull);
    pace_wl_thread wt;
    pace_wl_thread_init(&cfg->wl, &wt, w->id, cfg->threads);
    int open_loop = cfg->rate > 0.0;
    double mean_interval_ns = open_loop ? 1e9 * (double)cfg->threads / cfg->rate : 0.0;
    uint64_t next_intended = open_loop ? now_ns() + (uint64_t)(mean_interval_ns * w->id / cfg->threads) : 0;
    unsigned rr = 0;
    pace_op op;
    struct epoll_event evs[256];
    while (ok && !atomic_load(&sh->failed) && !atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
        uint64_t now = now_ns();
        int all_done = cfg->requests > 0;
        if (open_loop) {
            while (next_intended <= now) {
                if (backlog_push(&bl, next_intended) != 0) {
                    atomic_store(&sh->failed, 1);
                    break;
                }
                next_intended += next_arrival(cfg, &rng, mean_interval_ns);
            }
            /* Requests waiting for a free pipeline slot keep their intended send time. */
            for (unsigned tries = 0; bl.count > 0 && tries < nconn;) {
                struct aslot *s = &slots[rr % nconn];
                if (s->count < s->depth && (cfg->requests == 0 || s->issued < cfg->requests)) {
                    pace_wl_next(&cfg->wl, &wt, &rng, &op);
                    if (async_submit(cfg, s, &op, vbuf, bl.t[bl.head]) != 0) {
                        atomic_store(&sh->failed, 1);
                        break;
                    }
                    bl.head = (bl.head + 1) % bl.cap;
                    bl.count--;
                    tries = 0;
                } else {
                    tries++;
                }
                rr++;
            }
        } else {
            for (unsigned i = 0; i < nconn; i++) {
                struct aslot *s = &slots[i];
                while (s->count < s->depth && (cfg->requests == 0 || s->issued < cfg->requests)) {
                    pace_wl_next(&cfg->wl, &wt, &rng, &op);
                    if (async_submit(cfg, s, &op, vbuf, now_ns()) != 0) {
                        atomic_store(&sh->failed, 1);
                        break;
                    }
                }
            }
        }
        int ready_now = 0;
        for (unsigned i = 0; i < nconn; i++) {
            struct aslot *s = &slots[i];
            if (ad->io(s->c) != PACE_OK || async_drain(w, s) != 0) {
                fprintf(stderr, "worker %u: connection %u failed\n", w->id, i);
                atomic_store(&sh->failed, 1);
                break;
            }
            all_done = all_done && s->completed >= cfg->requests;
            unsigned evm = ad->events(s->c);
            if (evm != s->events) {
                struct epoll_event ev = {.events = evm, .data.u32 = i};
                epoll_ctl(epfd, EPOLL_CTL_MOD, ad->fd(s->c), &ev);
                s->events = evm;
            }
            if (s->count > 0 && ad->arm(s->c) != 0) {
                ready_now = 1;
            }
            /* A free pipeline slot that may be filled now: submit again instead of waiting. Replies that
             * arrive before this drain would otherwise leave the connection idle until the next event. */
            int may_issue = cfg->requests == 0 || s->issued < cfg->requests;
            if (s->count < s->depth && may_issue && (!open_loop || bl.count > 0)) {
                ready_now = 1;
            }
        }
        if (all_done) {
            break;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000};
        if (ready_now) {
            ts.tv_nsec = 0;
        } else if (open_loop) {
            uint64_t t = now_ns();
            uint64_t left = next_intended > t ? next_intended - t : 0;
            if (bl.count > 0) {
                left = left < 50000000 ? left : 50000000;
            }
            if (left < (uint64_t)ts.tv_nsec) {
                ts.tv_nsec = (long)left;
            }
        }
        int n = epoll_pwait2(epfd, evs, 256, &ts, NULL);
        for (int k = 0; k < n; k++) {
            struct aslot *s = &slots[evs[k].data.u32];
            if (ad->io(s->c) != PACE_OK || async_drain(w, s) != 0) {
                atomic_store(&sh->failed, 1);
                break;
            }
        }
    }
    if (cfg->requests > 0) {
        atomic_fetch_add(&sh->finished, 1);
    }
    for (unsigned i = 0; slots != NULL && i < nconn; i++) {
        if (slots[i].c != NULL) {
            ad->close(slots[i].c);
        }
        free(slots[i].ring);
    }
    free(slots);
    free(bl.t);
    free(vbuf);
    if (epfd >= 0) {
        close(epfd);
    }
    return NULL;
}

/* ---- orchestration ------------------------------------------------------------------------------------- */

/* Checks that the driver offers every operation of the mix. Returns 0 or -1 with a message. */
static int check_ops(const pace_config *cfg, int async) {
    const pace_driver *d = cfg->driver;
    const pace_workload *wl = &cfg->wl;
    if (async && d->async == NULL) {
        fprintf(stderr,
                "driver %s has no asynchronous interface (needed for --conns-per-thread/--pipeline)\n",
                d->name);
        return -1;
    }
    if ((wl->weight[PACE_OP_DEL] > 0 && d->del == NULL) ||
        (wl->weight[PACE_OP_INCR] > 0 && d->incr == NULL) ||
        (wl->weight[PACE_OP_MGET] > 0 && d->mget == NULL)) {
        fprintf(stderr, "driver %s does not support every operation of the mix\n", d->name);
        return -1;
    }
    return 0;
}

int pace_run(pace_config *cfg, pace_result *res) {
    int async = cfg->conns_per_thread > 1 || cfg->pipeline > 1;
    if (cfg->scenario == PACE_SCENARIO_KVCACHE && async) {
        fprintf(stderr, "the kvcache scenario uses one connection per thread and no pipelining\n");
        return -1;
    }
    if (cfg->scenario == PACE_SCENARIO_KV && check_ops(cfg, async) != 0) {
        return -1;
    }
    char err[160];
    if (pace_wl_init(&cfg->wl, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }
    struct shared sh = {.cfg = cfg};
    atomic_init(&sh.measuring, 0);
    atomic_init(&sh.stop, 0);
    atomic_init(&sh.failed, 0);
    atomic_init(&sh.ready, 0);
    atomic_init(&sh.go, 0);
    atomic_init(&sh.finished, 0);

    struct worker *ws = calloc(cfg->threads, sizeof(*ws));
    if (ws == NULL) {
        return -1;
    }
    unsigned started = 0;
    for (unsigned i = 0; i < cfg->threads; i++) {
        ws[i].sh = &sh;
        ws[i].id = i;
        for (unsigned k = 0; k < PACE_OP_COUNT; k++) {
            pace_hist_reset(&ws[i].lat[k]);
        }
        pace_hist_reset(&ws[i].svc_lat);
        pace_hist_reset(&ws[i].req_lat);
        if (pthread_create(&ws[i].thread, NULL, async ? async_main : sync_main, &ws[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            atomic_store(&sh.failed, 1);
            break;
        }
        started++;
    }
    /* Wait until every started worker has connected and finished its preload share. */
    while (atomic_load(&sh.ready) < started && !atomic_load(&sh.failed)) {
        sleep_s(0.001);
    }
    if (atomic_load(&sh.failed)) {
        atomic_store(&sh.stop, 1);
        for (unsigned i = 0; i < started; i++) {
            pthread_join(ws[i].thread, NULL);
        }
        free(ws);
        return -1;
    }
    if (cfg->start_at > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        if (cfg->start_at > now) {
            sleep_s(cfg->start_at - now);
        }
    }
    pace_proc_sample srv_before;
    pace_proc_sample srv_after;
    int srv_ok = 0;
    uint64_t t_start;
    if (cfg->requests > 0) {
        /* Count mode: no warm-up; the measurement ends when every worker has finished. */
        srv_ok = cfg->server_pid > 0 && pace_proc_sample_pid(cfg->server_pid, &srv_before) == 0;
        t_start = now_ns();
        atomic_store(&sh.measure_start_ns, t_start);
        atomic_store(&sh.measuring, 1);
        atomic_store(&sh.go, 1);
        while (atomic_load(&sh.finished) < started && !atomic_load(&sh.failed)) {
            sleep_s(0.001);
        }
    } else {
        atomic_store(&sh.go, 1);
        sleep_s(cfg->warmup_s);
        srv_ok = cfg->server_pid > 0 && pace_proc_sample_pid(cfg->server_pid, &srv_before) == 0;
        t_start = now_ns();
        atomic_store(&sh.measure_start_ns, t_start);
        atomic_store(&sh.measuring, 1);
        sleep_s(cfg->duration_s);
    }
    atomic_store(&sh.measuring, 0);
    uint64_t t_end = now_ns();
    srv_ok = srv_ok && pace_proc_sample_pid(cfg->server_pid, &srv_after) == 0;
    atomic_store(&sh.stop, 1);

    memset(res, 0, sizeof(*res));
    for (unsigned k = 0; k < PACE_OP_COUNT; k++) {
        pace_hist_reset(&res->lat[k]);
    }
    pace_hist_reset(&res->svc_lat);
    pace_hist_reset(&res->req_lat);
    for (unsigned i = 0; i < started; i++) {
        pthread_join(ws[i].thread, NULL);
        for (unsigned k = 0; k < PACE_OP_COUNT; k++) {
            pace_hist_merge(&res->lat[k], &ws[i].lat[k]);
            res->ops_of[k] += ws[i].ops_of[k];
        }
        pace_hist_merge(&res->svc_lat, &ws[i].svc_lat);
        pace_hist_merge(&res->req_lat, &ws[i].req_lat);
        res->kvcache.requests += ws[i].akv.requests;
        res->kvcache.block_gets += ws[i].akv.block_gets;
        res->kvcache.block_hits += ws[i].akv.block_hits;
        res->kvcache.block_puts += ws[i].akv.block_puts;
        res->kvcache.bytes_get += ws[i].akv.bytes_get;
        res->kvcache.bytes_put += ws[i].akv.bytes_put;
        res->kvcache.corrupt += ws[i].akv.corrupt;
        if (ws[i].max_lag_ns > res->max_lag_ns) {
            res->max_lag_ns = ws[i].max_lag_ns;
        }
        for (unsigned k = 0; k < PACE_MAX_SERIES; k++) {
            res->series[k] += ws[i].series[k];
        }
        res->misses += ws[i].misses;
        res->mget_keys += ws[i].mget_keys;
        res->mget_hits += ws[i].mget_hits;
        res->errors += ws[i].errors;
    }
    res->gets = res->ops_of[PACE_OP_GET] + res->ops_of[PACE_OP_RMW];
    res->puts = res->ops_of[PACE_OP_SET] + res->ops_of[PACE_OP_INSERT] + res->ops_of[PACE_OP_RMW];
    if (cfg->scenario == PACE_SCENARIO_KVCACHE) {
        /* Operations of the KV-cache scenario are block operations (ops, ops_per_sec, CPU per operation). */
        res->ops_of[PACE_OP_GET] = res->kvcache.block_gets;
        res->ops_of[PACE_OP_SET] = res->kvcache.block_puts;
        res->gets = res->kvcache.block_gets;
        res->puts = res->kvcache.block_puts;
        res->misses = res->kvcache.block_gets - res->kvcache.block_hits;
    }
    res->seconds = (double)(t_end - t_start) / 1e9;
    res->nseries = (unsigned)ceil(res->seconds - 1e-9);
    if (cfg->requests == 0) {
        res->nseries = (unsigned)(res->seconds + 0.5);
    }
    if (res->nseries > PACE_MAX_SERIES) {
        res->nseries = PACE_MAX_SERIES;
    }
    if (srv_ok) {
        res->has_server = 1;
        res->server_cpu_seconds = srv_after.cpu_seconds - srv_before.cpu_seconds;
        res->server_rss_bytes = srv_after.rss_bytes;
    }
    free(ws);
    return atomic_load(&sh.failed) ? -1 : 0;
}
