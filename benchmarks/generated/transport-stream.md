Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

One-way streaming throughput in MB/s (batched sends, 64 buffers per direction). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 315 (cv 2.7%) | 46 (cv 5.7%) | 44 (cv 5.3%) |
| 256 | 1,161 (cv 4.0%) | 187 (cv 6.5%) | 167 (cv 10.1%) |
| 1024 | 3,137 (cv 11.1%) | 703 (cv 9.7%) | 679 (cv 5.6%) |
| 4096 | 7,913 (cv 5.9%) | 898 (cv 4.4%) | 935 (cv 2.0%) |
| 16384 | 10,631 (cv 12.8%) | 1,130 (cv 0.9%) | 1,139 (cv 1.1%) |
| 65536 | 10,538 (cv 8.1%) | 1,293 (cv 1.6%) | 1,284 (cv 0.5%) |
| 262144 | 6,007 (cv 4.6%) | 1,176 (cv 2.9%) | 1,176 (cv 1.2%) |
| 1048576 | 8,037 (cv 16.8%) | 1,118 (cv 1.6%) | 1,091 (cv 0.9%) |
