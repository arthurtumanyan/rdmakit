Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Valkey on one CPU, 64-byte values, 90% GET. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Connections | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|
| 1 | 109,333 (cv 3.5%) | 85,651 (cv 1.7%) | 1.28 | 8.8 (cv 2.2%) | 15.0 (cv 0.0%) | 13 (cv 7.5%) | 20 (cv 22.1%) |
| 16 | 199,259 (cv 1.9%) | 220,849 (cv 1.1%) | 0.90 | 76.5 (cv 1.4%) | 71.0 (cv 0.0%) | 148 (cv 1.9%) | 111 (cv 10.2%) |
