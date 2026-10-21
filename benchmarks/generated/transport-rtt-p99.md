Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

p99 round-trip time in microseconds. Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 14.1 (cv 20.4%) | 9.3 (cv 13.7%) | 13.0 (cv 10.8%) |
| 256 | 10.3 (cv 31.0%) | 7.9 (cv 3.6%) | 12.1 (cv 6.6%) |
| 1024 | 15.3 (cv 39.1%) | 7.9 (cv 2.9%) | 13.2 (cv 1.8%) |
| 4096 | 13.3 (cv 29.6%) | 13.9 (cv 7.3%) | 18.7 (cv 5.5%) |
| 16384 | 14.7 (cv 28.8%) | 36.3 (cv 11.9%) | 43.6 (cv 6.8%) |
| 65536 | 49.8 (cv 57.9%) | 125.1 (cv 4.3%) | 154.0 (cv 8.0%) |
| 262144 | 69.4 (cv 43.3%) | 552.9 (cv 14.4%) | 672.6 (cv 6.7%) |
| 1048576 | 305.2 (cv 48.7%) | 2,082.4 (cv 5.4%) | 2,651.0 (cv 12.7%) |
