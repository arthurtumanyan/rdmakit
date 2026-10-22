Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, 16 connections from 4 client threads, with the given number of requests in flight per connection. Latencies are GET latencies in microseconds. Server kernel share: kernel-mode part of the server CPU time (stime in /proc/<pid>/stat), that is system calls and the TCP stack run on the server's behalf.

| System | Depth | ops/s | p50 | p99 | Server CPU µs/op | Server kernel share | Server RSS MiB |
|---|---|---|---|---|---|---|---|
| arenakv (TCP) | 1 | 306,433 (cv 0.4%) | 48 (cv 0.7%) | 101 (cv 0.6%) | 3.26 (cv 0.5%) | 86% (cv 0.5%) | 17 (cv 0.2%) |
| arenakv (TCP) | 8 | 2,014,072 (cv 0.7%) | 58 (cv 1.1%) | 122 (cv 0.5%) | 0.50 (cv 0.7%) | 73% (cv 0.8%) | 17 (cv 0.2%) |
| arenakv (TCP) | 32 | 5,130,043 (cv 0.3%) | 90 (cv 0.5%) | 192 (cv 0.4%) | 0.20 (cv 0.4%) | 52% (cv 1.9%) | 17 (cv 0.3%) |
| arenakv (memcached protocol) | 1 | 293,627 (cv 0.7%) | 50 (cv 1.1%) | 105 (cv 0.7%) | 3.40 (cv 0.7%) | 85% (cv 1.0%) | 17 (cv 0.3%) |
| arenakv (memcached protocol) | 8 | 1,688,995 (cv 0.3%) | 70 (cv 0.4%) | 146 (cv 0.7%) | 0.59 (cv 0.3%) | 63% (cv 0.4%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 32 | 3,551,958 (cv 0.4%) | 131 (cv 0.4%) | 281 (cv 0.9%) | 0.28 (cv 0.6%) | 37% (cv 1.8%) | 17 (cv 0.1%) |
| Valkey | 1 | 222,416 (cv 0.4%) | 67 (cv 0.6%) | 133 (cv 0.6%) | 4.49 (cv 0.5%) | 83% (cv 0.9%) | 23 (cv 0.3%) |
| Valkey | 8 | 1,168,070 (cv 0.3%) | 100 (cv 0.4%) | 168 (cv 0.5%) | 0.86 (cv 0.2%) | 56% (cv 1.2%) | 23 (cv 0.5%) |
| Valkey | 32 | 2,250,029 (cv 0.1%) | 186 (cv 0.3%) | 346 (cv 0.4%) | 0.44 (cv 0.0%) | 29% (cv 1.0%) | 23 (cv 0.4%) |
| Dragonfly | 1 | 216,633 (cv 0.4%) | 73 (cv 0.6%) | 91 (cv 2.5%) | 4.61 (cv 0.4%) | 74% (cv 0.9%) | 32 (cv 2.6%) |
| Dragonfly | 8 | 815,180 (cv 0.3%) | 143 (cv 0.4%) | 251 (cv 1.2%) | 1.23 (cv 0.3%) | 38% (cv 0.8%) | 32 (cv 0.2%) |
| Dragonfly | 32 | 1,296,459 (cv 0.8%) | 351 (cv 0.5%) | 673 (cv 3.9%) | 0.77 (cv 0.8%) | 20% (cv 3.6%) | 33 (cv 0.2%) |
| memcached | 1 | 265,205 (cv 0.3%) | 54 (cv 0.4%) | 119 (cv 0.5%) | 3.77 (cv 0.3%) | 82% (cv 0.5%) | 21 (cv 0.1%) |
| memcached | 8 | 1,227,908 (cv 0.2%) | 87 (cv 0.3%) | 279 (cv 0.4%) | 0.81 (cv 0.2%) | 56% (cv 1.4%) | 21 (cv 0.3%) |
| memcached | 32 | 1,583,650 (cv 0.4%) | 283 (cv 0.5%) | 569 (cv 1.2%) | 0.63 (cv 0.5%) | 45% (cv 1.6%) | 22 (cv 0.4%) |
