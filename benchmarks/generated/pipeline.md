Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, 16 connections from 4 client threads, with the given number of requests in flight per connection. Latencies are GET latencies in microseconds. Server kernel share: kernel-mode part of the server CPU time (stime in /proc/<pid>/stat), that is system calls and the TCP stack run on the server's behalf.

| System | Depth | ops/s | p50 | p99 | Server CPU µs/op | Server kernel share | Server RSS MiB |
|---|---|---|---|---|---|---|---|
| arenakv (TCP) | 1 | 308,298 (cv 0.2%) | 48 (cv 0.2%) | 100 (cv 0.6%) | 3.24 (cv 0.4%) | 86% (cv 0.3%) | 17 (cv 0.0%) |
| arenakv (TCP) | 8 | 2,036,734 (cv 0.6%) | 57 (cv 0.8%) | 121 (cv 0.5%) | 0.49 (cv 0.6%) | 74% (cv 0.8%) | 17 (cv 0.1%) |
| arenakv (TCP) | 32 | 5,265,280 (cv 0.7%) | 87 (cv 1.0%) | 186 (cv 0.9%) | 0.19 (cv 0.6%) | 53% (cv 1.8%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 1 | 295,667 (cv 0.6%) | 50 (cv 0.8%) | 105 (cv 0.6%) | 3.38 (cv 0.6%) | 85% (cv 0.2%) | 17 (cv 0.2%) |
| arenakv (memcached protocol) | 8 | 1,717,097 (cv 0.4%) | 69 (cv 0.6%) | 144 (cv 0.3%) | 0.58 (cv 0.4%) | 64% (cv 1.2%) | 17 (cv 0.3%) |
| arenakv (memcached protocol) | 32 | 3,626,581 (cv 0.4%) | 129 (cv 0.5%) | 273 (cv 0.8%) | 0.28 (cv 0.4%) | 38% (cv 1.3%) | 17 (cv 0.1%) |
| Valkey | 1 | 221,048 (cv 0.6%) | 67 (cv 0.4%) | 134 (cv 0.4%) | 4.52 (cv 0.7%) | 83% (cv 0.9%) | 23 (cv 0.4%) |
| Valkey | 8 | 1,162,288 (cv 0.4%) | 101 (cv 0.4%) | 170 (cv 1.9%) | 0.86 (cv 0.4%) | 56% (cv 1.8%) | 23 (cv 0.3%) |
| Valkey | 32 | 2,239,084 (cv 0.5%) | 187 (cv 0.5%) | 348 (cv 0.9%) | 0.45 (cv 0.4%) | 30% (cv 0.7%) | 23 (cv 0.4%) |
| Dragonfly | 1 | 215,365 (cv 0.6%) | 73 (cv 0.8%) | 93 (cv 3.6%) | 4.64 (cv 0.5%) | 74% (cv 0.6%) | 32 (cv 2.4%) |
| Dragonfly | 8 | 816,860 (cv 0.3%) | 143 (cv 0.3%) | 249 (cv 1.6%) | 1.23 (cv 0.4%) | 38% (cv 1.3%) | 32 (cv 0.2%) |
| Dragonfly | 32 | 1,299,547 (cv 0.6%) | 350 (cv 0.6%) | 661 (cv 1.2%) | 0.77 (cv 0.6%) | 19% (cv 4.2%) | 33 (cv 0.3%) |
| memcached | 1 | 265,316 (cv 0.4%) | 54 (cv 0.5%) | 119 (cv 0.6%) | 3.77 (cv 0.4%) | 81% (cv 0.3%) | 21 (cv 0.2%) |
| memcached | 8 | 1,221,762 (cv 0.6%) | 87 (cv 0.8%) | 281 (cv 0.9%) | 0.82 (cv 0.7%) | 56% (cv 0.9%) | 21 (cv 0.2%) |
| memcached | 32 | 1,570,030 (cv 0.8%) | 285 (cv 0.5%) | 570 (cv 3.5%) | 0.64 (cv 0.8%) | 45% (cv 1.3%) | 22 (cv 0.4%) |
