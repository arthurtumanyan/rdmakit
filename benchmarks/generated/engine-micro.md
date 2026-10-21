Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

| Measurement | Result |
|---|---|
| 16-lane tag comparison, vector (sse2) | 0.671 ns (cv 4.1%) |
| 16-lane tag comparison, scalar reference | 8.208 ns (cv 0.5%) |
| Speedup of the vector version | 12.3x (cv 4.4%) |
| Key hash, 8-byte key | 3.00 ns (cv 1.4%) |
| Key hash, 16-byte key | 4.13 ns (cv 0.8%) |
| Key hash, 32-byte key | 6.49 ns (cv 0.9%) |
| Key hash, 64-byte key | 11.15 ns (cv 0.8%) |
| Key hash, 256-byte key | 39.46 ns (cv 0.8%) |
| Key hash, 1024-byte key | 151.78 ns (cv 1.0%) |
| Key hash, 4096-byte key | 603.45 ns (cv 0.8%) |
