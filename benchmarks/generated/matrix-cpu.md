Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

Server CPU time per operation in microseconds (KV-cache rows: per block GET or PUT).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 4.51 (cv 0.4%) | 4.91 (cv 0.8%) | 3.99 (cv 1.1%) | 4.82 (cv 0.5%) | 4.89 (cv 0.6%) | 4.42 (cv 0.7%) |
| 64-byte values | 4 | 5.31 (cv 0.7%) | 10.04 (cv 2.4%) | 4.69 (cv 0.7%) | 7.48 (cv 1.0%) | 6.91 (cv 1.4%) | 5.13 (cv 1.6%) |
| 16 KiB values | 1 | 6.95 (cv 1.1%) | 8.91 (cv 2.3%) | 6.48 (cv 2.1%) | 7.11 (cv 0.3%) | 7.42 (cv 0.9%) | 6.90 (cv 0.4%) |
| 16 KiB values | 4 | 7.75 (cv 1.2%) | 11.50 (cv 0.8%) | 7.09 (cv 1.9%) | 10.77 (cv 1.8%) | 9.39 (cv 2.0%) | 7.50 (cv 1.1%) |
| KV-cache requests | 1 | 7.10 (cv 0.6%) | 9.37 (cv 0.9%) | 6.71 (cv 1.1%) | 7.53 (cv 0.5%) | 7.40 (cv 0.6%) | 7.07 (cv 1.1%) |
| KV-cache requests | 4 | 8.37 (cv 1.4%) | 11.93 (cv 0.8%) | 7.87 (cv 1.9%) | 11.34 (cv 0.8%) | 11.74 (cv 5.6%) | 8.03 (cv 1.7%) |
