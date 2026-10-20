Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Server resident memory at the end of the run in MiB.

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|
| 64-byte values | 1 | 338 (cv 0.1%) | 372 (cv 0.1%) | 23 (cv 0.2%) | 32 (cv 2.1%) | 21 (cv 0.3%) |
| 64-byte values | 4 | 338 (cv 0.1%) | 372 (cv 0.2%) | 24 (cv 0.3%) | 35 (cv 0.1%) | 22 (cv 0.2%) |
| 16 KiB values | 1 | 325 (cv 0.0%) | 358 (cv 0.0%) | 407 (cv 0.0%) | 337 (cv 0.2%) | 341 (cv 0.0%) |
| 16 KiB values | 4 | 325 (cv 0.0%) | 358 (cv 0.0%) | 450 (cv 1.1%) | 346 (cv 0.2%) | 342 (cv 0.0%) |
| KV-cache requests | 1 | 2,382 (cv 1.6%) | 2,015 (cv 1.7%) | 3,071 (cv 4.1%) | 2,559 (cv 3.5%) | 2,734 (cv 4.0%) |
| KV-cache requests | 4 | 6,699 (cv 1.2%) | 2,941 (cv 1.2%) | 7,421 (cv 1.9%) | 4,481 (cv 7.3%) | 6,919 (cv 3.2%) |
