Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

One-way streaming throughput in MB/s (batched sends, 64 buffers per direction). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 301 (cv 17.0%) | 47 (cv 1.7%) | 47 (cv 5.0%) |
| 256 | 1,203 (cv 17.1%) | 190 (cv 5.8%) | 172 (cv 8.8%) |
| 1024 | 3,509 (cv 2.8%) | 763 (cv 3.8%) | 665 (cv 9.0%) |
| 4096 | 8,181 (cv 1.6%) | 930 (cv 4.7%) | 952 (cv 2.5%) |
| 16384 | 11,527 (cv 2.7%) | 1,154 (cv 2.6%) | 1,146 (cv 1.6%) |
| 65536 | 9,865 (cv 11.8%) | 1,300 (cv 2.0%) | 1,270 (cv 1.3%) |
| 262144 | 5,430 (cv 13.4%) | 1,196 (cv 4.7%) | 1,193 (cv 2.2%) |
| 1048576 | 5,892 (cv 33.2%) | 1,139 (cv 2.5%) | 1,102 (cv 2.2%) |
