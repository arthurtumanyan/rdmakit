Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Server resident memory at the end of the run in MiB.

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 17 (cv 0.2%) | 50 (cv 0.1%) | 17 (cv 0.2%) | 23 (cv 0.3%) | 31 (cv 0.2%) | 21 (cv 0.3%) |
| 64-byte values | 4 | 17 (cv 0.3%) | 50 (cv 0.1%) | 17 (cv 0.1%) | 24 (cv 0.5%) | 34 (cv 2.1%) | 22 (cv 0.5%) |
| 16 KiB values | 1 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 407 (cv 0.0%) | 339 (cv 0.2%) | 341 (cv 0.0%) |
| 16 KiB values | 4 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 451 (cv 0.8%) | 345 (cv 0.0%) | 341 (cv 0.0%) |
| KV-cache requests | 1 | 2,996 (cv 0.2%) | 2,210 (cv 1.5%) | 2,910 (cv 0.4%) | 3,227 (cv 1.6%) | 2,671 (cv 1.1%) | 2,941 (cv 0.3%) |
| KV-cache requests | 4 | 7,224 (cv 1.1%) | 2,757 (cv 2.3%) | 7,588 (cv 1.7%) | 7,262 (cv 1.6%) | 5,042 (cv 1.7%) | 6,812 (cv 3.2%) |
