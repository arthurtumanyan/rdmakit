Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

Median round-trip time in microseconds (ping-pong, one message in flight). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 9.6 (cv 37.9%) | 6.8 (cv 2.7%) | 10.3 (cv 9.7%) |
| 256 | 7.8 (cv 0.4%) | 6.7 (cv 3.4%) | 10.1 (cv 1.8%) |
| 1024 | 8.1 (cv 2.0%) | 7.2 (cv 2.9%) | 10.7 (cv 3.6%) |
| 4096 | 9.0 (cv 2.5%) | 11.2 (cv 1.8%) | 15.1 (cv 1.7%) |
| 16384 | 16.2 (cv 45.4%) | 30.4 (cv 1.4%) | 35.3 (cv 2.0%) |
| 65536 | 29.2 (cv 35.9%) | 106.5 (cv 0.9%) | 113.4 (cv 1.8%) |
| 262144 | 50.6 (cv 2.3%) | 442.1 (cv 2.7%) | 459.6 (cv 1.9%) |
| 1048576 | 185.7 (cv 1.7%) | 1,898.3 (cv 1.3%) | 1,938.9 (cv 1.1%) |
