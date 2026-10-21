Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Operations per second (KV-cache rows: requests per second).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 267,146 (cv 2.1%) | 226,389 (cv 0.9%) | 258,139 (cv 1.7%) | 210,691 (cv 1.5%) | 204,423 (cv 3.4%) | 232,434 (cv 0.9%) |
| 64-byte values | 4 | 723,389 (cv 4.4%) | 244,918 (cv 4.5%) | 805,570 (cv 1.2%) | 477,558 (cv 4.0%) | 529,566 (cv 2.1%) | 725,576 (cv 4.6%) |
| 16 KiB values | 1 | 157,518 (cv 3.9%) | 118,128 (cv 1.9%) | 159,958 (cv 0.8%) | 143,522 (cv 0.4%) | 136,874 (cv 0.6%) | 144,302 (cv 2.9%) |
| 16 KiB values | 4 | 510,367 (cv 3.9%) | 133,901 (cv 3.7%) | 517,462 (cv 6.8%) | 342,350 (cv 4.3%) | 377,634 (cv 6.3%) | 493,253 (cv 3.5%) |
| KV-cache requests | 1 | 2,034 (cv 2.3%) | 1,445 (cv 1.7%) | 1,993 (cv 0.7%) | 1,758 (cv 1.1%) | 1,772 (cv 5.1%) | 1,862 (cv 2.6%) |
| KV-cache requests | 4 | 4,704 (cv 3.2%) | 1,762 (cv 6.0%) | 4,864 (cv 2.4%) | 3,885 (cv 1.6%) | 2,340 (cv 5.0%) | 4,427 (cv 4.3%) |
