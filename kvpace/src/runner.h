/*
 * runner.h - benchmark runner (closed loop and open loop).
 *
 * One thread per connection; each thread has one request in flight.
 *
 * Closed loop (rate = 0): a thread issues the next request when the previous one completes.
 *
 * Open loop (rate > 0): requests are scheduled at a fixed aggregate rate, split evenly over the
 * threads, with constant or exponentially distributed (Poisson) inter-arrival times. Latency is
 * measured from the intended send time to completion, so time spent waiting behind a slow response
 * is included (no coordinated omission). A thread that is behind schedule sends immediately.
 * The pure service time (actual send to completion) is reported separately.
 *
 * With scenario KVCACHE one unit of work is a whole request (a chain of block GETs followed by PUTs of the
 * missing tail, see kvcache.h) instead of a single operation. Rates, latencies and the per-second series
 * then count requests; gets, puts and misses count block operations.
 *
 * A run has three phases:
 *   1. preload: every key of the key space is written once (optional).
 *   2. warm-up: requests are issued but not recorded.
 *   3. measurement: latencies and counters are recorded.
 * Throughput is ops recorded during the measurement phase divided by its
 * wall-clock length. Operations in flight at the phase boundaries are counted
 * by the phase in which they complete.
 */
#ifndef PACE_RUNNER_H
#define PACE_RUNNER_H

#include <stdint.h>

#include "driver.h"
#include "hist.h"
#include "kvcache.h"

typedef enum { PACE_DIST_UNIFORM = 0, PACE_DIST_ZIPF = 1 } pace_dist;
typedef enum { PACE_SCENARIO_KV = 0, PACE_SCENARIO_KVCACHE = 1 } pace_scenario;
typedef enum { PACE_ARRIVAL_CONSTANT = 0, PACE_ARRIVAL_POISSON = 1 } pace_arrival;

/* Upper bound of per-second throughput samples kept per run. */
#define PACE_MAX_SERIES 3600

typedef struct pace_config {
    const pace_driver *driver;
    const char *host;
    const char *port;
    unsigned threads;
    double warmup_s;
    double duration_s;
    uint64_t nkeys;
    pace_dist dist;
    double theta;
    unsigned value_size;
    double read_ratio;
    uint64_t seed;
    int preload;
    const int *pin_cpus;
    unsigned npin;
    pace_scenario scenario;   /* KV: single-key operations; KVCACHE: prefix-chain requests (kvcache.h) */
    pace_kvcache_cfg kvcache; /* parameters of the KVCACHE scenario */
    double rate;              /* aggregate operations per second; 0 selects the closed loop */
    pace_arrival arrival;     /* inter-arrival distribution of the open loop */
    int spin;                 /* open loop: busy-wait for the intended time instead of sleeping */
    int server_pid;           /* host PID of the server for CPU and memory sampling; 0 = off */
} pace_config;

typedef struct pace_result {
    pace_hist get_lat;
    pace_hist put_lat;
    pace_hist req_lat;          /* KVCACHE: latency of whole requests, measured from the intended send time */
    pace_kvcache_stats kvcache; /* KVCACHE: summed counters */
    pace_hist svc_lat;          /* service time of all operations: actual send to completion */
    uint64_t gets;
    uint64_t puts;
    uint64_t misses;
    uint64_t errors;
    double seconds;
    uint64_t max_lag_ns;              /* largest delay of an actual send behind its intended time */
    uint64_t series[PACE_MAX_SERIES]; /* completed operations per second of the measurement phase */
    unsigned nseries;
    int has_server;            /* server sampling succeeded */
    double server_cpu_seconds; /* server CPU time consumed during the measurement phase */
    uint64_t server_rss_bytes; /* server resident set size at the end */
} pace_result;

/* Key length produced by pace_make_key: 'k' followed by 15 decimal digits. */
#define PACE_KEY_LEN 16

/* Writes the key for index idx into out (PACE_KEY_LEN bytes, no terminator). */
void pace_make_key(uint64_t idx, char *out);

/* Runs the benchmark. Returns 0 on success, -1 on setup failure (message on stderr). */
int pace_run(const pace_config *cfg, pace_result *res);

#endif
