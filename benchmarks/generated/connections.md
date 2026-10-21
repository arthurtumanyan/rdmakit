Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, connections spread over 4 client threads, one request in flight per connection. Latencies are GET latencies in microseconds. Server kernel share: kernel-mode part of the server CPU time (stime in /proc/<pid>/stat), that is system calls and the TCP stack run on the server's behalf.

| System | Connections | ops/s | p50 | p99 | Server CPU µs/op | Server kernel share | Server RSS MiB |
|---|---|---|---|---|---|---|---|
| arenakv (TCP) | 16 | 305,443 (cv 2.2%) | 48 (cv 1.9%) | 102 (cv 2.7%) | 3.27 (cv 2.2%) | 85% (cv 1.5%) | 17 (cv 0.2%) |
| arenakv (TCP) | 64 | 316,202 (cv 1.7%) | 194 (cv 1.4%) | 396 (cv 1.7%) | 3.16 (cv 1.8%) | 84% (cv 1.8%) | 17 (cv 0.1%) |
| arenakv (TCP) | 256 | 314,839 (cv 0.4%) | 808 (cv 0.4%) | 888 (cv 2.4%) | 3.18 (cv 0.4%) | 83% (cv 0.6%) | 19 (cv 0.1%) |
| arenakv (TCP) | 1,024 | 293,097 (cv 1.0%) | 3,421 (cv 0.8%) | 4,666 (cv 4.8%) | 3.41 (cv 0.9%) | 80% (cv 0.7%) | 27 (cv 0.1%) |
| arenakv (memcached protocol) | 16 | 295,820 (cv 0.3%) | 50 (cv 0.6%) | 104 (cv 0.4%) | 3.38 (cv 0.3%) | 85% (cv 0.9%) | 17 (cv 0.2%) |
| arenakv (memcached protocol) | 64 | 302,849 (cv 0.7%) | 204 (cv 0.6%) | 412 (cv 0.8%) | 3.30 (cv 0.6%) | 84% (cv 0.5%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 256 | 298,285 (cv 0.8%) | 854 (cv 0.7%) | 938 (cv 1.5%) | 3.35 (cv 0.8%) | 83% (cv 1.1%) | 19 (cv 0.2%) |
| arenakv (memcached protocol) | 1,024 | 293,182 (cv 0.9%) | 3,447 (cv 1.0%) | 4,476 (cv 4.4%) | 3.41 (cv 1.0%) | 81% (cv 0.5%) | 25 (cv 0.0%) |
| Valkey | 16 | 222,157 (cv 0.6%) | 67 (cv 0.5%) | 133 (cv 0.5%) | 4.50 (cv 0.6%) | 83% (cv 0.5%) | 23 (cv 0.3%) |
| Valkey | 64 | 229,176 (cv 0.6%) | 269 (cv 0.3%) | 546 (cv 0.4%) | 4.36 (cv 0.6%) | 82% (cv 0.6%) | 23 (cv 0.3%) |
| Valkey | 256 | 220,906 (cv 6.9%) | 1,134 (cv 7.5%) | 2,284 (cv 7.5%) | 4.55 (cv 7.6%) | 81% (cv 0.4%) | 24 (cv 0.4%) |
| Valkey | 1,024 | 222,453 (cv 4.1%) | 4,470 (cv 3.5%) | 8,913 (cv 3.3%) | 4.50 (cv 4.4%) | 79% (cv 0.9%) | 26 (cv 2.8%) |
| Dragonfly | 16 | 215,452 (cv 1.5%) | 73 (cv 1.4%) | 92 (cv 4.8%) | 4.64 (cv 1.6%) | 74% (cv 0.8%) | 31 (cv 0.3%) |
| Dragonfly | 64 | 212,071 (cv 0.2%) | 300 (cv 0.4%) | 338 (cv 1.4%) | 4.71 (cv 0.3%) | 74% (cv 0.7%) | 34 (cv 2.3%) |
| Dragonfly | 256 | 209,450 (cv 0.5%) | 1,221 (cv 0.5%) | 1,362 (cv 1.6%) | 4.78 (cv 0.5%) | 72% (cv 0.7%) | 42 (cv 1.4%) |
| Dragonfly | 1,024 | 178,908 (cv 6.3%) | 5,643 (cv 7.1%) | 7,818 (cv 4.6%) | 5.61 (cv 6.9%) | 65% (cv 2.0%) | 75 (cv 1.1%) |
| memcached | 16 | 265,949 (cv 0.6%) | 54 (cv 0.7%) | 120 (cv 0.6%) | 3.76 (cv 0.6%) | 82% (cv 0.7%) | 21 (cv 0.3%) |
| memcached | 64 | 271,502 (cv 0.6%) | 224 (cv 0.6%) | 459 (cv 0.6%) | 3.68 (cv 0.5%) | 82% (cv 0.8%) | 21 (cv 0.2%) |
| memcached | 256 | 261,604 (cv 2.9%) | 961 (cv 2.7%) | 1,674 (cv 14.3%) | 3.82 (cv 3.0%) | 79% (cv 2.7%) | 22 (cv 0.2%) |
| memcached | 1,024 | 258,673 (cv 0.8%) | 3,880 (cv 0.6%) | 5,675 (cv 9.9%) | 3.86 (cv 0.8%) | 77% (cv 0.4%) | 22 (cv 0.2%) |
