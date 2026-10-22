Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

| Measurement | Result |
|---|---|
| 16-lane tag comparison, vector (sse2) | 0.691 ns (cv 5.1%) |
| 16-lane tag comparison, scalar reference | 8.312 ns (cv 0.2%) |
| Speedup of the vector version | 12.1x (cv 5.1%) |
| Key hash, 8-byte key | 3.07 ns (cv 0.5%) |
| Key hash, 16-byte key | 4.20 ns (cv 1.8%) |
| Key hash, 32-byte key | 6.48 ns (cv 1.1%) |
| Key hash, 64-byte key | 11.17 ns (cv 1.2%) |
| Key hash, 256-byte key | 39.41 ns (cv 0.5%) |
| Key hash, 1024-byte key | 151.10 ns (cv 0.5%) |
| Key hash, 4096-byte key | 599.15 ns (cv 0.7%) |
