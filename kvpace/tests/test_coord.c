/*
 * test_coord.c - coordinated runs (kvpace barrier and --coordinator), in one process.
 *
 * A coordinator thread waits for two generator threads, each running the closed loop of pace_run() over a
 * driver without a server. Checked: both generators leave the start hook within a few milliseconds of
 * each other; the coordinator stores both results and merges them (operations summed, a measurement
 * overlap close to the run length); a generator that reports failure, and a generator that never comes,
 * end the coordinated run with an error instead of a hang.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "coord.h"
#include "json.h"
#include "report.h"
#include "runner.h"

static int failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

/* ---- a driver without a server: every store succeeds, every lookup misses -------------------------------- */

static pace_conn *nop_connect(const char *host, const char *port) {
    (void)host;
    (void)port;
    return (pace_conn *)malloc(1);
}

static int nop_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    (void)c;
    (void)key;
    (void)klen;
    (void)val;
    (void)vlen;
    return PACE_OK;
}

static int nop_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    (void)c;
    (void)key;
    (void)klen;
    (void)buf;
    (void)cap;
    *vlen = 0;
    return PACE_NOTFOUND;
}

static void nop_close(pace_conn *c) {
    free(c);
}

static const pace_driver nop = {
    .name = "nop", .connect = nop_connect, .put = nop_put, .get = nop_get, .close = nop_close};

/* ---- coordinator and generators -------------------------------------------------------------------------- */

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct barrier_arg {
    pace_barrier *b;
    const char *out;
    double timeout;
    int rc;
};

static void *barrier_main(void *arg) {
    struct barrier_arg *a = arg;
    a->rc = pace_barrier_run(a->b, a->out, a->timeout);
    return NULL;
}

struct gen_arg {
    int port;
    int fail;       /* report failure instead of running */
    double started; /* when the start hook returned */
    int rc;
};

static int timed_start(void *arg) {
    struct gen_arg *g = ((void **)arg)[1];
    int rc = pace_coord_start(((void **)arg)[0]);
    g->started = mono();
    return rc;
}

static void *gen_main(void *arg) {
    struct gen_arg *g = arg;
    char addr[64];
    char err[256];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", g->port);
    pace_coord *c = pace_coord_connect(addr, err, sizeof(err));
    if (c == NULL) {
        g->rc = -1;
        return NULL;
    }
    if (g->fail) {
        pace_coord_failed(c);
        pace_coord_close(c);
        g->rc = 0;
        return NULL;
    }
    pace_config cfg;
    pace_config_default(&cfg);
    cfg.driver = &nop;
    cfg.threads = 1;
    cfg.warmup_s = 0.05;
    cfg.duration_s = 0.3;
    cfg.preload = 0;
    cfg.wl.nkeys = 100;
    void *hook[2] = {c, g};
    cfg.start_hook = timed_start;
    cfg.start_hook_arg = hook;
    static __thread pace_result res;
    g->rc = pace_run(&cfg, &res);
    if (g->rc == 0) {
        char *json = NULL;
        size_t len = 0;
        FILE *mf = open_memstream(&json, &len);
        pace_percentiles pc;
        pace_default_percentiles(&pc);
        char *argv[1] = {"kvpace"};
        pace_report_json(mf, &cfg, &res, &pc, "coord-test", 1, argv);
        fclose(mf);
        g->rc = pace_coord_result(c, json, len);
        free(json);
    } else {
        pace_coord_failed(c);
    }
    pace_coord_close(c);
    return NULL;
}

/* Runs a coordinator for count generators and the given generators; returns the coordinator's result. */
static int coordinated(const char *out, unsigned count, struct gen_arg *gens, unsigned ngens,
                       double timeout) {
    char err[256];
    pace_barrier *b = pace_barrier_open("127.0.0.1", "0", count, err, sizeof(err));
    EXPECT(b != NULL, "barrier: %s", err);
    if (b == NULL) {
        return -1;
    }
    struct barrier_arg ba = {.b = b, .out = out, .timeout = timeout};
    pthread_t bt;
    pthread_create(&bt, NULL, barrier_main, &ba);
    pthread_t gt[4];
    for (unsigned i = 0; i < ngens; i++) {
        gens[i].port = pace_barrier_port(b);
        pthread_create(&gt[i], NULL, gen_main, &gens[i]);
    }
    pthread_join(bt, NULL);
    /* Close the listener first, as the exit of a coordinator process does: a generator that connects after
     * the coordinated run has ended is then refused instead of waiting for a start message forever. */
    pace_barrier_close(b);
    for (unsigned i = 0; i < ngens; i++) {
        pthread_join(gt[i], NULL);
    }
    return ba.rc;
}

int main(void) {
    char dir[] = "/tmp/pace-coord-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        return 1;
    }
    char out[256];
    snprintf(out, sizeof(out), "%s/merged.json", dir);

    struct gen_arg gens[2] = {{0}};
    EXPECT(coordinated(out, 2, gens, 2, 30.0) == 0, "coordinated run");
    EXPECT(gens[0].rc == 0 && gens[1].rc == 0, "generators %d %d", gens[0].rc, gens[1].rc);
    double skew = gens[0].started > gens[1].started ? gens[0].started - gens[1].started
                                                    : gens[1].started - gens[0].started;
    EXPECT(skew < 0.05, "generators started %.3f s apart", skew);
    char err[160];
    char in1[300];
    char in2[300];
    snprintf(in1, sizeof(in1), "%s/merged-1.json", dir);
    snprintf(in2, sizeof(in2), "%s/merged-2.json", dir);
    pace_json *m = pace_json_load(out, err, sizeof(err));
    pace_json *a = pace_json_load(in1, err, sizeof(err));
    pace_json *b = pace_json_load(in2, err, sizeof(err));
    EXPECT(m != NULL && a != NULL && b != NULL, "merged and input files: %s", err);
    if (m != NULL && a != NULL && b != NULL) {
        uint64_t ops = pace_json_u64(pace_json_get(pace_json_get(m, "results"), "ops"));
        uint64_t oa = pace_json_u64(pace_json_get(pace_json_get(a, "results"), "ops"));
        uint64_t ob = pace_json_u64(pace_json_get(pace_json_get(b, "results"), "ops"));
        double overlap = pace_json_num(pace_json_get(m, "tool"), "measure_overlap_s", -1);
        EXPECT(oa > 0 && ob > 0 && ops == oa + ob, "operations summed: %llu = %llu + %llu",
               (unsigned long long)ops, (unsigned long long)oa, (unsigned long long)ob);
        EXPECT(overlap > 0.25 && overlap <= 0.31, "measured together for %.3f s of 0.3 s", overlap);
        EXPECT(pace_json_u64(pace_json_get(pace_json_get(m, "tool"), "inputs")) == 2, "two inputs");
    }
    pace_json_free(m);
    pace_json_free(a);
    pace_json_free(b);

    /* a generator reports failure: the coordinated run fails at once, the other generator is released */
    struct gen_arg failing[2] = {{.fail = 1}, {0}};
    double t0 = mono();
    EXPECT(coordinated(out, 2, failing, 2, 30.0) != 0, "a failed generator fails the run");
    EXPECT(failing[1].rc != 0, "the waiting generator does not run");
    EXPECT(mono() - t0 < 5.0, "failure noticed without waiting for the timeout");

    /* a generator never comes: the coordinator gives up at its timeout */
    struct gen_arg alone[1] = {{0}};
    t0 = mono();
    EXPECT(coordinated(out, 2, alone, 1, 0.5) != 0, "timeout with one of two generators");
    double waited = mono() - t0;
    EXPECT(waited >= 0.45 && waited < 5.0, "timed out after %.2f s", waited);

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        return 1;
    }
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_coord: ok");
    return 0;
}
