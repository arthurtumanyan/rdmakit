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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static const pace_driver fake_driver = {
    .name = "fake", .connect = fake_connect, .put = fake_put, .get = fake_get, .close = fake_close};

static pace_config base_config(void) {
    return (pace_config){
        .driver = &fake_driver,
        .host = "x",
        .port = "0",
        .threads = 1,
        .warmup_s = 0.2,
        .duration_s = 2.0,
        .nkeys = 1000,
        .dist = PACE_DIST_UNIFORM,
        .value_size = 16,
        .read_ratio = 1.0,
        .seed = 3,
        .preload = 0,
    };
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
    EXPECT(pace_hist_mean(&result.get_lat) >= 2.0e6, "mean latency %.0f ns below the service time",
           pace_hist_mean(&result.get_lat));
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
    double mean_ms = pace_hist_mean(&result.get_lat) / 1e6;
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
    double p99_ms = (double)pace_hist_quantile(&result.get_lat, 0.99) / 1e6;
    EXPECT(p99_ms > 200.0, "p99 latency %.1f ms must include the backlog (> 200 ms)", p99_ms);
    EXPECT(result.max_lag_ns > 300000000ull, "max lag %.1f ms", (double)result.max_lag_ns / 1e6);
    double svc_ms = pace_hist_mean(&result.svc_lat) / 1e6;
    EXPECT(svc_ms >= 5.0 && svc_ms < 7.0, "service time %.2f ms must exclude the backlog", svc_ms);
}

int main(void) {
    test_closed_loop();
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
