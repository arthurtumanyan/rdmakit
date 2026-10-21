Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,565,959 (cv 0.2%) | 6,043,200 (cv 1.1%) | 10,205,007 (cv 0.6%) | 16,650,776 (cv 0.8%) | 20,616,976 (cv 1.1%) | 22,854,775 (cv 2.2%) |
| 4096-byte values | 1,447,782 (cv 2.5%) | 2,441,348 (cv 1.4%) | 3,826,800 (cv 0.6%) | 5,343,564 (cv 0.4%) | 5,679,720 (cv 0.6%) | 5,898,117 (cv 1.7%) |
