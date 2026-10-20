# Validation against memtier_benchmark

`kvpace` is checked against `memtier_benchmark` (Redis Ltd.), a widely used load generator, by running both
against the same server under equivalent settings. If the two tools disagree for a reason that is not explained,
comparisons between servers made with `kvpace` should not be trusted.

## Method

Server: Valkey in a container on the host network, one logical CPU, persistence off, fresh instance for every
run. Workload: 100,000 keys, 64-byte values, uniform keys, 90% GET / 10% SET, key space preloaded, 3 s warm-up
(`kvpace`) and 8 s measurement. Settings of the two tools:

| Connections | `kvpace` | `memtier_benchmark` |
|---|---|---|
| 1 | 1 thread, 1 connection | `-t 1 -c 1` |
| 16 | 16 threads, 1 connection each | `-t 4 -c 4` |

Both tools run pinned to the same client CPUs. Each configuration is repeated and the order of the two tools
alternates between repetitions. Raw files are in the `validation` suite of the benchmark run
(`benchmarks/data/<run>/validation/`).

## Results

<!-- BEGIN GENERATED:validation -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Valkey on one CPU, 64-byte values, 90% GET. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Connections | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|
| 1 | 109,333 (cv 3.5%) | 85,651 (cv 1.7%) | 1.28 | 8.8 (cv 2.2%) | 15.0 (cv 0.0%) | 13 (cv 7.5%) | 20 (cv 22.1%) |
| 16 | 199,259 (cv 1.9%) | 220,849 (cv 1.1%) | 0.90 | 76.5 (cv 1.4%) | 71.0 (cv 0.0%) | 148 (cv 1.9%) | 111 (cv 10.2%) |
<!-- END GENERATED:validation -->

## Interpretation

The differences between the tools are explained by client design, not by the server:

- `kvpace` issues one blocking request per connection and thread; `memtier_benchmark` runs an event loop that
  multiplexes several connections per thread. With one connection the per-request overhead of the generator
  dominates and the tools differ. With many connections the server is the bottleneck, and the burst pattern of the
  multiplexing client lets the server read several requests per wake-up, which lowers its per-request cost.
- A comparison between servers must use one tool for all servers.
- A multiplexed runner would remove the burst difference (listed under known limitations in
  [`ARCHITECTURE.md`](ARCHITECTURE.md)).
