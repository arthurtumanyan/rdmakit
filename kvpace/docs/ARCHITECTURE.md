# kvpace architecture

This document is for people who change or review the tool. Usage is in the [README](../README.md) and
[`USAGE.md`](USAGE.md).

## Source map

| File | Role |
|---|---|
| `src/main.c` | command line parsing (`--help` text), wiring, `merge` subcommand dispatch |
| `src/runner.c`, `runner.h` | threads, phases (preload, warm-up, measurement), closed and open loop, synchronous and multiplexed connection models, result aggregation |
| `src/workload.c`, `workload.h` | operation mix, key distributions, value sizes, YCSB presets |
| `src/hist.c`, `hist.h` | log-linear latency histogram |
| `src/rng.h`, `src/zipf.c`, `zipf.h` | xoshiro256** generator, Zipfian rank generator |
| `src/kvcache.c`, `kvcache.h` | KV-cache request model |
| `src/driver.h`, `driver.c` | driver interface and registry |
| `src/drv_resp.c`, `drv_memcached.c`, `drv_native.c` | protocol drivers, each with a synchronous and an asynchronous interface |
| `src/sock.c`, `sock.h` | TCP connection with optional TLS (OpenSSL) |
| `src/netbuf.c`, `abuf.c` | buffered blocking connection (synchronous drivers); non-blocking input and output buffers (asynchronous drivers) |
| `src/proc.c`, `proc.h` | server CPU and memory sampling from `/proc` |
| `src/report.c`, `report.h` | JSON, text, CSV and HdrHistogram output |
| `src/json.c`, `json.h`, `src/merge.c`, `merge.h` | JSON reader; `kvpace merge` |

## Execution model

`pace_run()` creates `--threads` worker threads. Each worker:

1. connects its connections, then (optionally) writes its share of the key space (thread `i` writes keys `i`,
   `i + T`, ...);
2. reports ready and waits for the start flag (and for `--start-at`, when given);
3. loops: choose the next operation from the workload, wait for its intended time (open loop), execute it, record
   the outcome if the measurement phase is active.

Step 3 has two implementations:

- **Synchronous** (`--conns-per-thread 1 --pipeline 1`, and always for the KV-cache scenario): one connection, one
  request in flight; the worker calls the blocking driver functions.
- **Multiplexed** (any other setting): the worker owns `C` connections registered in one epoll set. Each connection
  has a ring of up to `--pipeline` in-flight records (operation type, intended time, send time). In the closed loop
  every free slot is filled at once. In the open loop the schedule produces intended times into a per-thread
  backlog, and the oldest backlog entry is submitted to the next connection with a free slot (round robin), keeping
  its intended time, so time spent waiting for a slot counts as latency. After writing and reading, replies are
  taken in request order and matched with the head of the ring. The worker blocks in `epoll_pwait2` only when no
  connection can make progress; the timeout is the next intended time in the open loop (50 ms otherwise, to notice
  the stop flag).

The main thread waits until all workers are ready, sets the start flag, sleeps for the warm-up, takes the first
server sample and a timestamp, sets the `measuring` flag, sleeps for the measurement duration (or, with
`--requests`, waits until every worker has finished its count), clears the flag, takes the second server sample and
stops the workers. Phase changes are atomic flags; an operation belongs to the phase in which it completes. No lock
is taken on the measurement path: each worker owns its histograms, counters and per-second series, and the main
thread merges them after joining.

## Time

All times use `CLOCK_MONOTONIC` in nanoseconds. Closed loop: the latency of an operation is the time from before the
driver call to after it returns. Open loop: the intended time of operation `k` of a thread is
`start + offset + sum of inter-arrival steps`; the recorded latency is `completion - intended`, the service time is
`completion - actual start`, and `max_lag_ns` tracks `actual start - intended`. In the multiplexed runner the actual
start is the time the request is encoded into the connection's output; it is written in the same loop iteration.
The synchronous runner waits with `nanosleep` until 100 us before the target and spins for the rest; `--spin` spins
for the whole wait. The multiplexed runner waits in `epoll_pwait2` with a nanosecond timeout and does not spin.

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

The interface is in `src/driver.h`. Every driver has a synchronous table (`connect`, `put`, `get`, `close`, and
optionally `del`, `incr`, `mget`, `set_option`) and an asynchronous table `async`:

| Function | Contract |
|---|---|
| `connect` | non-blocking connection, authenticated when credentials are set |
| `fd`, `events`, `arm` | descriptor and epoll events to wait for; `arm` returns 1 when progress is possible without waiting (for example TLS records already decrypted, or replies already queued by the native client) |
| `window` | largest number of outstanding requests the connection accepts (native: the client's window) |
| `submit` | encodes a request (`pace_req`: type, key, value, MGET keys); data is copied |
| `io` | writes pending output and reads available input without blocking |
| `next` | the reply of the oldest outstanding request: status and, for MGET, the number of keys found |

Return codes are `PACE_OK`, `PACE_NOTFOUND` and `PACE_ERR`; a value larger than the buffer is an error. The text
drivers parse replies incrementally from a byte buffer, so a reply split over any number of reads is handled; the
native driver uses the pipelined calls of the arenakv client (`akvc_post`, `akvc_next`). Operation encodings:

| Operation | RESP | memcached | native |
|---|---|---|---|
| GET, SET, DEL | `GET`, `SET`, `DEL` | `get`, `set`, `delete` | `GET`, `PUT`, `DEL` |
| INCR | `INCR` | meta `ma <key> N0 J1` (creates the counter) | `INCRS` |
| MGET | `MGET` | `get k1 k2 ...` | batched `MGET` |

RMW is a GET followed by a SET; INSERT is a SET of a new key. To add a driver: implement both tables in
`src/drv_<name>.c`, declare it in `driver.h`, register it in `driver.c`, add the source to `LIB_SRC` in the
Makefile, and write tests against an in-process server (`tests/test_resp.c` and `tests/test_memcached.c` for the
synchronous pattern, `tests/test_drivers.c` for pipelining).

TLS lives below the drivers in `sock.c`: `pace_tls_setup()` builds one client context; `pace_sock_connect()`
performs the handshake (blocking) and verifies the certificate against the host name or IP address. Reads and
writes map `SSL_ERROR_WANT_READ/WRITE` to "no progress", and `arm` reports records that OpenSSL has already
decrypted (`SSL_pending`), which epoll cannot see.

The native driver is compiled only when `../arenakv` and `../rdmaduct` are available (`PACE_HAVE_NATIVE`).

## KV-cache scenario

`kvcache.c` implements the request model described in [`kvcache-scenario.md`](kvcache-scenario.md). Each worker
owns a state (conversation slots, random generator) so threads share nothing. A request issues block GETs in chain
order until the first miss, then PUTs of the remaining blocks. Block contents are derived from the block key and
verified on every hit. `tests/test_kvcache.c` uses an in-memory LRU cache as a driver to check hit and reuse rates
with ample and tiny capacity, determinism for equal seeds, key sharing and corruption detection.

## Result aggregation

Each worker keeps one latency histogram per operation type, service-time and request histograms, operation
counters, the largest schedule lag and a per-second completion series (up to 3600 seconds). `pace_run()` merges
them. The JSON result stores the non-empty buckets of every histogram, so `kvpace merge` can combine results of
several processes exactly (bucket counts add) and recompute any percentile. The server sample is taken before and
after the measurement phase; the CPU time is the difference.

## Tests

| Test | Purpose |
|---|---|
| `test_hist` | histogram accuracy and merging |
| `test_zipf` | generator distribution |
| `test_workload` | mix parsing and frequencies, key distributions, value sizes, presets, key text |
| `test_resp`, `test_memcached` | synchronous drivers against in-process servers |
| `test_drivers` | all drivers against an in-process arenakv server: extra operations, authentication, pipelined asynchronous requests |
| `test_kvcache` | KV-cache model |
| `test_runner` | runner timing with a synthetic driver, synchronous and multiplexed: closed-loop bound, open-loop rate below saturation, backlog-inclusive latency above saturation, series sum, request-count mode |
| `test_merge` | JSON round trip, merging, CSV and `.hgrm` output |
| `test_tls` | TLS with certificate verification, refusal and skip-verify (`TLS=1`) |

`make SAN=1 test` runs them under AddressSanitizer and UndefinedBehaviorSanitizer.

## Known limitations

- The server CPU sample has 10 ms resolution.
- The per-second series is capped at 3600 entries.
- The multiplexed runner does not spin; open-loop send times follow the schedule with the wake-up latency of
  `epoll_pwait2` (microseconds), which `max_lag_ns` reports.
