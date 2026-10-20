/* runner.c - closed-loop benchmark runner, see runner.h. */
#include "runner.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "proc.h"
#include "rng.h"
#include "zipf.h"

/* State shared by all worker threads. */
struct shared {
    const pace_config *cfg;
    const pace_zipf *zipf;
    atomic_int measuring;
    atomic_uint_fast64_t measure_start_ns;
    atomic_int stop;
    atomic_int failed;
    atomic_uint ready;
    atomic_int go;
};

struct worker {
    struct shared *sh;
    unsigned id;
    pthread_t thread;
    pace_hist get_lat;
    pace_hist put_lat;
    pace_hist svc_lat;
    pace_hist req_lat;
    pace_kvcache_stats akv;
    uint64_t gets;
    uint64_t puts;
    uint64_t misses;
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

void pace_make_key(uint64_t idx, char *out) {
    out[0] = 'k';
    for (int i = PACE_KEY_LEN - 1; i >= 1; i--) {
        out[i] = (char)('0' + idx % 10);
        idx /= 10;
    }
}

/* Fills buf with a value derived from the key index so that readers can verify content. */
static void make_value(uint64_t idx, unsigned size, unsigned char *buf) {
    uint64_t state = idx;
    for (unsigned i = 0; i < size; i += 8) {
        uint64_t v = pace_splitmix64(&state);
        unsigned n = size - i < 8 ? size - i : 8;
        memcpy(buf + i, &v, n);
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

static uint64_t next_key(const struct shared *sh, pace_rng *rng) {
    const pace_config *cfg = sh->cfg;
    if (cfg->dist == PACE_DIST_ZIPF) {
        return pace_zipf_scramble(pace_zipf_next(sh->zipf, rng), cfg->nkeys);
    }
    return pace_rng_below(rng, cfg->nkeys);
}

static void *worker_main(void *arg) {
    struct worker *w = arg;
    struct shared *sh = w->sh;
    const pace_config *cfg = sh->cfg;
    const pace_driver *drv = cfg->driver;
    pin_self(cfg, w->id);

    pace_conn *conn = drv->connect(cfg->host, cfg->port);
    if (conn == NULL) {
        fprintf(stderr, "worker %u: connect to %s:%s failed\n", w->id, cfg->host, cfg->port);
        atomic_store(&sh->failed, 1);
    }
    unsigned char *vbuf = malloc(cfg->value_size);
    unsigned char *rbuf = malloc(cfg->value_size + 64);
    if (vbuf == NULL || rbuf == NULL) {
        atomic_store(&sh->failed, 1);
    }
    pace_kvcache_state *akvc = NULL;
    if (cfg->scenario == PACE_SCENARIO_KVCACHE) {
        akvc = pace_kvcache_create(&cfg->kvcache, w->id, cfg->seed + w->id * 0x2545f4914f6cdd1dull);
        if (akvc == NULL) {
            fprintf(stderr, "worker %u: invalid kvcache parameters\n", w->id);
            atomic_store(&sh->failed, 1);
        }
    }

    /* Preload: thread id writes keys id, id + threads, id + 2 * threads, ... */
    if (conn != NULL && vbuf != NULL && cfg->preload && akvc == NULL) {
        char key[PACE_KEY_LEN];
        for (uint64_t k = w->id; k < cfg->nkeys; k += cfg->threads) {
            pace_make_key(k, key);
            make_value(k, cfg->value_size, vbuf);
            if (drv->put(conn, key, PACE_KEY_LEN, vbuf, cfg->value_size) != PACE_OK) {
                fprintf(stderr, "worker %u: preload of key %llu failed\n", w->id, (unsigned long long)k);
                atomic_store(&sh->failed, 1);
                break;
            }
        }
    }
    atomic_fetch_add(&sh->ready, 1);
    while (!atomic_load(&sh->go) && !atomic_load(&sh->stop)) {
        sleep_s(0.001);
    }

    pace_rng rng;
    pace_rng_seed(&rng, cfg->seed + w->id * 0x9e3779b97f4a7c15ull);
    char key[PACE_KEY_LEN];
    int open_loop = cfg->rate > 0.0;
    double mean_interval_ns = open_loop ? 1e9 * (double)cfg->threads / cfg->rate : 0.0;
    uint64_t next_intended = 0;
    if (open_loop) {
        /* Stagger thread schedules so that arrivals are spread over one interval. */
        next_intended = now_ns() + (uint64_t)(mean_interval_ns * (double)w->id / (double)cfg->threads);
    }
    while (conn != NULL && vbuf != NULL && (cfg->scenario != PACE_SCENARIO_KVCACHE || akvc != NULL) &&
           !atomic_load(&sh->failed) && !atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
        uint64_t idx = next_key(sh, &rng);
        int is_get = pace_rng_double(&rng) < cfg->read_ratio;
        pace_make_key(idx, key);
        uint64_t intended;
        if (open_loop) {
            intended = next_intended;
            double step = mean_interval_ns;
            if (cfg->arrival == PACE_ARRIVAL_POISSON) {
                step = -log(1.0 - pace_rng_double(&rng)) * mean_interval_ns;
            }
            next_intended += (uint64_t)step;
            wait_until(sh, intended);
            if (atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
                break;
            }
        }
        uint64_t t0 = now_ns();
        if (!open_loop) {
            intended = t0;
        }
        int rc;
        size_t vlen = 0;
        pace_kvcache_stats req_stats = {0};
        if (akvc != NULL) {
            rc = pace_kvcache_request(akvc, drv, conn, &req_stats);
        } else if (is_get) {
            rc = drv->get(conn, key, PACE_KEY_LEN, rbuf, cfg->value_size + 64, &vlen);
        } else {
            make_value(idx, cfg->value_size, vbuf);
            rc = drv->put(conn, key, PACE_KEY_LEN, vbuf, cfg->value_size);
        }
        uint64_t t1 = now_ns();
        if (!atomic_load_explicit(&sh->measuring, memory_order_relaxed)) {
            continue;
        }
        if (rc == PACE_ERR) {
            w->errors++;
            continue;
        }
        pace_hist_record(&w->svc_lat, t1 - t0);
        if (t0 > intended && t0 - intended > w->max_lag_ns) {
            w->max_lag_ns = t0 - intended;
        }
        uint64_t start = atomic_load_explicit(&sh->measure_start_ns, memory_order_relaxed);
        if (t1 >= start && (t1 - start) / 1000000000ull < PACE_MAX_SERIES) {
            w->series[(t1 - start) / 1000000000ull]++;
        }
        if (akvc != NULL) {
            pace_hist_record(&w->req_lat, t1 - intended);
            w->akv.requests += req_stats.requests;
            w->akv.block_gets += req_stats.block_gets;
            w->akv.block_hits += req_stats.block_hits;
            w->akv.block_puts += req_stats.block_puts;
            w->akv.bytes_get += req_stats.bytes_get;
            w->akv.bytes_put += req_stats.bytes_put;
            w->akv.corrupt += req_stats.corrupt;
        } else if (is_get) {
            pace_hist_record(&w->get_lat, t1 - intended);
            w->gets++;
            if (rc == PACE_NOTFOUND) {
                w->misses++;
            }
        } else {
            pace_hist_record(&w->put_lat, t1 - intended);
            w->puts++;
        }
    }
    if (conn != NULL) {
        drv->close(conn);
    }
    pace_kvcache_destroy(akvc);
    free(vbuf);
    free(rbuf);
    return NULL;
}

int pace_run(const pace_config *cfg, pace_result *res) {
    struct shared sh = {.cfg = cfg};
    pace_zipf zipf;
    if (cfg->dist == PACE_DIST_ZIPF) {
        if (pace_zipf_init(&zipf, cfg->nkeys, cfg->theta) != 0) {
            fprintf(stderr, "invalid zipf parameters (keys=%llu theta=%g)\n", (unsigned long long)cfg->nkeys,
                    cfg->theta);
            return -1;
        }
        sh.zipf = &zipf;
    }
    atomic_init(&sh.measuring, 0);
    atomic_init(&sh.stop, 0);
    atomic_init(&sh.failed, 0);
    atomic_init(&sh.ready, 0);
    atomic_init(&sh.go, 0);

    struct worker *ws = calloc(cfg->threads, sizeof(*ws));
    if (ws == NULL) {
        return -1;
    }
    unsigned started = 0;
    for (unsigned i = 0; i < cfg->threads; i++) {
        ws[i].sh = &sh;
        ws[i].id = i;
        pace_hist_reset(&ws[i].get_lat);
        pace_hist_reset(&ws[i].put_lat);
        pace_hist_reset(&ws[i].svc_lat);
        pace_hist_reset(&ws[i].req_lat);
        if (pthread_create(&ws[i].thread, NULL, worker_main, &ws[i]) != 0) {
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
    atomic_store(&sh.go, 1);

    sleep_s(sh.cfg->warmup_s);
    pace_proc_sample srv_before;
    pace_proc_sample srv_after;
    int srv_ok = sh.cfg->server_pid > 0 && pace_proc_sample_pid(sh.cfg->server_pid, &srv_before) == 0;
    uint64_t t_start = now_ns();
    atomic_store(&sh.measure_start_ns, t_start);
    atomic_store(&sh.measuring, 1);
    sleep_s(sh.cfg->duration_s);
    atomic_store(&sh.measuring, 0);
    uint64_t t_end = now_ns();
    srv_ok = srv_ok && pace_proc_sample_pid(sh.cfg->server_pid, &srv_after) == 0;
    atomic_store(&sh.stop, 1);

    memset(res, 0, sizeof(*res));
    pace_hist_reset(&res->get_lat);
    pace_hist_reset(&res->put_lat);
    pace_hist_reset(&res->svc_lat);
    pace_hist_reset(&res->req_lat);
    for (unsigned i = 0; i < sh.cfg->threads; i++) {
        pthread_join(ws[i].thread, NULL);
        pace_hist_merge(&res->get_lat, &ws[i].get_lat);
        pace_hist_merge(&res->put_lat, &ws[i].put_lat);
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
        res->gets += ws[i].gets;
        res->puts += ws[i].puts;
        res->misses += ws[i].misses;
        res->errors += ws[i].errors;
    }
    if (sh.cfg->scenario == PACE_SCENARIO_KVCACHE) {
        res->gets = res->kvcache.block_gets;
        res->puts = res->kvcache.block_puts;
        res->misses = res->kvcache.block_gets - res->kvcache.block_hits;
    }
    res->seconds = (double)(t_end - t_start) / 1e9;
    res->nseries = (unsigned)(res->seconds + 0.5);
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
