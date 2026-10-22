Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 16 connections, 50% PUT of 64-byte values (100,000 keys). The log is written to the NVMe disk of the host. The replica runs on another physical core; replication is asynchronous, so the primary's numbers include sending the changes but not waiting for them. Latencies in microseconds; ratio: throughput relative to no persistence of the same system.

| System | Durability | ops/s | Ratio | PUT p50 | PUT p99 | GET p50 | GET p99 | Server CPU µs/op |
|---|---|---|---|---|---|---|---|---|
| arenakv (TCP) | no persistence | 266,408 (cv 0.4%) | 1.00 | 56 (cv 0.8%) | 117 (cv 0.5%) | 56 (cv 0.8%) | 117 (cv 0.5%) | 3.75 (cv 0.4%) |
| arenakv (TCP) | log, fsync every second | 251,893 (cv 1.3%) | 0.95 | 57 (cv 1.0%) | 119 (cv 1.1%) | 57 (cv 1.0%) | 119 (cv 1.1%) | 3.97 (cv 1.3%) |
| arenakv (TCP) | log, fsync per write | 2,633 (cv 0.3%) | 0.01 | 12,177 (cv 0.5%) | 12,911 (cv 2.6%) | 16 (cv 7.1%) | 121 (cv 9.1%) | 6.08 (cv 8.6%) |
| arenakv (TCP) | one replica | 261,512 (cv 0.6%) | 0.98 | 57 (cv 0.9%) | 119 (cv 0.5%) | 57 (cv 0.8%) | 119 (cv 0.5%) | 3.82 (cv 0.5%) |
| Valkey | no persistence | 208,235 (cv 0.5%) | 1.00 | 74 (cv 0.4%) | 142 (cv 0.4%) | 73 (cv 0.3%) | 141 (cv 0.5%) | 4.80 (cv 0.4%) |
| Valkey | log, fsync every second | 196,569 (cv 0.6%) | 0.94 | 76 (cv 0.3%) | 148 (cv 0.3%) | 76 (cv 0.3%) | 147 (cv 0.6%) | 5.01 (cv 0.9%) |
| Valkey | log, fsync per write | 1,642 (cv 17.3%) | 0.01 | 11,089 (cv 24.2%) | 13,186 (cv 5.3%) | 11,089 (cv 24.2%) | 13,120 (cv 4.9%) | 9.28 (cv 17.9%) |
| Valkey | one replica | 196,479 (cv 0.6%) | 0.94 | 79 (cv 0.7%) | 140 (cv 5.4%) | 79 (cv 0.5%) | 127 (cv 8.0%) | 5.09 (cv 0.6%) |
