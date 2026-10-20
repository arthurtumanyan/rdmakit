# kvpace

A load generator and measurement tool for key-value servers. It drives the RESP protocol (Redis, Valkey,
Dragonfly), the memcached text protocol and the native protocol of `arenakv` with identical workloads and
identical measurement code, in closed-loop or open-loop mode, and records the environment of every run in
its result files.

- Closed-loop and open-loop load; open-loop latency is measured from the **intended** send time.
- One connection per thread with blocking calls, or many connections per thread from one epoll loop with up to
  `--pipeline` requests in flight per connection (the client model of `memtier_benchmark -t/-c/--pipeline`).
- Operation mixes of GET, SET, DEL, INCR, MGET, read-modify-write and INSERT; YCSB core workloads A, B, C, D and F
  as presets.
- Key distributions: uniform, Zipfian, sequential, Gaussian, latest. Value sizes: fixed, uniform range or weighted
  list. Key prefixes.
- Duration or request-count runs; synchronized start on several hosts and `kvpace merge` to combine their results.
- Authentication (RESP `AUTH`, memcached ASCII authentication, native `AUTH`), RESP `SELECT`, and optional TLS for
  the RESP and memcached drivers (`TLS=1`).
- A KV-cache scenario: prefix-chain requests with reuse accounting.
- Latency histograms with at most 0.8% relative error, configurable percentiles, per-operation latencies;
  per-second throughput series; server CPU and memory sampling.
- One JSON result file per run (schema in [`docs/USAGE.md`](docs/USAGE.md#result-files)), plus CSV and
  HdrHistogram percentile-distribution output.
- CPU pinning of client threads.

**Status:** experimental. Cross-checked against `memtier_benchmark` (see [`docs/validation.md`](docs/validation.md)).

## Why this exists

Published key-value benchmark numbers are hard to compare, for three reasons. Different servers are often measured
with different tools. Closed-loop tools hide queueing delay: a request that waits behind a slow one is never sent
late, so its delay is never counted (coordinated omission). And throughput says nothing about what a cache in
front of an expensive computation actually saves. `kvpace` exists to remove these three sources of error:

- **One tool for every server.** The RESP, memcached text and arenakv native protocols share the same workload
  definitions, key generators, histograms and timing code. A difference between servers is then not a difference
  between client implementations. The results were cross-checked against `memtier_benchmark`
  ([validation](docs/validation.md)).
- **Open-loop latency from the intended send time**, reported next to the pure service time. A server that falls
  behind schedule shows it.
- **A KV-cache scenario** that models prefix chains of shared blocks, such as the key-value cache of an
  inference server. It reports the reuse rate, the fraction of needed blocks served from the cache, which measures saved
  recomputation instead of operations per second ([scenario](docs/kvcache-scenario.md)).
- **Self-describing results.** Every JSON result file records the hardware, kernel, compiler and server CPU and
  memory samples. The benchmark suite in [`../benchmarks`](../benchmarks) uses them to make every published
  number checkable.

`memtier_benchmark`, YCSB, `mutilate` and `wrk2` remain better choices for their own strengths: Redis Cluster
support and the memcached binary protocol, databases other than key-value caches, coordinated distributed load
generation, HTTP. See
[`../docs/PRIOR_ART.md`](../docs/PRIOR_ART.md).

### Comparison

| | kvpace | memtier_benchmark | YCSB |
|---|---|---|---|
| Protocols | RESP, memcached text, arenakv native | RESP, memcached text and binary | many databases through bindings |
| Same measurement code for all servers | yes | yes (two protocols) | per binding |
| Several connections per thread, pipelining | yes (`--conns-per-thread`, `--pipeline`) | yes (`-c`, `--pipeline`) | no (one request per thread) |
| Operation mix | GET, SET, DEL, INCR, MGET, read-modify-write, INSERT | GET/SET ratio, multi-get | read, update, insert, scan, read-modify-write |
| YCSB core workloads | A, B, C, D, F (E needs range scans) | no | A to F |
| Key distributions | uniform, Zipfian, sequential, Gaussian, latest | uniform, Gaussian, sequential | uniform, Zipfian, latest, hotspot, sequential |
| Value sizes | fixed, range, weighted list | fixed, range, weighted list | field count and length |
| TLS, authentication | yes (`TLS=1`) | yes | per binding |
| Open-loop arrivals | constant or Poisson schedule | per-connection rate limit | target throughput |
| Latency from the intended send time | yes, reported next to service time | no | yes (intended latency) |
| HdrHistogram output | percentile distribution files | yes | yes |
| KV-cache prefix-chain scenario with reuse accounting | yes | no | no |
| Server CPU time and memory per operation | yes (`--server-pid`) | no | no |
| Hardware and software environment in every result file | yes | no | no |
| Multi-host load generation | synchronized start, `kvpace merge` of the result files | no | separate client processes |
| Cluster-aware clients | no | Redis Cluster | per binding |

Statements about other systems reflect their public documentation at the time of writing.

### Measured results

Same machine for client and server, loopback, default server configurations. The tables are generated from the raw
data in [`../benchmarks/data`](../benchmarks/data) and checked by `make verify` at the repository root.

Validation against `memtier_benchmark` (Valkey, one server CPU):

<!-- BEGIN GENERATED:validation -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Valkey on one CPU, 64-byte values, 90% GET. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Connections | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|
| 1 | 109,333 (cv 3.5%) | 85,651 (cv 1.7%) | 1.28 | 8.8 (cv 2.2%) | 15.0 (cv 0.0%) | 13 (cv 7.5%) | 20 (cv 22.1%) |
| 16 | 199,259 (cv 1.9%) | 220,849 (cv 1.1%) | 0.90 | 76.5 (cv 1.4%) | 71.0 (cv 0.0%) | 148 (cv 1.9%) | 111 (cv 10.2%) |
<!-- END GENERATED:validation -->

With one connection kvpace measures higher throughput than memtier_benchmark; with 16 connections it measures
between 5% and 15% lower [C21]. The differences come from the client models (run1 used one blocking thread per
connection, memtier an event loop) and are analysed in [`docs/validation.md`](docs/validation.md).

Open-loop sweep over offered load, and the effect of measuring from the intended send time:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/sweep-dark.svg">
  <img alt="Two line charts: achieved operations per second versus offered load, and p99 latency versus offered load, for four servers" src="../benchmarks/figures/sweep.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/coordinated-omission-dark.svg">
  <img alt="Two line charts comparing p99 latency from the intended send time with the pure service time as offered load grows" src="../benchmarks/figures/coordinated-omission.svg">
</picture>

<!-- BEGIN GENERATED:sweep -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Open loop, one server CPU, 64-byte values, 16 connections. Latencies in microseconds, measured from the intended send time unless stated.

| System | Offered ops/s | Achieved ops/s | p50 | p99 | p99 service time | Max schedule lag (ms) |
|---|---|---|---|---|---|---|
| arenakv (TCP) | 20,000 | 20,000 (cv 0.0%) | 39 (cv 29.6%) | 627 (cv 37.8%) | 320 (cv 12.4%) | 5.0 (cv 26.7%) |
| arenakv (TCP) | 60,000 | 60,000 (cv 0.0%) | 141 (cv 10.5%) | 759 (cv 19.5%) | 593 (cv 5.4%) | 6.9 (cv 50.2%) |
| arenakv (TCP) | 100,000 | 100,002 (cv 0.0%) | 295 (cv 6.3%) | 15,199 (cv 133.0%) | 928 (cv 5.6%) | 47.4 (cv 133.8%) |
| arenakv (TCP) | 140,000 | 140,001 (cv 0.0%) | 176 (cv 1.0%) | 40,654 (cv 89.9%) | 519 (cv 0.5%) | 90.7 (cv 46.5%) |
| arenakv (TCP) | 180,000 | 172,831 (cv 0.1%) | 170,568 (cv 15.6%) | 403,352 (cv 31.7%) | 167 (cv 2.8%) | 422.2 (cv 29.9%) |
| arenakv (TCP) | 220,000 | 173,262 (cv 0.5%) | 1,052,770 (cv 0.4%) | 1,756,015 (cv 2.6%) | 168 (cv 1.9%) | 1,814.1 (cv 2.4%) |
| arenakv (TCP) | 260,000 | 173,322 (cv 0.2%) | 1,663,741 (cv 1.0%) | 2,661,985 (cv 0.4%) | 170 (cv 1.3%) | 2,713.4 (cv 1.2%) |
| Valkey | 20,000 | 20,000 (cv 0.0%) | 35 (cv 4.5%) | 479 (cv 38.8%) | 271 (cv 42.7%) | 3.8 (cv 12.3%) |
| Valkey | 60,000 | 60,000 (cv 0.0%) | 45 (cv 37.9%) | 1,353 (cv 47.4%) | 564 (cv 6.7%) | 32.9 (cv 21.8%) |
| Valkey | 100,000 | 99,998 (cv 0.0%) | 324 (cv 10.5%) | 5,822 (cv 17.0%) | 918 (cv 3.1%) | 35.9 (cv 72.5%) |
| Valkey | 140,000 | 140,002 (cv 0.0%) | 185 (cv 8.3%) | 3,173 (cv 14.9%) | 553 (cv 3.9%) | 34.7 (cv 109.9%) |
| Valkey | 180,000 | 180,002 (cv 0.0%) | 93 (cv 2.4%) | 16,409 (cv 165.2%) | 282 (cv 1.5%) | 39.5 (cv 139.3%) |
| Valkey | 220,000 | 204,794 (cv 0.6%) | 353,021 (cv 7.0%) | 580,212 (cv 6.1%) | 143 (cv 0.7%) | 599.6 (cv 6.3%) |
| Valkey | 260,000 | 205,400 (cv 0.6%) | 1,065,353 (cv 4.8%) | 1,691,703 (cv 3.6%) | 143 (cv 0.7%) | 1,730.9 (cv 3.4%) |
| Dragonfly | 20,000 | 20,000 (cv 0.0%) | 31 (cv 2.4%) | 489 (cv 35.4%) | 274 (cv 31.1%) | 5.1 (cv 56.4%) |
| Dragonfly | 60,000 | 60,000 (cv 0.0%) | 58 (cv 34.6%) | 963 (cv 27.8%) | 493 (cv 5.1%) | 13.9 (cv 74.2%) |
| Dragonfly | 100,000 | 100,010 (cv 0.0%) | 490 (cv 10.7%) | 6,423 (cv 2.7%) | 890 (cv 1.2%) | 14.0 (cv 8.7%) |
| Dragonfly | 140,000 | 139,999 (cv 0.0%) | 269 (cv 6.6%) | 5,844 (cv 49.4%) | 539 (cv 3.5%) | 41.9 (cv 38.0%) |
| Dragonfly | 180,000 | 180,009 (cv 0.0%) | 137 (cv 2.4%) | 2,529 (cv 10.4%) | 170 (cv 15.3%) | 10.4 (cv 46.3%) |
| Dragonfly | 220,000 | 203,198 (cv 0.5%) | 396,362 (cv 6.5%) | 673,885 (cv 6.6%) | 108 (cv 10.2%) | 686.5 (cv 6.5%) |
| Dragonfly | 260,000 | 203,266 (cv 1.2%) | 1,086,325 (cv 1.8%) | 1,744,830 (cv 3.2%) | 103 (cv 2.2%) | 1,779.9 (cv 2.4%) |
| memcached | 20,000 | 20,000 (cv 0.0%) | 41 (cv 6.0%) | 365 (cv 19.1%) | 270 (cv 26.5%) | 3.5 (cv 4.0%) |
| memcached | 60,000 | 59,999 (cv 0.0%) | 98 (cv 41.7%) | 677 (cv 7.9%) | 554 (cv 9.2%) | 5.4 (cv 21.8%) |
| memcached | 100,000 | 99,999 (cv 0.0%) | 371 (cv 30.7%) | 6,575 (cv 23.2%) | 1,013 (cv 2.4%) | 44.4 (cv 14.1%) |
| memcached | 140,000 | 140,064 (cv 0.1%) | 248 (cv 5.2%) | 3,539 (cv 3.5%) | 687 (cv 1.2%) | 33.5 (cv 57.6%) |
| memcached | 180,000 | 179,846 (cv 0.2%) | 162 (cv 1.1%) | 2,387 (cv 6.7%) | 442 (cv 1.7%) | 34.8 (cv 113.9%) |
| memcached | 220,000 | 219,986 (cv 0.0%) | 104 (cv 4.5%) | 15,707 (cv 96.4%) | 181 (cv 2.8%) | 33.0 (cv 32.5%) |
| memcached | 260,000 | 226,520 (cv 0.5%) | 641,729 (cv 3.4%) | 1,291,846 (cv 5.2%) | 143 (cv 1.1%) | 1,446.5 (cv 11.4%) |
<!-- END GENERATED:sweep -->

- Up to 140,000 offered operations per second all four servers achieve the offered rate within 1% [C8].
- At 260,000 offered operations per second, p99 latency measured from the intended send time is more than 1,000
  times the p99 service time for every server [C9]. A closed-loop or service-time-only tool would not show this
  backlog.

## Requirements

Linux, a C compiler with `-std=gnu2x` (gcc 13 or newer, clang 18 or newer), `make`, pthreads, libm. The native
driver for `arenakv` additionally needs `../arenakv` and `../rdmaduct` (built automatically; override the locations
with `AKV_DIR` and `DUCT_DIR`, or build without it with `NATIVE=0`).

## Build and test

```
make                 # build/<arch>/kvpace
make test            # unit tests: histogram, generators, drivers, runner timing, merge, KV-cache workload
make SAN=1 test      # AddressSanitizer + UndefinedBehaviorSanitizer
make TLS=1 test      # with TLS for the text drivers (OpenSSL), build/<arch>-tls/
make NATIVE=0        # without the arenakv driver
make RDMA=0          # native driver over TCP only (build/<arch>-tcp/)
```

Test results: all ten suites pass on x86-64, in the default, `TLS=1`, `RDMA=0` and `NATIVE=0` builds and under
AddressSanitizer with UndefinedBehaviorSanitizer (with and without TLS). CI runs them with gcc and clang on x86-64
and arm64, and the TLS build on x86-64.

| Suite | Covers |
|---|---|
| `test_hist` | exact storage of small values, bucket bounds, relative quantization error over the 64-bit range, quantiles, merge |
| `test_zipf` | empirical rank frequencies against the exact Zipf probabilities, uniformity at theta 0, range |
| `test_workload` | parsing of mixes, sizes and presets; operation frequencies; sequential, Gaussian, latest and Zipf keys; value-size bounds and frequencies; key text |
| `test_resp`, `test_memcached` | drivers against in-process servers: round trips, missing keys, binary-safe and empty values, oversize values |
| `test_drivers` | all three drivers against an in-process arenakv server: DEL, INCR, MGET, authentication, and pipelined asynchronous requests compared with the synchronous results |
| `test_runner` | closed- and open-loop timing with a synthetic driver whose throughput and latency are known analytically, for one and for several connections per thread with pipelining; request-count mode |
| `test_merge` | JSON round trip, `kvpace merge` (summed counters and series, merged histograms, idempotent percentiles, schema check), CSV and `.hgrm` output |
| `test_tls` | TLS round trips and 200 pipelined requests with certificate verification, refusal of an untrusted certificate, `--tls-skip-verify` (`TLS=1` builds) |
| `test_kvcache` | request structure, hit rates, determinism, key sharing and separation, corruption detection |

## Quick start

Start a server (any of the supported protocols), then:

```
# closed loop: 16 connections, 64-byte values, 90% GET, 10 s after 2 s warm-up
build/x86_64/kvpace --port 6379 --threads 16 --duration 10 --value-size 64 --read-ratio 0.9 \
    --pin 8,10,12,14 --label "valkey 9" --out result.json

# open loop: 100,000 operations per second in total, constant arrivals
build/x86_64/kvpace --port 6379 --threads 16 --rate 100000 --duration 10 --out result.json

# memtier-style client: 4 threads x 4 connections, 8 requests in flight per connection, SET:GET 1:10
build/x86_64/kvpace --port 6379 --threads 4 --conns-per-thread 4 --pipeline 8 --ratio 1:10 --out result.json

# YCSB workload A (50% read, 50% update, Zipfian, 1000-byte values) over TLS with a password
build/x86_64-tls/kvpace --port 6380 --tls --cacert ca.pem --auth secret --workload ycsb-a --threads 8

# two load-generator hosts started together, results combined afterwards
build/x86_64/kvpace --host 192.0.2.10 --threads 8 --start-at 1793000000 --out a.json    # on each host
build/x86_64/kvpace merge --out all.json a.json b.json

# KV-cache scenario against a memcached server
build/x86_64/kvpace --driver memcached --port 11211 --scenario kvcache --threads 8 --block-size 16384

# native protocol of arenakv over RDMA
build/x86_64/kvpace --driver native --driver-opt transport=rdma --host 192.0.2.10 --port 7600
```

The text output prints throughput, latency percentiles per operation type and, with `--server-pid`, the server's
CPU time per operation and resident memory. `kvpace --help` lists every option.

## Options

The main options; `kvpace --help` prints the complete list.

| Option | Default | Meaning |
|---|---|---|
| `--driver NAME` | `resp` | `resp`, `memcached`, `native` |
| `--driver-opt K=V` | | driver option, repeatable; `native`: `transport=tcp\|rdma\|auto`, `wait=poll\|event`, `max-msg=N`, `depth=N` |
| `--host`, `--port` | `127.0.0.1`, `6379` | server address |
| `--threads N` | 1 | client threads |
| `--conns-per-thread N`, `--pipeline N` | 1, 1 | connections per thread; requests in flight per connection |
| `--auth SECRET`, `--db N` | | password or `user:password`; RESP database |
| `--tls`, `--cacert`, `--cert`, `--key`, `--sni`, `--tls-skip-verify` | off | TLS for `resp` and `memcached` (`TLS=1` builds) |
| `--duration SEC`, `--warmup SEC` | 10, 2 | measurement and warm-up length |
| `--requests N` | | requests per connection instead of a duration (no warm-up) |
| `--start-at UNIXTIME` | | start the warm-up at this time after the preload |
| `--keys N`, `--key-prefix TEXT` | 100000, `k` | key space size (preloaded unless `--no-preload`); key prefix |
| `--dist NAME`, `--theta X`, `--gauss-sd X` | `uniform`, 0.99, 1/6 | `uniform`, `zipf`, `sequential`, `gaussian`, `latest` |
| `--value-size SPEC` | 64 | `N`, `A-B` or `S:W,S:W,...` |
| `--read-ratio X`, `--ratio SET:GET`, `--mix SPEC` | 0.9 | operation mix, for example `--mix get=80,set=15,del=3,incr=2` |
| `--multi-get N` | 1 | keys per MGET |
| `--workload NAME` | | `ycsb-a`, `ycsb-b`, `ycsb-c`, `ycsb-d`, `ycsb-f` |
| `--rate N`, `--arrival NAME`, `--spin` | 0, `constant` | open loop: aggregate operations per second, `constant` or `poisson`, busy-wait |
| `--scenario NAME` | `kv` | `kv` or `kvcache` (parameters: `--block-size`, `--groups`, `--shared-blocks`, `--turn-blocks`, `--active-conv`, `--max-turns`, `--new-conv-prob`) |
| `--server-pid PID` | | sample the server's CPU time and resident memory |
| `--pin LIST` | | CPU list for client threads (thread i uses entry i mod length) |
| `--label TEXT`, `--out FILE` | | name stored in the result; JSON output path |
| `--csv FILE`, `--hdr-prefix PREFIX`, `--percentiles LIST` | 50,90,99,99.9,99.99 | CSV summary; HdrHistogram files; reported percentiles |
| `--seed N`, `--no-preload` | 1 | random seed; skip the preload |

The exit status is 0 on success, 1 on setup failure, 2 on invalid options and 3 when some operations failed (the
result is still written).

## Measurement model in one paragraph

With one connection per thread and no pipelining, each thread has one request in flight and calls the driver
synchronously. Otherwise each thread serves its connections from one epoll loop and keeps up to `--pipeline`
requests in flight on each. In the open loop requests follow a schedule; a request that cannot be sent at its
intended time, because the connection is busy or the generator is behind, is sent as soon as possible and the
delay counts as latency, because latency is measured from the intended send time (the pure service time is
reported separately). Warm-up operations are not recorded. Throughput is completed operations divided by the
measurement time. Definitions of all metrics: [`../benchmarks/METHODOLOGY.md`](../benchmarks/METHODOLOGY.md).

## Helper scripts

| Script | Purpose |
|---|---|
| `scripts/servers.sh` | starts and stops the systems under test (containers or native `arenakvd`), with a capacity setting |
| `scripts/env.sh` | CPU assignment used by the benchmark suite |

The benchmark suite in [`../benchmarks`](../benchmarks) orchestrates these for the published measurements.

## Limitations

- No cluster-aware clients (Redis Cluster, consistent hashing over several servers) and no memcached binary
  protocol.
- The KV-cache scenario uses the synchronous model (one connection per thread).
- The native driver has no TLS; its RDMA transport is not encrypted.
- Keys are the prefix followed by 15 digits; the prefix is at most 32 bytes.
- `kvpace merge` adds throughput and merges histograms; it does not check that the inputs ran at the same time.
- Server CPU sampling has the 10 ms granularity of `/proc` and needs the server's host PID.

## Layout

| Path | Content |
|---|---|
| `src/` | tool and library code (`runner`, `workload`, `hist`, `zipf`, `kvcache`, drivers, `sock`, `report`, `json`, `merge`, `main`) |
| `tests/` | unit tests |
| `scripts/` | helper scripts |
| `docs/` | usage, architecture, scenario and validation documents |

## Further reading

[Usage and result schema](docs/USAGE.md) · [Architecture](docs/ARCHITECTURE.md) ·
[KV-cache scenario](docs/kvcache-scenario.md) · [Validation against memtier](docs/validation.md) ·
[Methodology](../benchmarks/METHODOLOGY.md)
