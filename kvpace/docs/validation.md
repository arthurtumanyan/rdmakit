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
Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

One server CPU, 64-byte values, 90% GET. memtier_benchmark runs `-t 1 -c 1` for one connection and `-t 4 -c 4` for 16. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Server | Connections | kvpace client model | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|---|---|
| Valkey (RESP) | 1 | 1 thread, 1 connection | 110,854 (cv 4.2%) | 87,675 (cv 1.1%) | 1.26 | 8.7 (cv 1.6%) | 15.0 (cv 0.0%) | 12 (cv 10.0%) | 15 (cv 0.0%) |
| Valkey (RESP) | 16 | 4 threads x 4 connections | 216,870 (cv 0.2%) | 222,912 (cv 0.3%) | 0.97 | 68.6 (cv 0.0%) | 71.0 (cv 0.0%) | 137 (cv 0.4%) | 100 (cv 4.4%) |
| Valkey (RESP) | 16 | 16 threads x 1 connection | 207,490 (cv 0.4%) | 222,912 (cv 0.3%) | 0.93 | 74.0 (cv 0.6%) | 71.0 (cv 0.0%) | 142 (cv 0.6%) | 100 (cv 4.4%) |
| arenakv (memcached protocol) | 1 | 1 thread, 1 connection | 132,154 (cv 1.0%) | 99,894 (cv 0.6%) | 1.32 | 7.4 (cv 0.9%) | 15.0 (cv 0.0%) | 10 (cv 3.0%) | 15 (cv 0.0%) |
| arenakv (memcached protocol) | 16 | 4 threads x 4 connections | 284,160 (cv 0.4%) | 285,290 (cv 0.4%) | 1.00 | 51.6 (cv 0.4%) | 55.0 (cv 0.0%) | 109 (cv 0.3%) | 111 (cv 0.0%) |
| arenakv (memcached protocol) | 16 | 16 threads x 1 connection | 252,072 (cv 0.3%) | 285,290 (cv 0.4%) | 0.88 | 59.2 (cv 0.4%) | 55.0 (cv 0.0%) | 124 (cv 0.4%) | 111 (cv 0.0%) |
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
