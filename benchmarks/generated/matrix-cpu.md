Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Server CPU time per operation in microseconds (KV-cache rows: per block GET or PUT).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 3.74 (cv 2.2%) | 4.42 (cv 0.9%) | 3.87 (cv 1.7%) | 4.75 (cv 1.6%) | 4.89 (cv 3.5%) | 4.30 (cv 0.9%) |
| 64-byte values | 4 | 5.13 (cv 4.9%) | 9.20 (cv 3.6%) | 4.92 (cv 1.2%) | 8.39 (cv 4.1%) | 7.39 (cv 2.2%) | 5.45 (cv 4.2%) |
| 16 KiB values | 1 | 6.35 (cv 4.0%) | 8.45 (cv 1.9%) | 6.25 (cv 0.8%) | 6.97 (cv 0.4%) | 7.30 (cv 0.6%) | 6.94 (cv 3.0%) |
| 16 KiB values | 4 | 7.74 (cv 3.9%) | 11.44 (cv 4.0%) | 7.68 (cv 6.4%) | 11.69 (cv 4.3%) | 10.32 (cv 6.3%) | 8.07 (cv 3.4%) |
| KV-cache requests | 1 | 6.34 (cv 2.5%) | 9.24 (cv 1.8%) | 6.49 (cv 0.7%) | 7.47 (cv 1.3%) | 7.42 (cv 5.9%) | 7.00 (cv 2.9%) |
| KV-cache requests | 4 | 8.38 (cv 4.1%) | 11.96 (cv 5.2%) | 8.60 (cv 2.5%) | 12.34 (cv 1.7%) | 12.68 (cv 3.0%) | 8.26 (cv 3.7%) |
