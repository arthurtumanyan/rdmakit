Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

| Measurement | Result |
|---|---|
| 16-lane tag comparison, vector (sse2) | 0.730 ns (cv 6.5%) |
| 16-lane tag comparison, scalar reference | 8.479 ns (cv 3.6%) |
| Speedup of the vector version | 11.6x (cv 4.6%) |
| Key hash, 8-byte key | 3.03 ns (cv 2.1%) |
| Key hash, 16-byte key | 4.14 ns (cv 2.2%) |
| Key hash, 32-byte key | 6.53 ns (cv 2.6%) |
| Key hash, 64-byte key | 11.22 ns (cv 1.3%) |
| Key hash, 256-byte key | 39.71 ns (cv 2.3%) |
| Key hash, 1024-byte key | 153.29 ns (cv 1.4%) |
| Key hash, 4096-byte key | 610.37 ns (cv 2.2%) |
