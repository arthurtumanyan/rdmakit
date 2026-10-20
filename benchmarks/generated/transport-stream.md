Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

One-way streaming throughput in MB/s (batched sends, 64 buffers per direction). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 28 (cv 12.9%) | 46 (cv 6.9%) | 46 (cv 10.3%) |
| 256 | 103 (cv 0.8%) | 189 (cv 2.8%) | 184 (cv 7.5%) |
| 1024 | 329 (cv 3.5%) | 722 (cv 5.8%) | 688 (cv 8.5%) |
| 4096 | 1,228 (cv 3.9%) | 910 (cv 3.4%) | 962 (cv 1.6%) |
| 16384 | 3,667 (cv 5.0%) | 1,135 (cv 3.4%) | 1,145 (cv 0.8%) |
| 65536 | 6,963 (cv 5.3%) | 1,280 (cv 1.5%) | 1,239 (cv 2.7%) |
| 262144 | 8,726 (cv 3.3%) | 1,174 (cv 3.3%) | 1,121 (cv 6.5%) |
| 1048576 | 7,990 (cv 2.7%) | 1,106 (cv 3.1%) | 1,086 (cv 2.5%) |
