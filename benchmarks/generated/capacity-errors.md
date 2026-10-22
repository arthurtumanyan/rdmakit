Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Share of KV-cache requests that failed (an operation was rejected by the server, for example with an out-of-memory error), by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |
| Valkey | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |
| Dragonfly | n/a | n/a | 96.27% | 85.35% | 0.00% |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.00% | 0.00% | 0.00% |
| memcached | 0.01% | 0.01% | 0.01% | 0.00% | 0.00% |
