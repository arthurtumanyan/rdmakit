Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, requests per second by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 1,794 (cv 2.0%) | 1,625 (cv 2.5%) | 1,627 (cv 0.0%) | 1,613 (cv 0.3%) | 1,583 (cv 0.4%) |
| Valkey | 1,936 (cv 0.5%) | 1,798 (cv 0.5%) | 1,721 (cv 1.2%) | 1,636 (cv 2.0%) | 1,587 (cv 0.6%) |
| Dragonfly | n/a | n/a | 171 (cv 3.5%) | 540 (cv 3.2%) | 1,654 (cv 0.3%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 1,552 (cv 6.2%) | 1,551 (cv 3.7%) | 1,568 (cv 6.5%) |
| memcached | 1,966 (cv 1.7%) | 1,809 (cv 4.1%) | 1,827 (cv 1.3%) | 1,835 (cv 0.7%) | 1,749 (cv 0.2%) |
