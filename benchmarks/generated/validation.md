Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

One server CPU, 64-byte values, 90% GET. memtier_benchmark runs `-t 1 -c 1` for one connection and `-t 4 -c 4` for 16. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Server | Connections | kvpace client model | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|---|---|
| Valkey (RESP) | 1 | 1 thread, 1 connection | 116,950 (cv 0.7%) | 89,760 (cv 0.3%) | 1.30 | 8.4 (cv 0.4%) | 15.0 (cv 0.0%) | 11 (cv 2.4%) | 15 (cv 0.0%) |
| Valkey (RESP) | 16 | 4 threads x 4 connections | 221,077 (cv 0.5%) | 227,432 (cv 0.2%) | 0.97 | 67.3 (cv 0.4%) | 71.0 (cv 0.0%) | 134 (cv 0.4%) | 95 (cv 0.0%) |
| Valkey (RESP) | 16 | 16 threads x 1 connection | 210,438 (cv 0.9%) | 227,432 (cv 0.2%) | 0.93 | 72.8 (cv 0.6%) | 71.0 (cv 0.0%) | 139 (cv 0.8%) | 95 (cv 0.0%) |
| arenakv (memcached protocol) | 1 | 1 thread, 1 connection | 136,463 (cv 1.3%) | 102,194 (cv 0.7%) | 1.34 | 7.2 (cv 1.3%) | 15.0 (cv 0.0%) | 10 (cv 1.7%) | 15 (cv 0.0%) |
| arenakv (memcached protocol) | 16 | 4 threads x 4 connections | 296,016 (cv 0.1%) | 294,108 (cv 0.7%) | 1.01 | 49.6 (cv 0.3%) | 48.6 (cv 7.4%) | 104 (cv 0.6%) | 105 (cv 3.4%) |
| arenakv (memcached protocol) | 16 | 16 threads x 1 connection | 258,259 (cv 0.8%) | 294,108 (cv 0.7%) | 0.88 | 57.9 (cv 1.0%) | 48.6 (cv 7.4%) | 120 (cv 0.6%) | 105 (cv 3.4%) |
