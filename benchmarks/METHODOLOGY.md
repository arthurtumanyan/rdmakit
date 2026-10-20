# Methodology

This document defines what is measured, how, and what the results do and do not show. It is written so that
a reader can disagree with a result for a specific reason and check that reason against the raw data.

## Principles

1. **Raw data is the source.** Every number in a figure or table is computed from the JSON files in
   `benchmarks/data/<run>/` by `tools/bench_data.py`. Nothing is typed by hand.
2. **Everything needed to repeat a run is recorded.** `env.json` holds the CPU model, kernel, governor,
   compiler, container image digests, software versions and the git commit; `manifest.json` holds the command
   line, parameters, status and wall time of every run.
   Run `2026-10-20-run1` was recorded during development: its `git_commit` refers to the development history, which
   is not part of this repository, and its files use the development names `kvstore` (arenakv), `kvbench`
   (kvpace) and `rdmatp` (rdmaduct). Absolute paths below the source checkout in its command lines were rewritten
   relative to the repository root after the run, and the address of the RDMA interface (`--host` of the Soft-RoCE
   rows) was replaced by the documentation address 192.0.2.10 (RFC 5737); no measured value was changed.
3. **Repetitions are interleaved.** Repetition 1 of every configuration runs before repetition 2 of any
   configuration, in a shuffled order seeded by the repetition number. Slow drift of the machine (thermal state,
   background load) is thereby spread over configurations instead of favouring one.
4. **Each run starts a fresh server** on an empty cache, so earlier runs cannot influence later ones.
5. **Failures are visible.** Failed runs stay in the manifest and are excluded from the statistics; runs that
   finish with operation errors are kept and their error count is reported. Configurations a system cannot run
   are listed as skipped with the reason.
6. **Claims are machine-checked.** Statements in the documentation that compare numbers are tagged `[C<n>]`;
   `tools/claims.py` evaluates each one against the data and `tools/verify.py` fails when one does not hold.

## Hardware and software setup

The system under test and the load generator run on the same machine and exchange data over the loopback
interface (127.0.0.1). The environment of each run is in `env.json`; the run that produced the published
figures is described in [`README.md`](README.md). Consequences of this choice:

- The network is not a bottleneck and not a variable, so differences reflect software cost on the server and the
  client. Results do not predict behaviour across a physical network.
- The load generator competes with the server for the memory hierarchy (shared L3 cache, memory bandwidth).
  CPU sets are disjoint to avoid scheduler interference.
- The RDMA rows use **Soft-RoCE** (`rdma_rxe`), a kernel implementation of the RDMA protocol over Ethernet.
  It exercises the RDMA programming model and the library code; it does not behave like RDMA hardware, where
  the NIC performs transfers without CPU involvement. No conclusion about RDMA hardware performance may be drawn
  from these rows.

CPU assignment (see `kvpace/scripts/env.sh`): the server runs under `taskset` or `--cpuset-cpus` on logical CPU
0 (one-CPU configurations) or CPUs 0,2,4,6 (four-CPU configurations); these are distinct physical cores of the
performance-core cluster. The load generator uses CPUs 8,10,12,14. Hyper-thread siblings of used cores are left idle.

## Systems under test

| System | How it runs | Configuration |
|---|---|---|
| Valkey | container, host network | `--io-threads` = number of CPUs, persistence off, `--maxmemory` with `allkeys-lru` |
| Dragonfly | container, host network | `--proactor_threads` = number of CPUs, `--cache_mode=true`, `--maxmemory`; refuses to start below 256 MiB per thread |
| memcached | container, host network | `-t` = number of CPUs, `-m` = capacity in megabytes |
| arenakv | native process under `taskset` | one connection thread per client, event-driven waiting, `--capacity` |
| arenakv (Soft-RoCE) | same process, RDMA transport on the `rxe0` device | as above |
| Dragonfly, eviction flag raised | as Dragonfly | additionally `--max_eviction_per_heartbeat=10000`; used only in the capacity suite, see below |

Server arguments are set in `tools/run.py` (for arenakv: `--avg-entry` 128 for `small`, 16384 for `block` and `kvcache`;
capacity 8 GiB unless the capacity suite sets one). The per-run manifest of the published run records the `kvpace`
command line but not the server arguments; later runs record them under `server`. Container images are used as pulled; their digests are in `env.json`. Default settings were not tuned for any
system. Tuning (for example kernel parameters, huge pages, per-system thread counts beyond the CPU count) might
change the ranking and was not attempted.

### A configuration change made after observing failures

In the capacity suite Dragonfly with its default eviction settings rejected most writes with `ERR Out of memory`
at 256 MB and 1 GB. A probe outside the benchmark (a sequential fill with 16 KiB values at 256 MB) showed that raising
`--max_eviction_per_heartbeat` removes the failures. The suite therefore contains both variants: the original
default-flag runs, with their failure rates reported, and a variant with that one flag raised (`capacity-evict` in
`tools/run.py`). The choice of flag and value was made after seeing the failures and the value (10000) was not
tuned further. Other systems received no comparable tuning, so the variant may favor Dragonfly relative to the
others; the default-flag results are kept so that readers can judge. Dragonfly also refuses to start with less than
256 MiB per thread, so it has no results at 16 and 64 MB.

## Workloads

All workloads use keys of 16 bytes (`k` followed by 15 digits) except the KV-cache scenario (17 bytes).

| Name | Definition |
|---|---|
| `small` | 100,000 keys preloaded, 64-byte values, uniform key choice, 90% GET / 10% SET |
| `block` | 20,000 keys preloaded, 16 KiB values, uniform key choice, 90% GET / 10% SET |
| `kvcache` | prefix-chain requests, see [`kvpace/docs/kvcache-scenario.md`](../kvpace/docs/kvcache-scenario.md): 16 KiB blocks, 16 prefix groups of 32 shared blocks, 8 new blocks per turn, 256 conversation slots per connection, 16 turns per conversation, 5% new conversations, Zipf skew 0.9; the cache starts empty |

Value contents are a deterministic function of the key; reads of the KV-cache scenario are verified byte for byte
(`corrupt` must be zero).

## Load generation

- **Closed loop** (`matrix`, `capacity`): 16 connections (8 for `capacity`), one thread each, one request in
  flight per connection, 3 s warm-up (not recorded) and 8 s (matrix) or 10 s (capacity) measurement.
- **Open loop** (`sweep`): requests are issued at a fixed aggregate rate with constant inter-arrival times,
  split over 16 connections, 2 s warm-up and 6 s measurement. Latency is measured from the **intended** send time
  to completion. A connection that is behind schedule sends immediately, so waiting behind a slow response counts
  as latency. This is the correction for coordinated omission; the figure `coordinated-omission` shows the
  difference between this latency and the pure service time on the same runs.
- Throughput is the number of operations that completed in the measurement phase divided by its wall-clock
  length. For `kvcache`, one unit is a request (a chain of block GETs plus PUTs of the missing blocks); rates
  and latencies refer to requests, while "per operation" server CPU refers to block GETs and PUTs.

## Metrics

| Metric | Definition |
|---|---|
| throughput | completed operations (requests for `kvcache`) per second of the measurement phase |
| p50, p99, p99.9 latency | percentiles of GET latency (request latency for `kvcache`), from a log-linear histogram with at most 0.8% relative error; closed loop: from request start; open loop: from the intended send time |
| service time | latency from actual send to completion, all operations |
| max schedule lag | largest delay of an actual send behind its intended time (open loop) |
| server CPU per operation | CPU time (user + system) of the server process during the measurement phase divided by completed operations; read from `/proc/<pid>/stat` with 10 ms granularity; for containers the PID of the server binary is used |
| server RSS | resident set size of the server process at the end of the run |
| reuse rate | `block_hits / (block_hits + block_puts)`: fraction of needed blocks served from the cache instead of being recomputed |

Server CPU time includes kernel time for network I/O. It does not include CPU time of the load generator.

## Statistics

Each configuration is repeated (5 times for `matrix`, `engine` and `transport`; 3 times for `sweep` and
`capacity`; 5 times for `validation`). Tables show the mean and the coefficient of variation (standard deviation divided by the mean,
sample standard deviation). Figures show the mean and a whisker from the minimum to the maximum repetition.
With 3 to 5 repetitions these are descriptive: no confidence intervals or significance tests are claimed. A
difference between two systems is stated in the documentation only when the ranges of the repetitions do not
overlap, or when the statement is a direct restatement of a table cell.

## Engine and transport benchmarks

- `akv_bench scale` (in `arenakv/tools`): threads call the engine directly in a closed loop over uniformly
  distributed keys; the key space is preloaded and fits in memory, so every GET hits and nothing is evicted.
  Threads are pinned to distinct physical cores first, then to their hyper-thread siblings (the shaded region of
  the figure).
- `akv_bench micro`: the 16-lane tag comparison is timed on 4096 tag arrays; the scalar reference is compiled at
  `-O2` without auto-vectorization, so the ratio describes this compiler setting. The hash is timed on
  consecutive calls whose inputs depend on the previous result: the figure is a latency-bound throughput.
- `duct_bench` (in `rdmaduct/tools`): `ping-pong` times single round trips of one message; `stream` sends
  messages in batches of 16 and waits for one acknowledgement. Both endpoints run on the same host (loopback).
  For large messages the volume per size is bounded (64 MiB ping-pong, 256 MiB streaming), so the number of
  iterations is recorded per size in the raw files.

## Threats to validity

- **Single machine, single run directory.** Results come from one machine (see `env.json`). Another CPU,
  kernel or NIC may rank the systems differently.
- **Power management.** The CPU governor was `powersave`, which adds wake-up latency and frequency variation.
  Tail latencies (p99 and above) at low load are affected most; the `performance` governor would reduce this.
  Throughput of saturated runs is less sensitive.
- **Client limits.** In run1 the load generator used one thread per connection with blocking I/O. In the four-CPU
  configurations the generator may be the bottleneck; those rows show the order of systems under this client, not
  the capacity of each server.
- **Default configurations.** Each system runs with defaults plus the settings above. A system's maintainers might
  recommend different settings.
- **Container overhead.** Valkey, Dragonfly and memcached run in containers on the host network; arenakv runs
  natively. The host-network mode avoids the bridge, but cgroup accounting and image differences (libc, allocator)
  remain. The reference validation against `memtier_benchmark` is in
  [`kvpace/docs/validation.md`](../kvpace/docs/validation.md).
- **Different memory accounting.** The configured capacity means different things to different systems: for
  Valkey and Dragonfly it limits total memory including per-key overhead; for memcached it limits slab memory;
  for arenakv it limits the arena of entries (the hash index is additional). Comparisons at equal configured
  capacity are therefore not comparisons at equal total memory. Server RSS is reported next to every capacity result.
- **Workload realism.** The `kvcache` scenario is a model with stated parameters, not a trace of a production
  system.

## Reproducing the results

```
make                                      # build all projects (repository root)
export RDMA_ADDR=<IPv4 address of an interface with an RDMA device>   # for the Soft-RoCE rows
export BENCH_NIC=<that interface's name>                              # recorded in env.json
benchmarks/tools/run.py --suite all --reps 5 --out benchmarks/data/<new-run-id>
benchmarks/tools/report.py benchmarks/data/<new-run-id> --update-docs
benchmarks/tools/verify.py benchmarks/data/<new-run-id>
```

`run.py` needs Docker for the third-party systems and at least 24 logical CPUs for the CPU assignment above
(edit `CLIENT_CPUS` and `SERVER_CPUS` in `tools/run.py` and `kvpace/scripts/env.sh` for other machines). A full run
takes about two hours. Third-party images are pulled as `latest`; to reproduce a published run exactly, pull the
digests listed in its `env.json`.

## Challenging a result

1. Find the raw files behind the number: `generated/summary.json` lists the input files; `manifest.json` lists the
   command line of each run.
2. Re-run a single configuration with the command from the manifest and compare.
3. Check the claim: `tools/claims.py` contains the exact condition behind each statement; `verify.py` evaluates it.
4. If a disagreement points to a flaw in the method (for example pinning, warm-up length, client limits), open an
   issue with the changed setting and raw data; results are replaced only by a new run directory, never edited.

## Raw file formats

- `kvpace` result: schema version 1, written by `kvpace/src/report.c` (configuration, environment, counters,
  latency summaries, per-second series, server CPU and memory).
- `akv_bench` and `duct_bench` results: one JSON document per run (`tool`, `mode`, parameters, `results`).
- `manifest.json`: list of `{suite, file, status, params, command, wall_s}`; `status` is `ok`, `errors`,
  `failed` or `skipped`.
