Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Server CPU time per operation in microseconds (KV-cache rows: per block GET or PUT).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|
| 64-byte values | 1 | 5.91 (cv 1.3%) | 5.26 (cv 2.7%) | 4.95 (cv 1.1%) | 4.96 (cv 1.1%) | 4.52 (cv 0.7%) |
| 64-byte values | 4 | 6.59 (cv 0.9%) | 11.42 (cv 1.6%) | 7.68 (cv 3.7%) | 7.02 (cv 2.0%) | 5.29 (cv 3.3%) |
| 16 KiB values | 1 | 7.86 (cv 0.6%) | 10.08 (cv 1.7%) | 7.38 (cv 1.3%) | 7.64 (cv 3.2%) | 7.15 (cv 1.8%) |
| 16 KiB values | 4 | 8.81 (cv 2.7%) | 12.33 (cv 0.9%) | 11.36 (cv 4.9%) | 9.56 (cv 2.6%) | 7.77 (cv 4.8%) |
| KV-cache requests | 1 | 8.19 (cv 1.8%) | 10.18 (cv 2.7%) | 7.93 (cv 4.9%) | 7.70 (cv 4.1%) | 7.53 (cv 5.1%) |
| KV-cache requests | 4 | 9.26 (cv 2.8%) | 12.72 (cv 1.2%) | 11.64 (cv 1.4%) | 12.95 (cv 6.4%) | 8.29 (cv 3.1%) |
