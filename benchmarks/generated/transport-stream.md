Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

One-way streaming throughput in MB/s (batched sends, 64 buffers per direction). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 285 (cv 26.5%) | 46 (cv 6.8%) | 47 (cv 2.6%) |
| 256 | 1,037 (cv 1.4%) | 188 (cv 2.7%) | 178 (cv 6.6%) |
| 1024 | 3,287 (cv 12.7%) | 730 (cv 2.2%) | 715 (cv 2.2%) |
| 4096 | 7,352 (cv 20.5%) | 915 (cv 0.5%) | 968 (cv 1.6%) |
| 16384 | 10,546 (cv 9.1%) | 1,155 (cv 1.3%) | 1,137 (cv 0.9%) |
| 65536 | 9,213 (cv 20.7%) | 1,279 (cv 3.0%) | 1,263 (cv 1.9%) |
| 262144 | 5,757 (cv 10.3%) | 1,170 (cv 4.4%) | 1,158 (cv 1.9%) |
| 1048576 | 6,329 (cv 43.2%) | 1,124 (cv 2.5%) | 1,087 (cv 2.4%) |
