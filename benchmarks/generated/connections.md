Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, connections spread over 4 client threads, one request in flight per connection. Latencies are GET latencies in microseconds.

| System | Connections | ops/s | p50 | p99 | Server CPU µs/op | Server RSS MiB |
|---|---|---|---|---|---|---|
| arenakv (TCP) | 16 | 242,278 (cv 0.6%) | 61 (cv 0.7%) | 126 (cv 0.6%) | 4.13 (cv 0.6%) | 17 (cv 0.1%) |
| arenakv (TCP) | 64 | 247,043 (cv 0.3%) | 251 (cv 0.3%) | 505 (cv 0.5%) | 4.05 (cv 0.2%) | 17 (cv 0.1%) |
| arenakv (TCP) | 256 | 243,538 (cv 0.3%) | 1,045 (cv 0.3%) | 1,116 (cv 0.8%) | 4.10 (cv 0.4%) | 19 (cv 0.2%) |
| arenakv (TCP) | 1,024 | 218,642 (cv 1.1%) | 4,535 (cv 0.6%) | 6,567 (cv 4.4%) | 4.58 (cv 1.1%) | 27 (cv 0.1%) |
| arenakv (memcached protocol) | 16 | 283,635 (cv 0.5%) | 52 (cv 0.6%) | 109 (cv 0.5%) | 3.53 (cv 0.7%) | 17 (cv 0.2%) |
| arenakv (memcached protocol) | 64 | 292,220 (cv 0.3%) | 210 (cv 0.3%) | 427 (cv 0.3%) | 3.42 (cv 0.3%) | 17 (cv 0.2%) |
| arenakv (memcached protocol) | 256 | 288,338 (cv 0.4%) | 882 (cv 0.5%) | 954 (cv 0.7%) | 3.47 (cv 0.3%) | 19 (cv 0.2%) |
| arenakv (memcached protocol) | 1,024 | 279,751 (cv 1.6%) | 3,601 (cv 1.6%) | 4,705 (cv 5.0%) | 3.58 (cv 1.7%) | 25 (cv 0.1%) |
| Valkey | 16 | 216,696 (cv 0.4%) | 69 (cv 0.3%) | 136 (cv 0.3%) | 4.61 (cv 0.4%) | 23 (cv 0.6%) |
| Valkey | 64 | 224,979 (cv 0.3%) | 274 (cv 0.3%) | 557 (cv 0.0%) | 4.44 (cv 0.3%) | 23 (cv 0.4%) |
| Valkey | 256 | 223,081 (cv 0.7%) | 1,116 (cv 0.6%) | 2,251 (cv 0.8%) | 4.48 (cv 0.7%) | 24 (cv 0.5%) |
| Valkey | 1,024 | 216,391 (cv 1.3%) | 4,542 (cv 1.3%) | 9,136 (cv 1.4%) | 4.62 (cv 1.3%) | 28 (cv 2.3%) |
| Dragonfly | 16 | 211,773 (cv 0.2%) | 75 (cv 0.3%) | 96 (cv 3.9%) | 4.72 (cv 0.2%) | 32 (cv 2.5%) |
| Dragonfly | 64 | 208,327 (cv 0.5%) | 306 (cv 0.6%) | 348 (cv 2.0%) | 4.80 (cv 0.5%) | 34 (cv 1.9%) |
| Dragonfly | 256 | 203,098 (cv 1.0%) | 1,262 (cv 1.1%) | 1,388 (cv 1.5%) | 4.92 (cv 1.0%) | 42 (cv 1.5%) |
| Dragonfly | 1,024 | 176,621 (cv 2.2%) | 5,662 (cv 2.7%) | 7,956 (cv 5.0%) | 5.66 (cv 2.3%) | 75 (cv 0.9%) |
| memcached | 16 | 257,889 (cv 0.3%) | 56 (cv 0.6%) | 123 (cv 0.7%) | 3.88 (cv 0.3%) | 21 (cv 0.2%) |
| memcached | 64 | 262,049 (cv 1.0%) | 232 (cv 1.2%) | 475 (cv 1.1%) | 3.81 (cv 1.0%) | 21 (cv 0.2%) |
| memcached | 256 | 256,433 (cv 1.1%) | 981 (cv 1.0%) | 1,843 (cv 8.2%) | 3.90 (cv 1.1%) | 22 (cv 0.2%) |
| memcached | 1,024 | 248,839 (cv 0.7%) | 4,027 (cv 0.4%) | 5,806 (cv 4.5%) | 4.02 (cv 0.8%) | 22 (cv 0.5%) |
