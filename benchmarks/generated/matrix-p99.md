Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

GET latency p99 in microseconds in the closed loop (KV-cache rows: request latency).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 141 (cv 0.4%) | 142 (cv 1.1%) | 125 (cv 0.8%) | 142 (cv 0.6%) | 99 (cv 1.8%) | 145 (cv 1.4%) |
| 64-byte values | 4 | 40 (cv 1.6%) | 321 (cv 10.2%) | 33 (cv 2.4%) | 62 (cv 16.9%) | 66 (cv 2.3%) | 41 (cv 14.7%) |
| 16 KiB values | 1 | 219 (cv 1.1%) | 321 (cv 14.7%) | 206 (cv 3.8%) | 157 (cv 0.9%) | 156 (cv 8.7%) | 228 (cv 0.5%) |
| 16 KiB values | 4 | 62 (cv 6.0%) | 585 (cv 1.6%) | 58 (cv 7.7%) | 96 (cv 21.5%) | 92 (cv 13.0%) | 84 (cv 11.9%) |
| KV-cache requests | 1 | 24,196 (cv 2.4%) | 25,192 (cv 2.1%) | 22,754 (cv 2.8%) | 19,399 (cv 0.5%) | 19,215 (cv 1.6%) | 24,091 (cv 2.5%) |
| KV-cache requests | 4 | 7,202 (cv 12.9%) | 22,492 (cv 4.8%) | 6,547 (cv 13.6%) | 8,415 (cv 5.6%) | 31,850 (cv 9.4%) | 8,375 (cv 11.0%) |
