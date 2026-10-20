Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, reuse rate (fraction of needed blocks served from the cache) by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.193 (cv 1.4%) | 0.441 (cv 0.3%) | 0.660 (cv 0.2%) | 0.836 (cv 0.1%) | 0.902 (cv 0.0%) |
| Valkey | 0.037 (cv 1.6%) | 0.115 (cv 2.6%) | 0.307 (cv 0.4%) | 0.795 (cv 0.7%) | 0.902 (cv 0.0%) |
| Dragonfly | n/a | n/a | 0.407 (cv 2.1%) | 0.695 (cv 0.7%) | 0.902 (cv 0.0%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.438 (cv 0.4%) | 0.614 (cv 1.5%) | 0.901 (cv 0.1%) |
| memcached | 0.355 (cv 0.3%) | 0.502 (cv 0.5%) | 0.665 (cv 0.1%) | 0.847 (cv 0.0%) | 0.903 (cv 0.0%) |
