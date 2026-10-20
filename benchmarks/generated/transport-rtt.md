Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Median round-trip time in microseconds (ping-pong, one message in flight). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 10.2 (cv 30.7%) | 6.7 (cv 1.0%) | 10.0 (cv 3.3%) |
| 256 | 8.5 (cv 5.6%) | 6.7 (cv 2.1%) | 10.2 (cv 1.0%) |
| 1024 | 8.5 (cv 1.5%) | 7.2 (cv 5.5%) | 10.5 (cv 3.5%) |
| 4096 | 10.3 (cv 15.2%) | 11.3 (cv 2.7%) | 15.1 (cv 3.4%) |
| 16384 | 12.2 (cv 2.8%) | 30.4 (cv 1.5%) | 35.5 (cv 3.5%) |
| 65536 | 23.8 (cv 4.4%) | 109.9 (cv 2.2%) | 113.6 (cv 3.8%) |
| 262144 | 59.0 (cv 1.8%) | 446.2 (cv 1.6%) | 456.8 (cv 3.3%) |
| 1048576 | 250.1 (cv 2.6%) | 1,901.0 (cv 1.2%) | 1,941.7 (cv 1.3%) |
