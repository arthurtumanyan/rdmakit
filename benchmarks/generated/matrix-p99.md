Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

GET latency p99 in microseconds in the closed loop (KV-cache rows: request latency).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|
| 64-byte values | 1 | 171 (cv 1.9%) | 151 (cv 10.7%) | 145 (cv 1.4%) | 103 (cv 1.4%) | 151 (cv 1.6%) |
| 64-byte values | 4 | 55 (cv 2.3%) | 421 (cv 7.5%) | 67 (cv 30.3%) | 60 (cv 17.5%) | 45 (cv 14.7%) |
| 16 KiB values | 1 | 233 (cv 1.4%) | 401 (cv 8.9%) | 181 (cv 6.7%) | 177 (cv 19.2%) | 240 (cv 2.4%) |
| 16 KiB values | 4 | 74 (cv 10.8%) | 700 (cv 3.9%) | 107 (cv 24.5%) | 93 (cv 11.4%) | 103 (cv 18.1%) |
| KV-cache requests | 1 | 21,365 (cv 2.6%) | 30,618 (cv 6.9%) | 20,709 (cv 5.6%) | 20,316 (cv 5.8%) | 25,087 (cv 3.6%) |
| KV-cache requests | 4 | 7,491 (cv 5.7%) | 23,672 (cv 5.8%) | 9,640 (cv 14.7%) | 40,685 (cv 12.5%) | 9,935 (cv 21.0%) |
