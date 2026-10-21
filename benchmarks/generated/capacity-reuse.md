Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, reuse rate (fraction of needed blocks served from the cache) by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.182 (cv 1.3%) | 0.429 (cv 0.3%) | 0.642 (cv 0.2%) | 0.822 (cv 0.2%) | 0.904 (cv 0.0%) |
| Valkey | 0.036 (cv 5.5%) | 0.115 (cv 3.0%) | 0.307 (cv 0.9%) | 0.789 (cv 0.2%) | 0.902 (cv 0.0%) |
| Dragonfly | n/a | n/a | 0.408 (cv 0.9%) | 0.685 (cv 0.5%) | 0.902 (cv 0.0%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.436 (cv 0.5%) | 0.599 (cv 0.6%) | 0.902 (cv 0.0%) |
| memcached | 0.354 (cv 0.3%) | 0.497 (cv 0.2%) | 0.663 (cv 0.2%) | 0.845 (cv 0.1%) | 0.903 (cv 0.0%) |
