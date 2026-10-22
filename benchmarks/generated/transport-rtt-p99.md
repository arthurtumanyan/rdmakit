Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

p99 round-trip time in microseconds. Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 13.3 (cv 28.7%) | 9.2 (cv 10.5%) | 14.4 (cv 14.1%) |
| 256 | 12.3 (cv 29.1%) | 8.6 (cv 12.0%) | 13.2 (cv 7.8%) |
| 1024 | 13.9 (cv 27.6%) | 8.6 (cv 5.3%) | 15.9 (cv 18.1%) |
| 4096 | 12.0 (cv 26.8%) | 14.1 (cv 1.8%) | 19.3 (cv 13.9%) |
| 16384 | 15.7 (cv 27.8%) | 40.5 (cv 7.6%) | 53.4 (cv 29.7%) |
| 65536 | 48.8 (cv 87.7%) | 149.9 (cv 17.7%) | 166.8 (cv 6.3%) |
| 262144 | 57.7 (cv 7.5%) | 588.8 (cv 9.4%) | 628.0 (cv 3.7%) |
| 1048576 | 241.8 (cv 46.3%) | 2,416.7 (cv 15.8%) | 2,793.8 (cv 2.9%) |
