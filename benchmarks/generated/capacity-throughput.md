Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, requests per second by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 2,020 (cv 0.7%) | 1,787 (cv 0.8%) | 1,755 (cv 1.1%) | 1,760 (cv 0.1%) | 1,737 (cv 0.6%) |
| Valkey | 1,930 (cv 0.3%) | 1,805 (cv 0.4%) | 1,734 (cv 0.3%) | 1,660 (cv 0.8%) | 1,609 (cv 0.2%) |
| Dragonfly | n/a | n/a | 174 (cv 1.3%) | 529 (cv 1.6%) | 1,598 (cv 0.4%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 1,630 (cv 0.2%) | 1,617 (cv 1.0%) | 1,517 (cv 2.2%) |
| memcached | 1,998 (cv 0.5%) | 1,864 (cv 1.1%) | 1,847 (cv 1.5%) | 1,837 (cv 1.4%) | 1,753 (cv 0.2%) |
