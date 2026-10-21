Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, 16 connections from 4 client threads, with the given number of requests in flight per connection. Latencies are GET latencies in microseconds.

| System | Depth | ops/s | p50 | p99 | Server CPU µs/op | Server RSS MiB |
|---|---|---|---|---|---|---|
| arenakv (TCP) | 1 | 242,165 (cv 0.3%) | 61 (cv 0.5%) | 126 (cv 0.3%) | 4.13 (cv 0.3%) | 17 (cv 0.2%) |
| arenakv (TCP) | 8 | 1,403,303 (cv 0.7%) | 84 (cv 0.8%) | 179 (cv 1.1%) | 0.71 (cv 0.8%) | 17 (cv 0.2%) |
| arenakv (TCP) | 32 | 2,211,746 (cv 2.2%) | 212 (cv 2.8%) | 468 (cv 3.4%) | 0.45 (cv 2.3%) | 19 (cv 0.1%) |
| arenakv (memcached protocol) | 1 | 283,322 (cv 0.3%) | 52 (cv 0.4%) | 109 (cv 0.4%) | 3.53 (cv 0.4%) | 17 (cv 0.2%) |
| arenakv (memcached protocol) | 8 | 1,550,965 (cv 1.7%) | 76 (cv 1.7%) | 164 (cv 2.4%) | 0.64 (cv 1.6%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 32 | 3,288,067 (cv 1.5%) | 140 (cv 1.4%) | 313 (cv 2.7%) | 0.30 (cv 1.5%) | 17 (cv 0.1%) |
| Valkey | 1 | 216,647 (cv 0.3%) | 69 (cv 0.5%) | 137 (cv 0.4%) | 4.61 (cv 0.3%) | 23 (cv 0.2%) |
| Valkey | 8 | 1,148,165 (cv 0.1%) | 102 (cv 0.0%) | 172 (cv 1.3%) | 0.87 (cv 0.1%) | 23 (cv 0.5%) |
| Valkey | 32 | 2,221,454 (cv 0.3%) | 189 (cv 0.6%) | 353 (cv 0.5%) | 0.45 (cv 0.3%) | 23 (cv 0.5%) |
| Dragonfly | 1 | 211,326 (cv 0.5%) | 75 (cv 0.6%) | 96 (cv 1.4%) | 4.73 (cv 0.5%) | 32 (cv 2.6%) |
| Dragonfly | 8 | 803,231 (cv 0.4%) | 145 (cv 0.6%) | 254 (cv 0.5%) | 1.24 (cv 0.3%) | 33 (cv 2.0%) |
| Dragonfly | 32 | 1,265,653 (cv 1.4%) | 359 (cv 1.4%) | 690 (cv 1.9%) | 0.79 (cv 1.5%) | 33 (cv 2.0%) |
| memcached | 1 | 256,340 (cv 0.3%) | 56 (cv 0.2%) | 123 (cv 0.8%) | 3.90 (cv 0.3%) | 21 (cv 0.3%) |
| memcached | 8 | 1,180,820 (cv 1.3%) | 90 (cv 0.8%) | 290 (cv 1.9%) | 0.85 (cv 1.3%) | 21 (cv 0.5%) |
| memcached | 32 | 1,526,793 (cv 2.6%) | 295 (cv 2.5%) | 596 (cv 4.6%) | 0.66 (cv 2.7%) | 22 (cv 0.2%) |
