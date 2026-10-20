# Benchmarks

Measurements of `arenakv`, `kvpace` and `rdmaduct`, and of the established servers they are compared with, with
the raw data, the scripts that produced it, and checks that tie every published number and statement to that data.

| Item | Location |
|---|---|
| Raw results, environment and manifest of every run | [`data/`](data) |
| Figures (light and dark variants) | [`figures/`](figures) |
| Tables generated from the raw data | [`generated/`](generated) and the blocks below |
| Method, definitions, threats to validity | [`METHODOLOGY.md`](METHODOLOGY.md) |
| Orchestrator, report generator, verifier, claim definitions | [`tools/`](tools) |

The run described here is `data/<run>` where `<run>` is the name in [`LATEST`](LATEST).

## How to read this page

- Every figure has a table with the same data directly below it (the table view). Tables show the mean of the
  repetitions and, in parentheses, the coefficient of variation (`cv`).
- Figures show the mean with a whisker from the minimum to the maximum repetition.
- Statements such as "[C1]" are checked against the data by `tools/verify.py`; `tools/claims.py` contains the exact
  condition.
- All measurements come from **one machine** with **default configurations** and the **`powersave` CPU governor**.
  The server and the load generator share the machine and communicate over loopback. The RDMA rows use **Soft-RoCE**
  (software RDMA); they say nothing about RDMA hardware. Read [`METHODOLOGY.md`](METHODOLOGY.md#threats-to-validity)
  before drawing conclusions.

## Environment of the run

<!-- BEGIN GENERATED:environment -->
| Item | Value |
|---|---|
| Run | 2026-10-20-run1 |
| Date (UTC) | 2026-10-20T12:52:05Z |
| CPU | 12th Gen Intel(R) Core(TM) i9-12900K |
| Logical CPUs | 24 |
| CPU governor | powersave |
| Memory | 31.1 GiB |
| Kernel | 7.0.0-34-generic |
| OS | Ubuntu 24.04.5 LTS |
| Compiler | cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Network interface | enp4s0, 1000 Mbit/s |
| RDMA devices | link rxe0/1 state ACTIVE physical_state LINK_UP netdev enp4s0 |
| Valkey | Valkey server v=9.1.2 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=16129465b18d72a |
| Dragonfly | dragonfly v2.0.2-8ce4bbb106586057fbd85f71ed353c60d0d5bf87 |
| memcached | memcached 1.6.45 |
| Container runtime | Docker 29.9.0 |
| Source commit | fc0dc39b5653 |
| Uncommitted files at run start | 0 |
<!-- END GENERATED:environment -->

Provenance of the run directory: the suites `matrix`, `sweep`, `capacity`, `engine` and `transport` ran first, from the
source commit above with a clean working tree. The suites `validation` and `capacity-evict` (Dragonfly with a raised
eviction flag, added after the failures in `capacity` were observed) were added to the same directory afterwards with
the same binaries but a later version of `tools/run.py` (see the git history of that file). The server arguments are
not part of the per-run records of this directory (they are set in `tools/run.py`); later runs record them.

## 1. Throughput of the servers

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/throughput-dark.svg">
  <img alt="Grouped bar charts of throughput for five systems in six panels: one and four server CPUs by 64-byte values, 16 KiB values and KV-cache requests" src="figures/throughput.svg">
</picture>

<!-- BEGIN GENERATED:matrix-throughput -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Operations per second (KV-cache rows: requests per second).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|
| 64-byte values | 1 | 169,230 (cv 1.3%) | 190,098 (cv 2.6%) | 202,099 (cv 1.2%) | 201,584 (cv 1.1%) | 221,236 (cv 0.7%) |
| 64-byte values | 4 | 601,090 (cv 1.0%) | 239,042 (cv 2.3%) | 521,525 (cv 3.6%) | 556,645 (cv 2.2%) | 749,478 (cv 3.9%) |
| 16 KiB values | 1 | 127,136 (cv 0.6%) | 99,118 (cv 1.8%) | 135,476 (cv 1.3%) | 130,974 (cv 3.2%) | 139,778 (cv 1.9%) |
| 16 KiB values | 4 | 453,342 (cv 2.6%) | 147,657 (cv 1.4%) | 352,473 (cv 5.0%) | 404,556 (cv 2.2%) | 506,687 (cv 5.2%) |
| KV-cache requests | 1 | 1,616 (cv 1.6%) | 1,328 (cv 2.5%) | 1,669 (cv 4.2%) | 1,711 (cv 3.6%) | 1,748 (cv 4.5%) |
| KV-cache requests | 4 | 4,526 (cv 2.0%) | 1,944 (cv 1.5%) | 4,114 (cv 1.4%) | 2,352 (cv 11.1%) | 4,376 (cv 4.3%) |
<!-- END GENERATED:matrix-throughput -->

## 2. Server CPU per operation

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/cpu-per-op-dark.svg">
  <img alt="Grouped bar charts of server CPU time per operation for five systems in six panels" src="figures/cpu-per-op.svg">
</picture>

<!-- BEGIN GENERATED:matrix-cpu -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Server CPU time per operation in microseconds (KV-cache rows: per block GET or PUT).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|
| 64-byte values | 1 | 5.91 (cv 1.3%) | 5.26 (cv 2.7%) | 4.95 (cv 1.1%) | 4.96 (cv 1.1%) | 4.52 (cv 0.7%) |
| 64-byte values | 4 | 6.59 (cv 0.9%) | 11.42 (cv 1.6%) | 7.68 (cv 3.7%) | 7.02 (cv 2.0%) | 5.29 (cv 3.3%) |
| 16 KiB values | 1 | 7.86 (cv 0.6%) | 10.08 (cv 1.7%) | 7.38 (cv 1.3%) | 7.64 (cv 3.2%) | 7.15 (cv 1.8%) |
| 16 KiB values | 4 | 8.81 (cv 2.7%) | 12.33 (cv 0.9%) | 11.36 (cv 4.9%) | 9.56 (cv 2.6%) | 7.77 (cv 4.8%) |
| KV-cache requests | 1 | 8.19 (cv 1.8%) | 10.18 (cv 2.7%) | 7.93 (cv 4.9%) | 7.70 (cv 4.1%) | 7.53 (cv 5.1%) |
| KV-cache requests | 4 | 9.26 (cv 2.8%) | 12.72 (cv 1.2%) | 11.64 (cv 1.4%) | 12.95 (cv 6.4%) | 8.29 (cv 3.1%) |
<!-- END GENERATED:matrix-cpu -->

Latency in the closed loop (queueing at 16 connections, not latency at a given load):

<!-- BEGIN GENERATED:matrix-p99 -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

GET latency p99 in microseconds in the closed loop (KV-cache rows: request latency).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|
| 64-byte values | 1 | 171 (cv 1.9%) | 151 (cv 10.7%) | 145 (cv 1.4%) | 103 (cv 1.4%) | 151 (cv 1.6%) |
| 64-byte values | 4 | 55 (cv 2.3%) | 421 (cv 7.5%) | 67 (cv 30.3%) | 60 (cv 17.5%) | 45 (cv 14.7%) |
| 16 KiB values | 1 | 233 (cv 1.4%) | 401 (cv 8.9%) | 181 (cv 6.7%) | 177 (cv 19.2%) | 240 (cv 2.4%) |
| 16 KiB values | 4 | 74 (cv 10.8%) | 700 (cv 3.9%) | 107 (cv 24.5%) | 93 (cv 11.4%) | 103 (cv 18.1%) |
| KV-cache requests | 1 | 21,365 (cv 2.6%) | 30,618 (cv 6.9%) | 20,709 (cv 5.6%) | 20,316 (cv 5.8%) | 25,087 (cv 3.6%) |
| KV-cache requests | 4 | 7,491 (cv 5.7%) | 23,672 (cv 5.8%) | 9,640 (cv 14.7%) | 40,685 (cv 12.5%) | 9,935 (cv 21.0%) |
<!-- END GENERATED:matrix-p99 -->

Server resident memory at the end of each run:

<!-- BEGIN GENERATED:matrix-rss -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Server resident memory at the end of the run in MiB.

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|
| 64-byte values | 1 | 338 (cv 0.1%) | 372 (cv 0.1%) | 23 (cv 0.2%) | 32 (cv 2.1%) | 21 (cv 0.3%) |
| 64-byte values | 4 | 338 (cv 0.1%) | 372 (cv 0.2%) | 24 (cv 0.3%) | 35 (cv 0.1%) | 22 (cv 0.2%) |
| 16 KiB values | 1 | 325 (cv 0.0%) | 358 (cv 0.0%) | 407 (cv 0.0%) | 337 (cv 0.2%) | 341 (cv 0.0%) |
| 16 KiB values | 4 | 325 (cv 0.0%) | 358 (cv 0.0%) | 450 (cv 1.1%) | 346 (cv 0.2%) | 342 (cv 0.0%) |
| KV-cache requests | 1 | 2,382 (cv 1.6%) | 2,015 (cv 1.7%) | 3,071 (cv 4.1%) | 2,559 (cv 3.5%) | 2,734 (cv 4.0%) |
| KV-cache requests | 4 | 6,699 (cv 1.2%) | 2,941 (cv 1.2%) | 7,421 (cv 1.9%) | 4,481 (cv 7.3%) | 6,919 (cv 3.2%) |
<!-- END GENERATED:matrix-rss -->

## 3. Latency under offered load (open loop)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/sweep-dark.svg">
  <img alt="Two line charts: achieved operations per second versus offered load, and p99 latency versus offered load, for four servers" src="figures/sweep.svg">
</picture>

<!-- BEGIN GENERATED:sweep -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Open loop, one server CPU, 64-byte values, 16 connections. Latencies in microseconds, measured from the intended send time unless stated.

| System | Offered ops/s | Achieved ops/s | p50 | p99 | p99 service time | Max schedule lag (ms) |
|---|---|---|---|---|---|---|
| arenakv (TCP) | 20,000 | 20,000 (cv 0.0%) | 39 (cv 29.6%) | 627 (cv 37.8%) | 320 (cv 12.4%) | 5.0 (cv 26.7%) |
| arenakv (TCP) | 60,000 | 60,000 (cv 0.0%) | 141 (cv 10.5%) | 759 (cv 19.5%) | 593 (cv 5.4%) | 6.9 (cv 50.2%) |
| arenakv (TCP) | 100,000 | 100,002 (cv 0.0%) | 295 (cv 6.3%) | 15,199 (cv 133.0%) | 928 (cv 5.6%) | 47.4 (cv 133.8%) |
| arenakv (TCP) | 140,000 | 140,001 (cv 0.0%) | 176 (cv 1.0%) | 40,654 (cv 89.9%) | 519 (cv 0.5%) | 90.7 (cv 46.5%) |
| arenakv (TCP) | 180,000 | 172,831 (cv 0.1%) | 170,568 (cv 15.6%) | 403,352 (cv 31.7%) | 167 (cv 2.8%) | 422.2 (cv 29.9%) |
| arenakv (TCP) | 220,000 | 173,262 (cv 0.5%) | 1,052,770 (cv 0.4%) | 1,756,015 (cv 2.6%) | 168 (cv 1.9%) | 1,814.1 (cv 2.4%) |
| arenakv (TCP) | 260,000 | 173,322 (cv 0.2%) | 1,663,741 (cv 1.0%) | 2,661,985 (cv 0.4%) | 170 (cv 1.3%) | 2,713.4 (cv 1.2%) |
| Valkey | 20,000 | 20,000 (cv 0.0%) | 35 (cv 4.5%) | 479 (cv 38.8%) | 271 (cv 42.7%) | 3.8 (cv 12.3%) |
| Valkey | 60,000 | 60,000 (cv 0.0%) | 45 (cv 37.9%) | 1,353 (cv 47.4%) | 564 (cv 6.7%) | 32.9 (cv 21.8%) |
| Valkey | 100,000 | 99,998 (cv 0.0%) | 324 (cv 10.5%) | 5,822 (cv 17.0%) | 918 (cv 3.1%) | 35.9 (cv 72.5%) |
| Valkey | 140,000 | 140,002 (cv 0.0%) | 185 (cv 8.3%) | 3,173 (cv 14.9%) | 553 (cv 3.9%) | 34.7 (cv 109.9%) |
| Valkey | 180,000 | 180,002 (cv 0.0%) | 93 (cv 2.4%) | 16,409 (cv 165.2%) | 282 (cv 1.5%) | 39.5 (cv 139.3%) |
| Valkey | 220,000 | 204,794 (cv 0.6%) | 353,021 (cv 7.0%) | 580,212 (cv 6.1%) | 143 (cv 0.7%) | 599.6 (cv 6.3%) |
| Valkey | 260,000 | 205,400 (cv 0.6%) | 1,065,353 (cv 4.8%) | 1,691,703 (cv 3.6%) | 143 (cv 0.7%) | 1,730.9 (cv 3.4%) |
| Dragonfly | 20,000 | 20,000 (cv 0.0%) | 31 (cv 2.4%) | 489 (cv 35.4%) | 274 (cv 31.1%) | 5.1 (cv 56.4%) |
| Dragonfly | 60,000 | 60,000 (cv 0.0%) | 58 (cv 34.6%) | 963 (cv 27.8%) | 493 (cv 5.1%) | 13.9 (cv 74.2%) |
| Dragonfly | 100,000 | 100,010 (cv 0.0%) | 490 (cv 10.7%) | 6,423 (cv 2.7%) | 890 (cv 1.2%) | 14.0 (cv 8.7%) |
| Dragonfly | 140,000 | 139,999 (cv 0.0%) | 269 (cv 6.6%) | 5,844 (cv 49.4%) | 539 (cv 3.5%) | 41.9 (cv 38.0%) |
| Dragonfly | 180,000 | 180,009 (cv 0.0%) | 137 (cv 2.4%) | 2,529 (cv 10.4%) | 170 (cv 15.3%) | 10.4 (cv 46.3%) |
| Dragonfly | 220,000 | 203,198 (cv 0.5%) | 396,362 (cv 6.5%) | 673,885 (cv 6.6%) | 108 (cv 10.2%) | 686.5 (cv 6.5%) |
| Dragonfly | 260,000 | 203,266 (cv 1.2%) | 1,086,325 (cv 1.8%) | 1,744,830 (cv 3.2%) | 103 (cv 2.2%) | 1,779.9 (cv 2.4%) |
| memcached | 20,000 | 20,000 (cv 0.0%) | 41 (cv 6.0%) | 365 (cv 19.1%) | 270 (cv 26.5%) | 3.5 (cv 4.0%) |
| memcached | 60,000 | 59,999 (cv 0.0%) | 98 (cv 41.7%) | 677 (cv 7.9%) | 554 (cv 9.2%) | 5.4 (cv 21.8%) |
| memcached | 100,000 | 99,999 (cv 0.0%) | 371 (cv 30.7%) | 6,575 (cv 23.2%) | 1,013 (cv 2.4%) | 44.4 (cv 14.1%) |
| memcached | 140,000 | 140,064 (cv 0.1%) | 248 (cv 5.2%) | 3,539 (cv 3.5%) | 687 (cv 1.2%) | 33.5 (cv 57.6%) |
| memcached | 180,000 | 179,846 (cv 0.2%) | 162 (cv 1.1%) | 2,387 (cv 6.7%) | 442 (cv 1.7%) | 34.8 (cv 113.9%) |
| memcached | 220,000 | 219,986 (cv 0.0%) | 104 (cv 4.5%) | 15,707 (cv 96.4%) | 181 (cv 2.8%) | 33.0 (cv 32.5%) |
| memcached | 260,000 | 226,520 (cv 0.5%) | 641,729 (cv 3.4%) | 1,291,846 (cv 5.2%) | 143 (cv 1.1%) | 1,446.5 (cv 11.4%) |
<!-- END GENERATED:sweep -->

Why latency is measured from the intended send time:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/coordinated-omission-dark.svg">
  <img alt="Two line charts comparing p99 latency from the intended send time with the pure service time as offered load grows" src="figures/coordinated-omission.svg">
</picture>

## 4. KV-cache workload: reuse versus capacity

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/capacity-dark.svg">
  <img alt="Two line charts: reuse rate and requests per second versus configured cache capacity for four servers" src="figures/capacity.svg">
</picture>

<!-- BEGIN GENERATED:capacity-reuse -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, reuse rate (fraction of needed blocks served from the cache) by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.193 (cv 1.4%) | 0.441 (cv 0.3%) | 0.660 (cv 0.2%) | 0.836 (cv 0.1%) | 0.902 (cv 0.0%) |
| Valkey | 0.037 (cv 1.6%) | 0.115 (cv 2.6%) | 0.307 (cv 0.4%) | 0.795 (cv 0.7%) | 0.902 (cv 0.0%) |
| Dragonfly | n/a | n/a | 0.407 (cv 2.1%) | 0.695 (cv 0.7%) | 0.902 (cv 0.0%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.438 (cv 0.4%) | 0.614 (cv 1.5%) | 0.901 (cv 0.1%) |
| memcached | 0.355 (cv 0.3%) | 0.502 (cv 0.5%) | 0.665 (cv 0.1%) | 0.847 (cv 0.0%) | 0.903 (cv 0.0%) |
<!-- END GENERATED:capacity-reuse -->

<!-- BEGIN GENERATED:capacity-throughput -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, requests per second by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 1,794 (cv 2.0%) | 1,625 (cv 2.5%) | 1,627 (cv 0.0%) | 1,613 (cv 0.3%) | 1,583 (cv 0.4%) |
| Valkey | 1,936 (cv 0.5%) | 1,798 (cv 0.5%) | 1,721 (cv 1.2%) | 1,636 (cv 2.0%) | 1,587 (cv 0.6%) |
| Dragonfly | n/a | n/a | 171 (cv 3.5%) | 540 (cv 3.2%) | 1,654 (cv 0.3%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 1,552 (cv 6.2%) | 1,551 (cv 3.7%) | 1,568 (cv 6.5%) |
| memcached | 1,966 (cv 1.7%) | 1,809 (cv 4.1%) | 1,827 (cv 1.3%) | 1,835 (cv 0.7%) | 1,749 (cv 0.2%) |
<!-- END GENERATED:capacity-throughput -->

Share of requests that failed because the server rejected an operation:

<!-- BEGIN GENERATED:capacity-errors -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Share of KV-cache requests that failed (an operation was rejected by the server, for example with an out-of-memory error), by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |
| Valkey | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |
| Dragonfly | n/a | n/a | 96.19% | 84.24% | 0.00% |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.00% | 0.00% | 0.00% |
| memcached | 0.03% | 0.00% | 0.00% | 0.00% | 0.00% |
<!-- END GENERATED:capacity-errors -->

<!-- BEGIN GENERATED:capacity-skipped -->
Skipped configurations: dragonfly at 16mb (dragonfly refuses to start with less than 256 MiB per thread); dragonfly at 64mb (dragonfly refuses to start with less than 256 MiB per thread).
<!-- END GENERATED:capacity-skipped -->

## 5. arenakv engine

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/engine-dark.svg">
  <img alt="Engine thread scaling, tag comparison cost of the vector and scalar versions, and key hash throughput" src="figures/engine.svg">
</picture>

<!-- BEGIN GENERATED:engine-scale -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,728,244 (cv 7.2%) | 6,574,471 (cv 3.7%) | 11,454,697 (cv 1.4%) | 18,568,392 (cv 1.3%) | 22,585,490 (cv 3.9%) | 25,142,175 (cv 2.7%) |
| 4096-byte values | 1,526,878 (cv 4.2%) | 2,524,133 (cv 7.6%) | 3,982,740 (cv 1.9%) | 5,304,605 (cv 4.2%) | 5,715,945 (cv 3.7%) | 5,872,373 (cv 3.5%) |
<!-- END GENERATED:engine-scale -->

<!-- BEGIN GENERATED:engine-micro -->
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
<!-- END GENERATED:engine-micro -->

## 6. rdmaduct transport (loopback on one host)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/transport-dark.svg">
  <img alt="Two line charts: median round-trip time and streaming throughput versus message size for TCP and Soft-RoCE in polling and event-driven mode" src="figures/transport.svg">
</picture>

<!-- BEGIN GENERATED:transport-rtt -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Median round-trip time in microseconds (ping-pong, one message in flight). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 10.2 (cv 30.7%) | 6.7 (cv 1.0%) | 10.0 (cv 3.3%) |
| 256 | 8.5 (cv 5.6%) | 6.7 (cv 2.1%) | 10.2 (cv 1.0%) |
| 1024 | 8.5 (cv 1.5%) | 7.2 (cv 5.5%) | 10.5 (cv 3.5%) |
| 4096 | 10.3 (cv 15.2%) | 11.3 (cv 2.7%) | 15.1 (cv 3.4%) |
| 16384 | 12.2 (cv 2.8%) | 30.4 (cv 1.5%) | 35.5 (cv 3.5%) |
| 65536 | 23.8 (cv 4.4%) | 109.9 (cv 2.2%) | 113.6 (cv 3.8%) |
| 262144 | 59.0 (cv 1.8%) | 446.2 (cv 1.6%) | 456.8 (cv 3.3%) |
| 1048576 | 250.1 (cv 2.6%) | 1,901.0 (cv 1.2%) | 1,941.7 (cv 1.3%) |
<!-- END GENERATED:transport-rtt -->

<!-- BEGIN GENERATED:transport-rtt-p99 -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

p99 round-trip time in microseconds. Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 45.7 (cv 55.8%) | 9.4 (cv 11.3%) | 13.9 (cv 11.9%) |
| 256 | 14.3 (cv 37.4%) | 9.1 (cv 21.3%) | 14.1 (cv 7.6%) |
| 1024 | 15.5 (cv 21.0%) | 9.1 (cv 12.8%) | 14.8 (cv 38.9%) |
| 4096 | 18.1 (cv 32.0%) | 14.5 (cv 9.1%) | 20.5 (cv 20.6%) |
| 16384 | 19.8 (cv 28.7%) | 38.9 (cv 13.1%) | 45.5 (cv 13.8%) |
| 65536 | 49.7 (cv 73.9%) | 153.2 (cv 16.5%) | 161.1 (cv 13.3%) |
| 262144 | 76.6 (cv 34.2%) | 566.4 (cv 11.1%) | 668.3 (cv 13.0%) |
| 1048576 | 356.1 (cv 45.5%) | 2,377.1 (cv 15.5%) | 2,827.6 (cv 2.2%) |
<!-- END GENERATED:transport-rtt-p99 -->

<!-- BEGIN GENERATED:transport-stream -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

One-way streaming throughput in MB/s (batched sends, 64 buffers per direction). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 28 (cv 12.9%) | 46 (cv 6.9%) | 46 (cv 10.3%) |
| 256 | 103 (cv 0.8%) | 189 (cv 2.8%) | 184 (cv 7.5%) |
| 1024 | 329 (cv 3.5%) | 722 (cv 5.8%) | 688 (cv 8.5%) |
| 4096 | 1,228 (cv 3.9%) | 910 (cv 3.4%) | 962 (cv 1.6%) |
| 16384 | 3,667 (cv 5.0%) | 1,135 (cv 3.4%) | 1,145 (cv 0.8%) |
| 65536 | 6,963 (cv 5.3%) | 1,280 (cv 1.5%) | 1,239 (cv 2.7%) |
| 262144 | 8,726 (cv 3.3%) | 1,174 (cv 3.3%) | 1,121 (cv 6.5%) |
| 1048576 | 7,990 (cv 2.7%) | 1,106 (cv 3.1%) | 1,086 (cv 2.5%) |
<!-- END GENERATED:transport-stream -->

## 7. Validation of the load generator

`kvpace` against `memtier_benchmark` on the same server and settings (see
[`../kvpace/docs/validation.md`](../kvpace/docs/validation.md)):

<!-- BEGIN GENERATED:validation -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Valkey on one CPU, 64-byte values, 90% GET. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Connections | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|
| 1 | 109,333 (cv 3.5%) | 85,651 (cv 1.7%) | 1.28 | 8.8 (cv 2.2%) | 15.0 (cv 0.0%) | 13 (cv 7.5%) | 20 (cv 22.1%) |
| 16 | 199,259 (cv 1.9%) | 220,849 (cv 1.1%) | 0.90 | 76.5 (cv 1.4%) | 71.0 (cv 0.0%) | 148 (cv 1.9%) | 111 (cv 10.2%) |
<!-- END GENERATED:validation -->

## Findings

Each statement below is checked against the data (tag `[C<n>]`, definition in [`tools/claims.py`](tools/claims.py)).
Differences are stated only for means or, where noted, for non-overlapping repetition ranges.

**Closed-loop throughput and server CPU (loopback, TCP)**

- memcached has the highest mean throughput in five of the six rows of the matrix; arenakv (TCP) has the highest in the sixth
  (KV-cache requests, four server CPUs) [C1].
- At four server CPUs the throughput of arenakv (TCP) is higher than that of Valkey and of Dragonfly in all three
  workloads, with non-overlapping ranges [C2]. At one server CPU it is lower than all three on 64-byte values,
  with non-overlapping ranges [C3]. The four-CPU rows are probably limited by the load generator (see
  [`METHODOLOGY.md`](METHODOLOGY.md#threats-to-validity)), so they show the order of the systems under this client,
  not server capacity.
- memcached has the lowest mean server CPU time per operation of the four TCP servers in all six rows [C4]. At four
  server CPUs arenakv (TCP) uses less server CPU time per operation than Valkey and Dragonfly in all three workloads [C5].
- arenakv over Soft-RoCE has lower mean throughput than arenakv over TCP in five of the six rows; the exception is
  64-byte values with one server CPU [C6]. Soft-RoCE is a software implementation sharing the CPUs with the load
  generator, so these rows say nothing about RDMA hardware.
- No operation failed in any matrix run [C22].
- With 64-byte values the resident memory of arenakv is more than eight times that of each of Valkey, Dragonfly and
  memcached, each of which stays below 40 MiB [C16]. The cause is the design, not the data: arenakv sizes its hash
  index from the configured capacity and the expected entry size, and this run used an 8 GiB capacity with a 128-byte
  expected entry size. This is a known weakness of the current design.

**Latency under offered load (open loop, one server CPU, 64-byte values)**

- Up to 140,000 offered operations per second all four TCP servers achieve the offered rate within 1% [C8]. At 260,000
  offered operations per second arenakv (TCP) achieves a lower rate than Valkey, Dragonfly and memcached, with
  non-overlapping ranges [C7].
- At 260,000 offered operations per second the p99 latency measured from the intended send time exceeds the p99
  service time by more than a factor of 1,000 for every TCP server [C9]. A closed-loop or service-time-only
  measurement would hide this backlog; it is the reason latency is measured from the intended send time.
- Below saturation the p99 latency has a coefficient of variation above 20% between repetitions in at least half of the
  16 cells [C10]. Tail latencies at moderate load are noisy on this machine and must not be used to rank servers; the
  non-monotonic p99 curves between 100,000 and 180,000 operations per second are likely effects of client thread
  scheduling and the `powersave` governor (a hypothesis that was not tested).

**KV-cache workload, by configured capacity (one server CPU, 8 connections)**

- At 16, 64 and 256 MB the reuse rate of arenakv (TCP) is higher than that of Valkey, with non-overlapping ranges [C11].
  memcached has a higher mean reuse rate than arenakv (TCP) at 16 and 64 MB and differs from it by less than 0.02 at
  256 MB, 1 GB and 4 GB [C12]. At 4 GB, where the working set fits, the mean reuse rates of all systems that ran
  differ by less than 0.005 [C13]. "Configured capacity" is accounted differently by each system (see the methodology),
  so this compares systems at equal configuration, not at equal total memory.
- With default flags Dragonfly in cache mode failed more than 90% of requests at 256 MB and more than 80% at 1 GB with
  out-of-memory errors; with `--max_eviction_per_heartbeat` raised no request failed [C14]. Both variants are shown;
  the raised flag was chosen after the failures were observed (see the methodology). memcached failed fewer than 0.1%
  of requests in every capacity configuration; arenakv and Valkey failed none [C15].

**Engine and transport**

- Engine throughput with 64-byte values grows by a factor between 4 and 6 from one to eight threads, with 4 KiB values
  by a factor below 4, and 4 KiB throughput grows by less than 15% from 8 to 16 threads (the added threads are
  hyper-thread siblings) [C17]. The vector tag comparison is at least 10 times faster than the scalar reference [C18]; the
  reference is compiled at `-O2` without auto-vectorization.
- Loopback transport: for messages up to 1 KiB the median round trip of Soft-RoCE with polling is lower than that of
  TCP; for messages of 16 KiB and larger TCP is lower than both Soft-RoCE modes [C19]. TCP streaming throughput is at
  least five times that of Soft-RoCE for messages of 64 KiB and larger, and lower than both Soft-RoCE modes for
  messages up to 1 KiB [C20]. These are software-RDMA results.

**Load generator**

- With one connection kvpace measures at least 20% higher throughput than `memtier_benchmark`; with 16 connections it
  measures between 5% and 15% lower [C21]. The tools differ in client design (see the validation document), so
  comparisons between servers must use one tool for all of them.

## What these results do not show

- Performance on RDMA hardware. The RDMA rows use Soft-RoCE, which runs the protocol in the kernel on the same CPUs.
- Performance across a physical network, with other CPUs, kernels or memory systems.
- Tuned configurations of any system, or behavior under workloads other than the three defined in the methodology.
- Differences smaller than the spread between repetitions (shown as `cv` and whiskers).

## Reproducing and checking

```
make -C .. && make -C .. verify          # build and check the published run against its raw data
tools/run.py --suite all --reps 5 --out data/<new-run>     # a new run (needs Docker, RDMA_ADDR, BENCH_NIC)
tools/report.py data/<new-run> --update-docs               # regenerate figures, tables and doc blocks
tools/verify.py data/<new-run>                             # verify
```

Details and how to challenge a result: [`METHODOLOGY.md`](METHODOLOGY.md#challenging-a-result).
