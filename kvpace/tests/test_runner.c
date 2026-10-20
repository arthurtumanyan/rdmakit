/*
 * test_runner.c - runner timing tests with a synthetic driver.
 *
 * The driver sleeps for a configurable service time instead of using a network, which makes the
 * expected throughput and latency analytically known:
 *   - closed loop: throughput is bounded by 1 / service time,
 *   - open loop below saturation: achieved rate equals the offered rate,
 *   - open loop above saturation: achieved rate equals 1 / service time and latency measured from the
 *     intended send time grows with the backlog (coordinated omission is not hidden),
 *   - the per-second series sums to the number of recorded operations.
 *
 * A synthetic asynchronous driver completes every request a fixed latency after it was submitted, with
 * unlimited parallelism, so that the multiplexed runner has analytic expectations too:
 *   - closed loop: throughput = connections * pipeline / latency,
 *   - open loop below capacity: achieved rate equals the offered rate, latency equals the fixed latency,
 *   - open loop above capacity: achieved rate equals the capacity, and requests waiting for a free
 *     pipeline slot keep their intended send time (latency grows with the backlog),
 *   - count mode: exactly the requested number of operations per connection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

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

static long service_ns;

static void sleep_service(void) {
    struct timespec ts = {.tv_sec = service_ns / 1000000000L, .tv_nsec = service_ns % 1000000000L};
    nanosleep(&ts, NULL);
}

static pace_conn *fake_connect(const char *host, const char *port) {
    (void)host;
    (void)port;
    return (pace_conn *)malloc(1);
}

static int fake_put(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen) {
    (void)c;
    (void)key;
    (void)klen;
    (void)val;
    (void)vlen;
    sleep_service();
    return PACE_OK;
}

static int fake_get(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen) {
    (void)c;
    (void)key;
    (void)klen;
    (void)buf;
    (void)cap;
    sleep_service();
    *vlen = 0;
    return PACE_OK;
}

static void fake_close(pace_conn *c) {
    free(c);
}

/* ---- synthetic asynchronous driver ---------------------------------------------------------------- */

static long async_latency_ns;

#define FAKE_RING 4096

struct pace_aconn {
    int tfd;
    uint64_t due[FAKE_RING]; /* completion times of outstanding requests, in submission order */
    unsigned head;
    unsigned count;
};

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Arms the timer for the oldest outstanding completion (requests complete in submission order). */
static void fake_timer(pace_aconn *c) {
    struct itimerspec its = {0};
    if (c->count > 0) {
        uint64_t d = c->due[c->head];
        its.it_value.tv_sec = (time_t)(d / 1000000000ull);
        its.it_value.tv_nsec = (long)(d % 1000000000ull);
    }
    timerfd_settime(c->tfd, TFD_TIMER_ABSTIME, &its, NULL);
}

static pace_aconn *afake_connect(const char *host, const char *port) {
    (void)host;
    (void)port;
    pace_aconn *c = calloc(1, sizeof(*c));
    c->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    return c;
}

static int afake_fd(pace_aconn *c) {
    return c->tfd;
}

static unsigned afake_window(pace_aconn *c) {
    (void)c;
    return FAKE_RING - 1;
}

static int afake_submit(pace_aconn *c, const pace_req *r) {
    (void)r;
    c->due[(c->head + c->count) % FAKE_RING] = mono_ns() + (uint64_t)async_latency_ns;
    c->count++;
    if (c->count == 1) {
        fake_timer(c);
    }
    return PACE_OK;
}

static int afake_io(pace_aconn *c) {
    uint64_t exp;
    if (read(c->tfd, &exp, sizeof(exp)) < 0) {
        /* not expired yet */
    }
    return PACE_OK;
}

static int afake_next(pace_aconn *c, pace_reply *out) {
    if (c->count == 0 || c->due[c->head] > mono_ns()) {
        fake_timer(c);
        return 0;
    }
    c->head = (c->head + 1) % FAKE_RING;
    c->count--;
    out->status = PACE_OK;
    out->hits = 1;
    return 1;
}

static unsigned afake_events(pace_aconn *c) {
    (void)c;
    return EPOLLIN;
}

static int afake_arm(pace_aconn *c) {
    return c->count > 0 && c->due[c->head] <= mono_ns();
}

static void afake_close(pace_aconn *c) {
    close(c->tfd);
    free(c);
}

static const pace_adriver fake_async = {
    .connect = afake_connect,
    .fd = afake_fd,
    .window = afake_window,
    .submit = afake_submit,
    .io = afake_io,
    .next = afake_next,
    .events = afake_events,
    .arm = afake_arm,
    .close = afake_close,
};

static const pace_driver fake_driver = {.name = "fake",
                                        .connect = fake_connect,
                                        .put = fake_put,
                                        .get = fake_get,
                                        .close = fake_close,
                                        .async = &fake_async};

static pace_config base_config(void) {
    pace_config c;
    pace_config_default(&c);
    c.driver = &fake_driver;
    c.host = "x";
    c.port = "0";
    c.warmup_s = 0.2;
    c.duration_s = 2.0;
    c.wl.nkeys = 1000;
    memset(c.wl.weight, 0, sizeof(c.wl.weight));
    c.wl.weight[PACE_OP_GET] = 1.0;
    c.wl.vmin = c.wl.vmax = 16;
    c.seed = 3;
    c.preload = 0;
    return c;
}

static double rate_of(const pace_result *r) {
    return (double)(r->gets + r->puts) / r->seconds;
}

static uint64_t series_sum(const pace_result *r) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < PACE_MAX_SERIES; i++) {
        sum += r->series[i];
    }
    return sum;
}

static pace_result result;

static void test_closed_loop(void) {
    service_ns = 2000000;
    pace_config cfg = base_config();
    EXPECT(pace_run(&cfg, &result) == 0, "run");
    double rate = rate_of(&result);
    EXPECT(rate > 300.0 && rate < 505.0, "closed-loop rate %.1f, expected below 500", rate);
    EXPECT(pace_hist_mean(&result.lat[PACE_OP_GET]) >= 2.0e6, "mean latency %.0f ns below the service time",
           pace_hist_mean(&result.lat[PACE_OP_GET]));
    EXPECT(series_sum(&result) == result.gets + result.puts, "series sum %llu vs ops %llu",
           (unsigned long long)series_sum(&result), (unsigned long long)(result.gets + result.puts));
}

static void test_open_below_saturation(pace_arrival arrival) {
    service_ns = 1000000;
    pace_config cfg = base_config();
    cfg.rate = 200.0;
    cfg.arrival = arrival;
    cfg.threads = 2;
    EXPECT(pace_run(&cfg, &result) == 0, "run");
    double rate = rate_of(&result);
    EXPECT(rate > 170.0 && rate < 230.0, "open-loop rate %.1f, offered 200 (arrival %d)", rate, arrival);
    double mean_ms = pace_hist_mean(&result.lat[PACE_OP_GET]) / 1e6;
    EXPECT(mean_ms >= 1.0 && mean_ms < 6.0, "mean latency %.2f ms (service 1 ms, arrival %d)", mean_ms,
           arrival);
    EXPECT(series_sum(&result) == result.gets + result.puts, "series sum");
}

static void test_open_overload(void) {
    service_ns = 5000000;
    pace_config cfg = base_config();
    cfg.rate = 400.0;
    cfg.duration_s = 2.0;
    EXPECT(pace_run(&cfg, &result) == 0, "run");
    double rate = rate_of(&result);
    EXPECT(rate > 150.0 && rate < 215.0, "achieved rate %.1f, expected about 200 (saturation)", rate);
    double p99_ms = (double)pace_hist_quantile(&result.lat[PACE_OP_GET], 0.99) / 1e6;
    EXPECT(p99_ms > 200.0, "p99 latency %.1f ms must include the backlog (> 200 ms)", p99_ms);
    EXPECT(result.max_lag_ns > 300000000ull, "max lag %.1f ms", (double)result.max_lag_ns / 1e6);
    double svc_ms = pace_hist_mean(&result.svc_lat) / 1e6;
    EXPECT(svc_ms >= 5.0 && svc_ms < 7.0, "service time %.2f ms must exclude the backlog", svc_ms);
}

static void test_async_closed(void) {
    async_latency_ns = 2000000;
    pace_config cfg = base_config();
    cfg.conns_per_thread = 4;
    cfg.pipeline = 8;
    EXPECT(pace_run(&cfg, &result) == 0, "async run");
    double rate = rate_of(&result);
    /* 4 connections x 8 in flight / 2 ms = 16,000 operations per second */
    EXPECT(rate > 12000.0 && rate < 16200.0, "multiplexed closed-loop rate %.0f, expected about 16000", rate);
    double mean_ms = pace_hist_mean(&result.lat[PACE_OP_GET]) / 1e6;
    EXPECT(mean_ms >= 2.0 && mean_ms < 3.0, "mean latency %.2f ms (fixed latency 2 ms)", mean_ms);
    EXPECT(series_sum(&result) == result.gets, "series sum");
}

static void test_async_open(void) {
    async_latency_ns = 2000000;
    pace_config cfg = base_config();
    cfg.conns_per_thread = 2;
    cfg.pipeline = 4; /* capacity 2 x 4 / 2 ms = 4,000 per second */
    cfg.rate = 2000.0;
    cfg.threads = 2;
    EXPECT(pace_run(&cfg, &result) == 0, "async open run");
    double rate = rate_of(&result);
    EXPECT(rate > 1800.0 && rate < 2200.0, "open-loop rate %.0f, offered 2000", rate);
    double p50_ms = (double)pace_hist_quantile(&result.lat[PACE_OP_GET], 0.5) / 1e6;
    EXPECT(p50_ms >= 2.0 && p50_ms < 3.0, "median latency %.2f ms below capacity", p50_ms);

    cfg.threads = 1;
    cfg.rate = 8000.0; /* twice the capacity of one thread's 2 x 4 slots */
    EXPECT(pace_run(&cfg, &result) == 0, "async overload run");
    rate = rate_of(&result);
    EXPECT(rate > 3500.0 && rate < 4100.0, "achieved rate %.0f, expected about 4000 (capacity)", rate);
    double p99_ms = (double)pace_hist_quantile(&result.lat[PACE_OP_GET], 0.99) / 1e6;
    EXPECT(p99_ms > 200.0, "p99 latency %.1f ms must include the wait for a pipeline slot", p99_ms);
    double svc_ms = pace_hist_mean(&result.svc_lat) / 1e6;
    EXPECT(svc_ms >= 2.0 && svc_ms < 3.0, "service time %.2f ms excludes the backlog", svc_ms);
}

static void test_count_mode(void) {
    async_latency_ns = 100000;
    pace_config cfg = base_config();
    cfg.conns_per_thread = 3;
    cfg.pipeline = 4;
    cfg.threads = 2;
    cfg.requests = 250;
    EXPECT(pace_run(&cfg, &result) == 0, "count run");
    EXPECT(result.gets == 2 * 3 * 250, "count mode: %llu operations, expected 1500",
           (unsigned long long)result.gets);
    service_ns = 100000;
    cfg.conns_per_thread = 1;
    cfg.pipeline = 1;
    cfg.requests = 400;
    EXPECT(pace_run(&cfg, &result) == 0 && result.gets == 800, "synchronous count mode: %llu operations",
           (unsigned long long)result.gets);
}

int main(void) {
    test_closed_loop();
    test_async_closed();
    test_async_open();
    test_count_mode();
    test_open_below_saturation(PACE_ARRIVAL_CONSTANT);
    test_open_below_saturation(PACE_ARRIVAL_POISSON);
    test_open_overload();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_runner: ok");
    return 0;
}
