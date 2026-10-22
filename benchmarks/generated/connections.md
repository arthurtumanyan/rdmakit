Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, connections spread over 4 client threads, one request in flight per connection. Latencies are GET latencies in microseconds. Server kernel share: kernel-mode part of the server CPU time (stime in /proc/<pid>/stat), that is system calls and the TCP stack run on the server's behalf.

| System | Connections | ops/s | p50 | p99 | Server CPU µs/op | Server kernel share | Server RSS MiB |
|---|---|---|---|---|---|---|---|
| arenakv (TCP) | 16 | 307,751 (cv 0.5%) | 48 (cv 0.5%) | 100 (cv 0.3%) | 3.25 (cv 0.4%) | 86% (cv 0.2%) | 17 (cv 0.1%) |
| arenakv (TCP) | 64 | 318,082 (cv 0.6%) | 193 (cv 0.8%) | 394 (cv 0.6%) | 3.15 (cv 0.7%) | 85% (cv 0.3%) | 17 (cv 0.1%) |
| arenakv (TCP) | 256 | 313,399 (cv 0.5%) | 813 (cv 0.3%) | 878 (cv 2.0%) | 3.19 (cv 0.4%) | 83% (cv 1.1%) | 19 (cv 0.2%) |
| arenakv (TCP) | 1,024 | 291,570 (cv 1.0%) | 3,464 (cv 0.9%) | 4,699 (cv 3.9%) | 3.43 (cv 1.0%) | 79% (cv 0.1%) | 27 (cv 0.1%) |
| arenakv (memcached protocol) | 16 | 295,386 (cv 0.5%) | 50 (cv 0.7%) | 105 (cv 0.3%) | 3.38 (cv 0.4%) | 85% (cv 1.0%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 64 | 301,878 (cv 0.4%) | 204 (cv 0.7%) | 413 (cv 0.4%) | 3.31 (cv 0.4%) | 84% (cv 0.3%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 256 | 300,454 (cv 1.0%) | 846 (cv 1.1%) | 934 (cv 2.5%) | 3.33 (cv 0.9%) | 83% (cv 1.2%) | 19 (cv 0.2%) |
| arenakv (memcached protocol) | 1,024 | 291,933 (cv 0.2%) | 3,454 (cv 0.4%) | 4,594 (cv 3.7%) | 3.42 (cv 0.3%) | 81% (cv 1.0%) | 25 (cv 0.1%) |
| Valkey | 16 | 221,757 (cv 0.4%) | 67 (cv 0.4%) | 134 (cv 0.4%) | 4.51 (cv 0.4%) | 83% (cv 0.6%) | 23 (cv 0.5%) |
| Valkey | 64 | 229,629 (cv 0.5%) | 269 (cv 0.6%) | 546 (cv 0.6%) | 4.35 (cv 0.5%) | 82% (cv 0.7%) | 23 (cv 0.5%) |
| Valkey | 256 | 229,266 (cv 0.4%) | 1,086 (cv 0.4%) | 2,189 (cv 0.4%) | 4.36 (cv 0.4%) | 81% (cv 0.5%) | 24 (cv 0.6%) |
| Valkey | 1,024 | 224,847 (cv 0.6%) | 4,424 (cv 0.5%) | 8,821 (cv 0.4%) | 4.45 (cv 0.5%) | 80% (cv 0.4%) | 27 (cv 4.2%) |
| Dragonfly | 16 | 216,197 (cv 0.6%) | 73 (cv 0.4%) | 93 (cv 2.1%) | 4.63 (cv 0.6%) | 74% (cv 1.0%) | 32 (cv 2.3%) |
| Dragonfly | 64 | 213,789 (cv 0.4%) | 298 (cv 0.4%) | 334 (cv 1.2%) | 4.68 (cv 0.4%) | 74% (cv 0.8%) | 34 (cv 2.3%) |
| Dragonfly | 256 | 208,649 (cv 0.7%) | 1,227 (cv 0.9%) | 1,347 (cv 1.3%) | 4.79 (cv 0.7%) | 73% (cv 0.7%) | 42 (cv 0.2%) |
| Dragonfly | 1,024 | 181,502 (cv 3.0%) | 5,531 (cv 3.1%) | 7,661 (cv 4.1%) | 5.51 (cv 3.1%) | 65% (cv 1.9%) | 75 (cv 0.9%) |
| memcached | 16 | 264,996 (cv 0.5%) | 54 (cv 0.6%) | 120 (cv 1.2%) | 3.77 (cv 0.5%) | 82% (cv 0.6%) | 21 (cv 0.3%) |
| memcached | 64 | 269,749 (cv 0.6%) | 225 (cv 0.7%) | 461 (cv 1.5%) | 3.71 (cv 0.6%) | 81% (cv 0.8%) | 21 (cv 0.3%) |
| memcached | 256 | 264,057 (cv 2.3%) | 947 (cv 1.3%) | 1,619 (cv 20.6%) | 3.79 (cv 2.3%) | 79% (cv 1.3%) | 22 (cv 0.1%) |
| memcached | 1,024 | 258,312 (cv 1.7%) | 3,876 (cv 1.7%) | 5,964 (cv 5.2%) | 3.87 (cv 1.7%) | 76% (cv 0.4%) | 22 (cv 0.3%) |
