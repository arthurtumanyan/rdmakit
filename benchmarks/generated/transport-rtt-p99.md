Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

p99 round-trip time in microseconds. Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 22.5 (cv 39.0%) | 9.3 (cv 9.8%) | 13.1 (cv 10.4%) |
| 256 | 9.3 (cv 2.2%) | 8.5 (cv 4.8%) | 12.6 (cv 7.5%) |
| 1024 | 11.6 (cv 34.8%) | 8.7 (cv 5.7%) | 13.6 (cv 12.3%) |
| 4096 | 12.8 (cv 38.5%) | 13.7 (cv 6.3%) | 18.8 (cv 3.9%) |
| 16384 | 39.3 (cv 81.1%) | 37.1 (cv 9.2%) | 46.5 (cv 5.2%) |
| 65536 | 64.1 (cv 55.9%) | 124.6 (cv 6.9%) | 146.5 (cv 11.8%) |
| 262144 | 80.9 (cv 29.9%) | 485.3 (cv 2.0%) | 664.7 (cv 5.9%) |
| 1048576 | 259.1 (cv 49.5%) | 2,215.1 (cv 14.1%) | 2,649.9 (cv 10.7%) |
