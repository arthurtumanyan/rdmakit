# kvpace architecture

This document is for people who change or review the tool. Usage is in the [README](../README.md) and
[`USAGE.md`](USAGE.md).

## Source map

| File | Role |
|---|---|
| `src/main.c` | command line parsing, wiring |
| `src/runner.c`, `runner.h` | threads, phases (preload, warm-up, measurement), closed and open loop, result aggregation |
| `src/hist.c`, `hist.h` | log-linear latency histogram |
| `src/rng.h`, `src/zipf.c`, `zipf.h` | xoshiro256** generator, Zipfian rank generator |
| `src/kvcache.c`, `kvcache.h` | KV-cache request model |
| `src/driver.h`, `driver.c` | driver interface and registry |
| `src/drv_resp.c`, `drv_memcached.c`, `drv_native.c`, `netbuf.c` | protocol drivers and the shared buffered socket |
| `src/proc.c`, `proc.h` | server CPU and memory sampling from `/proc` |
| `src/report.c`, `report.h` | JSON and text output |

## Execution model

`pace_run()` creates one worker thread per connection. Each worker:

1. connects, then (optionally) writes its share of the key space (thread `i` writes keys `i`, `i + T`, ...);
2. reports ready and waits for the start flag;
3. loops: choose the next operation, wait for its intended time (open loop), execute it through the driver,
   record the outcome if the measurement phase is active.

The main thread waits until all workers are ready, sets the start flag, sleeps for the warm-up, takes the first
server sample and a timestamp, sets the `measuring` flag, sleeps for the measurement duration, clears the flag,
takes the second server sample and stops the workers. Phase changes are atomic flags; an operation belongs to the
phase in which it completes. No lock is taken on the measurement path: each worker owns its histograms, counters
and per-second series, and the main thread merges them after joining.

## Time

All times use `CLOCK_MONOTONIC` in nanoseconds. Closed loop: the latency of an operation is the time from before the
driver call to after it returns. Open loop: the intended time of operation `k` of a thread is
`start + offset + sum of inter-arrival steps`; the recorded latency is `completion - intended`, the service time is
`completion - actual start`, and `max_lag_ns` tracks `actual start - intended`. Waiting uses `nanosleep` until 100 us
before the target and spins for the rest; `--spin` spins for the whole wait.

## Histogram

Values are unsigned 64-bit integers (nanoseconds). Index function: values below 128 map to themselves; a larger
value with most significant bit `m` falls into one of 128 sub-buckets of the range `[2^m, 2^(m+1))`, so the
quantization error is at most 2^-7 relative. The table has 7424 counters (58 KiB). Quantile queries return the
upper bound of the bucket holding the sample of rank `ceil(q * n)`, clamped to the observed minimum and maximum.
`tests/test_hist.c` checks exactness below 128, bucket bounds, adjacency, the relative error over the whole
64-bit range, quantiles and merging.

## Key distributions

Uniform keys use a multiply-shift reduction of a 64-bit random value. Zipfian ranks use the generator of Gray et
al. (SIGMOD 1994, the same as YCSB): the first two ranks are exact, the tail is approximate; the rank is then
scrambled over the key space with a fixed hash so that popular keys do not cluster. Initialization is O(n).
`tests/test_zipf.c` compares the empirical rank frequencies with the exact probabilities.

## Drivers

```c
typedef struct pace_driver {
    const char *name;
    pace_conn *(*connect)(const char *host, const char *port);
    int (*put)(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen);
    int (*get)(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen);
    void (*close)(pace_conn *c);
    int (*set_option)(const char *key, const char *value);   /* optional */
} pace_driver;
```

A connection is used by one thread and has at most one request in flight. `put` returns `PACE_OK` or `PACE_ERR`;
`get` returns `PACE_OK`, `PACE_NOTFOUND` or `PACE_ERR` (a value larger than the buffer is an error). To add a driver:
implement the table in `src/drv_<name>.c`, declare it in `driver.h`, register it in `driver.c`, add the source
to `LIB_SRC` in the Makefile, and write a test against an in-process fake server (see `tests/test_resp.c` and
`tests/test_memcached.c` for the pattern: binary-safe values, missing keys, large values, buffer-too-small).

The native driver is compiled only when `../arenakv` and `../rdmaduct` are available (`PACE_HAVE_NATIVE`).

## KV-cache scenario

`kvcache.c` implements the request model described in [`kvcache-scenario.md`](kvcache-scenario.md). Each worker
owns a state (conversation slots, random generator) so threads share nothing. A request issues block GETs in chain
order until the first miss, then PUTs of the remaining blocks. Block contents are derived from the block key and
verified on every hit. `tests/test_kvcache.c` uses an in-memory LRU cache as a driver to check hit and reuse rates
with ample and tiny capacity, determinism for equal seeds, key sharing and corruption detection.

## Result aggregation

Each worker keeps GET, PUT, service-time and request histograms, operation counters, the largest schedule lag and a
per-second completion series (up to 3600 seconds). `pace_run()` merges them. The server sample is taken before and
after the measurement phase; the CPU time is the difference.

## Tests

| Test | Purpose |
|---|---|
| `test_hist` | histogram accuracy and merging |
| `test_zipf` | generator distribution |
| `test_resp`, `test_memcached` | drivers against in-process servers |
| `test_kvcache` | KV-cache model |
| `test_runner` | runner timing with a synthetic driver: closed-loop bound, open-loop rate below saturation, backlog-inclusive latency above saturation, series sum |

`make SAN=1 test` runs them under AddressSanitizer and UndefinedBehaviorSanitizer.

## Known limitations

- Thread per connection; no multiplexing, so burst arrival patterns of event-loop clients are not reproduced.
- The server CPU sample has 10 ms resolution.
- The per-second series is capped at 3600 entries.
