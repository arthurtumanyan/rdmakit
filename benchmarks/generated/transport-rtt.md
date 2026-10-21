Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Median round-trip time in microseconds (ping-pong, one message in flight). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 7.3 (cv 1.6%) | 7.0 (cv 9.2%) | 10.2 (cv 7.5%) |
| 256 | 7.1 (cv 3.3%) | 6.6 (cv 2.4%) | 10.0 (cv 2.8%) |
| 1024 | 7.6 (cv 4.4%) | 6.7 (cv 1.9%) | 10.5 (cv 3.2%) |
| 4096 | 8.4 (cv 2.5%) | 11.1 (cv 1.9%) | 15.3 (cv 3.2%) |
| 16384 | 11.1 (cv 5.2%) | 30.2 (cv 3.0%) | 34.4 (cv 2.2%) |
| 65536 | 22.1 (cv 2.6%) | 110.9 (cv 8.4%) | 112.5 (cv 0.8%) |
| 262144 | 49.2 (cv 8.5%) | 440.4 (cv 1.6%) | 450.9 (cv 1.2%) |
| 1048576 | 181.9 (cv 2.0%) | 1,864.1 (cv 1.0%) | 1,927.9 (cv 1.2%) |
