Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

p99 round-trip time in microseconds. Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 45.7 (cv 55.8%) | 9.4 (cv 11.3%) | 13.9 (cv 11.9%) |
| 256 | 14.3 (cv 37.4%) | 9.1 (cv 21.3%) | 14.1 (cv 7.6%) |
| 1024 | 15.5 (cv 21.0%) | 9.1 (cv 12.8%) | 14.8 (cv 38.9%) |
| 4096 | 18.1 (cv 32.0%) | 14.5 (cv 9.1%) | 20.5 (cv 20.6%) |
| 16384 | 19.8 (cv 28.7%) | 38.9 (cv 13.1%) | 45.5 (cv 13.8%) |
| 65536 | 49.7 (cv 73.9%) | 153.2 (cv 16.5%) | 161.1 (cv 13.3%) |
| 262144 | 76.6 (cv 34.2%) | 566.4 (cv 11.1%) | 668.3 (cv 13.0%) |
| 1048576 | 356.1 (cv 45.5%) | 2,377.1 (cv 15.5%) | 2,827.6 (cv 2.2%) |
