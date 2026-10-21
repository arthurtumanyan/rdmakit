Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Server resident memory at the end of the run in MiB.

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 17 (cv 0.2%) | 50 (cv 0.0%) | 17 (cv 0.1%) | 23 (cv 0.4%) | 32 (cv 2.1%) | 21 (cv 0.3%) |
| 64-byte values | 4 | 17 (cv 0.3%) | 50 (cv 0.0%) | 17 (cv 0.1%) | 24 (cv 0.3%) | 34 (cv 2.1%) | 22 (cv 0.2%) |
| 16 KiB values | 1 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 407 (cv 0.0%) | 339 (cv 0.2%) | 341 (cv 0.0%) |
| 16 KiB values | 4 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 449 (cv 0.9%) | 345 (cv 0.0%) | 341 (cv 0.0%) |
| KV-cache requests | 1 | 2,988 (cv 1.6%) | 2,204 (cv 1.0%) | 2,913 (cv 0.6%) | 3,233 (cv 1.6%) | 2,640 (cv 4.3%) | 2,923 (cv 2.2%) |
| KV-cache requests | 4 | 7,220 (cv 2.3%) | 2,750 (cv 4.5%) | 7,362 (cv 2.6%) | 7,256 (cv 0.9%) | 4,624 (cv 2.7%) | 7,022 (cv 3.3%) |
