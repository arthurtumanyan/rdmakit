# Validation against memtier_benchmark

`kvpace` is checked against `memtier_benchmark` (Redis Ltd.), a widely used load generator, by running both
against the same server under equivalent settings. If the two tools disagree for a reason that is not explained,
comparisons between servers made with `kvpace` should not be trusted.

## Method

Servers: Valkey (RESP) and arenakv serving the memcached text protocol, each on one logical CPU, persistence off,
fresh instance for every run. Valkey runs in a container on the host network, arenakv as a native process.
Workload: 100,000 keys, 64-byte values, uniform keys, 90% GET / 10% SET, key space preloaded, 3 s warm-up
(`kvpace`) and 8 s measurement. Client settings:

| Connections | `kvpace`, tool `kvpace` | `kvpace`, tool `kvpace-threads` | `memtier_benchmark` |
|---|---|---|---|
| 1 | 1 thread, 1 connection | | `-t 1 -c 1` |
| 16 | `--threads 4 --conns-per-thread 4` (memtier's model) | 16 threads, 1 connection each (the matrix suite's model) | `-t 4 -c 4` |

Both tools run pinned to the same client CPUs. Each configuration is repeated five times and the order of the tools
alternates between repetitions. Raw files are in the `validation` suite of the benchmark run
(`benchmarks/data/<run>/validation/`).

## Results

<!-- BEGIN GENERATED:validation -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

One server CPU, 64-byte values, 90% GET. memtier_benchmark runs `-t 1 -c 1` for one connection and `-t 4 -c 4` for 16. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Server | Connections | kvpace client model | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|---|---|
| Valkey (RESP) | 1 | 1 thread, 1 connection | 116,870 (cv 0.8%) | 89,611 (cv 1.1%) | 1.30 | 8.4 (cv 1.2%) | 15.0 (cv 0.0%) | 11 (cv 1.9%) | 15 (cv 0.0%) |
| Valkey (RESP) | 16 | 4 threads x 4 connections | 221,642 (cv 0.6%) | 227,090 (cv 0.4%) | 0.98 | 67.0 (cv 0.6%) | 71.0 (cv 0.0%) | 133 (cv 0.3%) | 98 (cv 4.5%) |
| Valkey (RESP) | 16 | 16 threads x 1 connection | 209,842 (cv 0.6%) | 227,090 (cv 0.4%) | 0.92 | 72.7 (cv 0.7%) | 71.0 (cv 0.0%) | 140 (cv 0.8%) | 98 (cv 4.5%) |
| arenakv (memcached protocol) | 1 | 1 thread, 1 connection | 136,302 (cv 0.5%) | 101,226 (cv 0.9%) | 1.35 | 7.2 (cv 0.5%) | 15.0 (cv 0.0%) | 9 (cv 2.0%) | 15 (cv 0.0%) |
| arenakv (memcached protocol) | 16 | 4 threads x 4 connections | 295,443 (cv 0.8%) | 293,834 (cv 0.5%) | 1.01 | 49.6 (cv 0.5%) | 50.2 (cv 8.7%) | 104 (cv 0.3%) | 105 (cv 3.4%) |
| arenakv (memcached protocol) | 16 | 16 threads x 1 connection | 258,669 (cv 1.9%) | 293,834 (cv 0.5%) | 0.88 | 57.6 (cv 1.5%) | 50.2 (cv 8.7%) | 120 (cv 1.3%) | 105 (cv 3.4%) |
<!-- END GENERATED:validation -->

## Interpretation

- With the same client model the two tools agree within 3% in throughput with 16 connections, on both servers
  [C21]. The server is the bottleneck there, and both tools offer it the same pattern of requests.
- With one blocking thread per connection kvpace measures 5% to 15% less [C23]: a multiplexing client delivers
  requests in bursts, so the server reads several requests per wake-up and spends less per request. Results of
  the two models must not be compared with each other.
- With one connection kvpace measures at least 25% more throughput [C21] and a p50 latency of 7 to 9 microseconds
  where memtier reports 15. The per-request cost of the generator dominates in this configuration, and kvpace's is
  lower; memtier reports latencies in milliseconds with three decimals, which also limits its resolution.
- A comparison between servers must use one tool and one client model for all servers.
