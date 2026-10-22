Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,733,462 (cv 1.1%) | 6,392,434 (cv 0.8%) | 10,982,242 (cv 1.5%) | 17,475,078 (cv 2.0%) | 21,333,074 (cv 1.6%) | 22,255,629 (cv 4.8%) |
| 4096-byte values | 1,503,683 (cv 1.7%) | 2,550,371 (cv 0.6%) | 3,919,707 (cv 0.9%) | 5,381,192 (cv 1.0%) | 5,727,927 (cv 1.4%) | 6,014,616 (cv 0.8%) |
