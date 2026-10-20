Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

| Measurement | Result |
|---|---|
| 16-lane tag comparison, vector (sse2) | 0.689 ns (cv 4.1%) |
| 16-lane tag comparison, scalar reference | 8.334 ns (cv 1.0%) |
| Speedup of the vector version | 12.1x (cv 3.8%) |
| Key hash, 8-byte key | 3.08 ns (cv 1.4%) |
| Key hash, 16-byte key | 4.21 ns (cv 1.8%) |
| Key hash, 32-byte key | 6.55 ns (cv 1.1%) |
| Key hash, 64-byte key | 11.43 ns (cv 2.3%) |
| Key hash, 256-byte key | 40.30 ns (cv 2.4%) |
| Key hash, 1024-byte key | 155.28 ns (cv 1.7%) |
| Key hash, 4096-byte key | 615.50 ns (cv 1.7%) |
