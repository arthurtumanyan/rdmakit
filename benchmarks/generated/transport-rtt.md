Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Median round-trip time in microseconds (ping-pong, one message in flight). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 7.4 (cv 3.2%) | 6.9 (cv 5.2%) | 10.4 (cv 7.2%) |
| 256 | 7.5 (cv 2.6%) | 6.7 (cv 2.7%) | 10.4 (cv 4.7%) |
| 1024 | 7.7 (cv 4.6%) | 7.1 (cv 3.1%) | 11.2 (cv 7.5%) |
| 4096 | 8.5 (cv 2.4%) | 11.4 (cv 1.8%) | 15.3 (cv 2.9%) |
| 16384 | 10.8 (cv 3.7%) | 30.5 (cv 0.9%) | 35.9 (cv 1.7%) |
| 65536 | 22.7 (cv 1.2%) | 109.5 (cv 3.1%) | 114.0 (cv 5.3%) |
| 262144 | 48.9 (cv 3.4%) | 451.7 (cv 3.1%) | 454.2 (cv 0.8%) |
| 1048576 | 181.0 (cv 1.4%) | 1,889.5 (cv 0.8%) | 1,932.7 (cv 1.1%) |
