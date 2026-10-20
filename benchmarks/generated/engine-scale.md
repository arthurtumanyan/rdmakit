Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,728,244 (cv 7.2%) | 6,574,471 (cv 3.7%) | 11,454,697 (cv 1.4%) | 18,568,392 (cv 1.3%) | 22,585,490 (cv 3.9%) | 25,142,175 (cv 2.7%) |
| 4096-byte values | 1,526,878 (cv 4.2%) | 2,524,133 (cv 7.6%) | 3,982,740 (cv 1.9%) | 5,304,605 (cv 4.2%) | 5,715,945 (cv 3.7%) | 5,872,373 (cv 3.5%) |
