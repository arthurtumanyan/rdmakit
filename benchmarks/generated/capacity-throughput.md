Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, requests per second by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 2,342 (cv 0.8%) | 2,038 (cv 0.2%) | 1,994 (cv 0.4%) | 1,960 (cv 0.8%) | 1,952 (cv 0.4%) |
| Valkey | 1,981 (cv 0.4%) | 1,832 (cv 0.5%) | 1,752 (cv 0.9%) | 1,673 (cv 1.4%) | 1,625 (cv 0.9%) |
| Dragonfly | n/a | n/a | 172 (cv 1.8%) | 522 (cv 3.3%) | 1,586 (cv 1.4%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 1,648 (cv 0.8%) | 1,641 (cv 0.9%) | 1,592 (cv 1.5%) |
| memcached | 2,031 (cv 1.2%) | 1,901 (cv 1.0%) | 1,894 (cv 0.4%) | 1,888 (cv 0.3%) | 1,790 (cv 0.5%) |
