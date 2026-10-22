Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, reuse rate (fraction of needed blocks served from the cache) by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.183 (cv 2.7%) | 0.430 (cv 0.2%) | 0.643 (cv 0.2%) | 0.822 (cv 0.1%) | 0.904 (cv 0.0%) |
| Valkey | 0.036 (cv 4.0%) | 0.115 (cv 1.0%) | 0.304 (cv 0.8%) | 0.789 (cv 0.5%) | 0.902 (cv 0.0%) |
| Dragonfly | n/a | n/a | 0.407 (cv 1.3%) | 0.689 (cv 0.7%) | 0.902 (cv 0.0%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.435 (cv 0.4%) | 0.600 (cv 0.7%) | 0.902 (cv 0.0%) |
| memcached | 0.354 (cv 0.3%) | 0.497 (cv 0.2%) | 0.664 (cv 0.2%) | 0.845 (cv 0.1%) | 0.903 (cv 0.0%) |
