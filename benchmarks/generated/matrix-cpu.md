Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Server CPU time per operation in microseconds (KV-cache rows: per block GET or PUT).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 3.74 (cv 1.3%) | 4.41 (cv 0.5%) | 3.82 (cv 1.1%) | 4.75 (cv 0.4%) | 4.78 (cv 0.6%) | 4.32 (cv 0.6%) |
| 64-byte values | 4 | 5.09 (cv 3.7%) | 9.19 (cv 3.2%) | 5.07 (cv 1.7%) | 8.42 (cv 2.7%) | 7.42 (cv 4.9%) | 5.36 (cv 3.1%) |
| 16 KiB values | 1 | 6.24 (cv 2.3%) | 8.44 (cv 2.4%) | 6.29 (cv 0.4%) | 7.01 (cv 0.3%) | 7.32 (cv 0.5%) | 6.80 (cv 0.3%) |
| 16 KiB values | 4 | 7.76 (cv 2.2%) | 10.92 (cv 5.5%) | 7.77 (cv 2.3%) | 11.89 (cv 2.6%) | 10.11 (cv 1.6%) | 8.09 (cv 1.6%) |
| KV-cache requests | 1 | 6.28 (cv 0.3%) | 9.20 (cv 2.5%) | 6.49 (cv 0.5%) | 7.51 (cv 2.4%) | 7.35 (cv 1.7%) | 6.91 (cv 0.3%) |
| KV-cache requests | 4 | 8.44 (cv 1.5%) | 11.96 (cv 2.9%) | 8.30 (cv 2.3%) | 12.16 (cv 1.7%) | 11.68 (cv 2.9%) | 8.55 (cv 2.2%) |
