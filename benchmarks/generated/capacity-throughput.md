Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, requests per second by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 2,334 (cv 0.9%) | 2,040 (cv 0.4%) | 1,987 (cv 0.6%) | 1,968 (cv 0.7%) | 1,952 (cv 0.4%) |
| Valkey | 1,970 (cv 0.3%) | 1,827 (cv 0.7%) | 1,751 (cv 0.4%) | 1,685 (cv 0.4%) | 1,626 (cv 0.3%) |
| Dragonfly | n/a | n/a | 173 (cv 1.5%) | 513 (cv 1.0%) | 1,688 (cv 0.4%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 1,653 (cv 0.3%) | 1,644 (cv 0.1%) | 1,686 (cv 0.2%) |
| memcached | 2,018 (cv 1.3%) | 1,905 (cv 0.4%) | 1,880 (cv 1.2%) | 1,883 (cv 0.5%) | 1,776 (cv 1.3%) |
