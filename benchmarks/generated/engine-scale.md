Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,755,944 (cv 1.4%) | 6,646,938 (cv 1.1%) | 11,480,767 (cv 1.6%) | 18,632,091 (cv 1.8%) | 22,429,186 (cv 1.4%) | 23,051,921 (cv 7.0%) |
| 4096-byte values | 1,516,694 (cv 0.5%) | 2,506,579 (cv 0.8%) | 3,883,060 (cv 0.7%) | 5,361,826 (cv 0.7%) | 5,721,626 (cv 0.9%) | 5,901,489 (cv 4.8%) |
