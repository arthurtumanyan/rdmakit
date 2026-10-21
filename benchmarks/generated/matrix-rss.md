Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

Server resident memory at the end of the run in MiB.

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 17 (cv 0.2%) | 50 (cv 0.1%) | 17 (cv 0.1%) | 23 (cv 0.1%) | 32 (cv 2.0%) | 21 (cv 0.2%) |
| 64-byte values | 4 | 17 (cv 0.1%) | 50 (cv 0.1%) | 17 (cv 0.2%) | 24 (cv 0.5%) | 35 (cv 2.1%) | 22 (cv 0.2%) |
| 16 KiB values | 1 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 407 (cv 0.0%) | 339 (cv 0.2%) | 341 (cv 0.0%) |
| 16 KiB values | 4 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 450 (cv 0.7%) | 345 (cv 0.2%) | 341 (cv 0.0%) |
| KV-cache requests | 1 | 2,687 (cv 0.5%) | 2,154 (cv 0.5%) | 2,821 (cv 1.1%) | 3,223 (cv 0.5%) | 2,653 (cv 0.6%) | 2,889 (cv 1.0%) |
| KV-cache requests | 4 | 7,148 (cv 0.7%) | 2,834 (cv 1.0%) | 7,801 (cv 1.0%) | 7,645 (cv 0.5%) | 4,945 (cv 4.8%) | 7,103 (cv 1.7%) |
