Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

GET latency p99 in microseconds in the closed loop (KV-cache rows: request latency).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 118 (cv 2.2%) | 125 (cv 1.2%) | 121 (cv 1.6%) | 139 (cv 1.7%) | 111 (cv 14.3%) | 140 (cv 1.5%) |
| 64-byte values | 4 | 68 (cv 18.4%) | 377 (cv 26.3%) | 48 (cv 18.3%) | 205 (cv 40.7%) | 116 (cv 7.9%) | 80 (cv 20.1%) |
| 16 KiB values | 1 | 205 (cv 7.5%) | 306 (cv 3.9%) | 196 (cv 1.3%) | 152 (cv 1.5%) | 152 (cv 6.9%) | 236 (cv 8.8%) |
| 16 KiB values | 4 | 89 (cv 23.5%) | 900 (cv 8.9%) | 97 (cv 33.6%) | 190 (cv 64.1%) | 204 (cv 27.0%) | 141 (cv 4.2%) |
| KV-cache requests | 1 | 22,282 (cv 4.5%) | 25,585 (cv 6.7%) | 22,282 (cv 2.0%) | 19,268 (cv 1.7%) | 19,451 (cv 7.4%) | 24,432 (cv 4.8%) |
| KV-cache requests | 4 | 12,052 (cv 33.0%) | 39,846 (cv 32.6%) | 10,951 (cv 26.5%) | 14,523 (cv 27.7%) | 44,984 (cv 7.9%) | 11,318 (cv 27.9%) |
