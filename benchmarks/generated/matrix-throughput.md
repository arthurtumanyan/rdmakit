Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Operations per second (KV-cache rows: requests per second).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 267,326 (cv 1.3%) | 226,745 (cv 0.5%) | 261,541 (cv 1.1%) | 210,303 (cv 0.5%) | 209,497 (cv 0.6%) | 231,548 (cv 0.7%) |
| 64-byte values | 4 | 725,006 (cv 3.5%) | 246,569 (cv 3.2%) | 782,749 (cv 1.2%) | 475,480 (cv 2.7%) | 529,027 (cv 5.1%) | 740,397 (cv 3.3%) |
| 16 KiB values | 1 | 160,180 (cv 2.3%) | 118,380 (cv 2.3%) | 159,022 (cv 0.4%) | 142,610 (cv 0.2%) | 136,526 (cv 0.5%) | 147,010 (cv 0.4%) |
| 16 KiB values | 4 | 509,698 (cv 2.2%) | 142,196 (cv 6.8%) | 510,336 (cv 1.7%) | 336,293 (cv 2.5%) | 385,691 (cv 1.5%) | 491,118 (cv 1.5%) |
| KV-cache requests | 1 | 2,052 (cv 0.3%) | 1,449 (cv 2.4%) | 1,991 (cv 0.4%) | 1,749 (cv 2.3%) | 1,784 (cv 1.6%) | 1,881 (cv 0.3%) |
| KV-cache requests | 4 | 4,733 (cv 1.6%) | 1,764 (cv 3.6%) | 5,002 (cv 1.8%) | 3,940 (cv 1.6%) | 2,791 (cv 3.7%) | 4,266 (cv 3.7%) |
