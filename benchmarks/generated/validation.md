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
