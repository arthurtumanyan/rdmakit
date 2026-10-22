Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

GET latency p99 in microseconds in the closed loop (KV-cache rows: request latency).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 116 (cv 1.0%) | 125 (cv 0.6%) | 120 (cv 1.1%) | 140 (cv 0.6%) | 93 (cv 5.1%) | 139 (cv 1.9%) |
| 64-byte values | 4 | 55 (cv 27.9%) | 364 (cv 22.8%) | 56 (cv 16.8%) | 165 (cv 41.9%) | 113 (cv 45.3%) | 71 (cv 18.7%) |
| 16 KiB values | 1 | 198 (cv 5.1%) | 317 (cv 9.8%) | 197 (cv 0.5%) | 152 (cv 0.6%) | 144 (cv 1.2%) | 224 (cv 0.5%) |
| 16 KiB values | 4 | 92 (cv 7.8%) | 745 (cv 23.8%) | 88 (cv 10.6%) | 188 (cv 7.4%) | 175 (cv 22.1%) | 140 (cv 9.0%) |
| KV-cache requests | 1 | 21,601 (cv 1.5%) | 26,110 (cv 8.4%) | 22,807 (cv 2.7%) | 19,504 (cv 4.6%) | 19,608 (cv 7.0%) | 24,327 (cv 3.2%) |
| KV-cache requests | 4 | 10,853 (cv 25.1%) | 34,420 (cv 19.5%) | 7,661 (cv 18.6%) | 13,631 (cv 24.4%) | 46,137 (cv 5.5%) | 12,242 (cv 21.4%) |
