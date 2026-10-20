/*
 * runner.h - benchmark runner (closed loop and open loop, one or many connections per thread).
 *
 * Connection models:
 *   - one connection per thread, one request in flight (conns_per_thread = 1, pipeline = 1): the thread
 *     calls the synchronous driver interface (the model of all results before revision 2 of the result
 *     schema);
 *   - otherwise every thread serves conns_per_thread connections from one epoll loop with up to
 *     `pipeline` requests in flight per connection, through the asynchronous driver interface (the model
 *     of memtier_benchmark's -t, -c and --pipeline).
 *
 * Closed loop (rate = 0): a connection issues the next request as soon as a slot of its pipeline is free.
 *
 * Open loop (rate > 0): requests are scheduled at a fixed aggregate rate, split evenly over the threads,
 * with constant or exponentially distributed (Poisson) inter-arrival times. Latency is measured from the
 * intended send time to completion, so time spent waiting behind a slow response, or waiting for a free
 * pipeline slot, is included (no coordinated omission). The pure service time (actual send to completion)
 * is reported separately.
 *
 * With scenario KVCACHE one unit of work is a whole request (a chain of block GETs followed by PUTs of the
 * missing tail, see kvcache.h) instead of a single operation; it uses the synchronous interface. Rates,
 * latencies and the per-second series then count requests; gets, puts and misses count block operations.
 *
 * A run has three phases:
 *   1. preload: every key of the key space is written once (optional).
 *   2. warm-up: requests are issued but not recorded.
 *   3. measurement: latencies and counters are recorded, for duration_s seconds or, with requests > 0,
 *      until every connection has completed that many requests (warm-up is skipped then).
 * With start_at > 0 the threads wait for that Unix time after the preload, so that load generators on
 * several hosts start together (their clocks must be synchronized, for example with NTP or PTP).
 * Throughput is operations recorded during the measurement phase divided by its wall-clock length.
 * Operations in flight at the phase boundaries are counted by the phase in which they complete.
 */
#ifndef PACE_RUNNER_H
#define PACE_RUNNER_H

#include <stdint.h>

#include "driver.h"
#include "hist.h"
#include "kvcache.h"
#include "workload.h"

typedef enum { PACE_SCENARIO_KV = 0, PACE_SCENARIO_KVCACHE = 1 } pace_scenario;
typedef enum { PACE_ARRIVAL_CONSTANT = 0, PACE_ARRIVAL_POISSON = 1 } pace_arrival;

/* Upper bound of per-second throughput samples kept per run. */
#define PACE_MAX_SERIES 3600

typedef struct pace_config {
    const pace_driver *driver;
    const char *host;
    const char *port;
    unsigned threads;
    unsigned conns_per_thread; /* connections served by each thread; 0 or 1 = one */
    unsigned pipeline;         /* requests in flight per connection; 0 or 1 = one */
    double warmup_s;
    double duration_s;
    uint64_t requests; /* > 0: requests per connection instead of a duration */
    double start_at;   /* > 0: Unix time at which the warm-up (or the count) starts */
    pace_workload wl;  /* operations, keys and values of the KV scenario */
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
    pace_hist lat[PACE_OP_COUNT]; /* latency per operation type, from the intended send time */
    uint64_t ops_of[PACE_OP_COUNT];
    pace_hist req_lat;          /* KVCACHE: latency of whole requests, measured from the intended send time */
    pace_kvcache_stats kvcache; /* KVCACHE: summed counters */
    pace_hist svc_lat;          /* service time of all operations: actual send to completion */
    uint64_t gets;              /* GET and the read of RMW */
    uint64_t puts;              /* SET, INSERT and the write of RMW */
    uint64_t misses;            /* GET of an absent key */
    uint64_t mget_keys;         /* keys requested by MGET */
    uint64_t mget_hits;         /* of those, found */
    uint64_t errors;
    double seconds;
    uint64_t max_lag_ns;              /* largest delay of an actual send behind its intended time */
    uint64_t series[PACE_MAX_SERIES]; /* completed operations per second of the measurement phase */
    unsigned nseries;
    int has_server;            /* server sampling succeeded */
    double server_cpu_seconds; /* server CPU time consumed during the measurement phase */
    uint64_t server_rss_bytes; /* server resident set size at the end */
} pace_result;

/* Total operations of a result (all types). */
uint64_t pace_result_ops(const pace_result *r);

/* Fills cfg with defaults: resp driver, 127.0.0.1:6379, 1 thread, 2 s warm-up, 10 s, workload defaults. */
void pace_config_default(pace_config *cfg);

/* Key text of the KV scenario (PACE_KEY_MAX bytes at most); counters (INCR) use their own namespace. */
size_t pace_key_text(const pace_config *cfg, uint64_t idx, int counter, char *out);

/* Fills buf with a value derived from the key index so that readers can verify content. */
void pace_make_value(uint64_t idx, unsigned size, unsigned char *buf);

/* Runs the benchmark. Returns 0 on success, -1 on setup failure (message on stderr). */
int pace_run(pace_config *cfg, pace_result *res);

#endif
