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
- All measurements come from **one machine** with **default configurations** and the **`performance` CPU governor**
  (run3 to run5; run1 and run2 used `powersave`).
  The server and the load generator share the machine and communicate over loopback. The RDMA rows use **Soft-RoCE**
  (software RDMA); they say nothing about RDMA hardware. Read [`METHODOLOGY.md`](METHODOLOGY.md#threats-to-validity)
  before drawing conclusions.

## Environment of the run

<!-- BEGIN GENERATED:environment -->
| Item | Value |
|---|---|
| Run | 2026-10-23-run5 |
| Date (UTC) | 2026-10-22T20:32:03Z |
| CPU | 12th Gen Intel(R) Core(TM) i9-12900K |
| Logical CPUs | 24 |
| CPU governor | performance |
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
| Source commit | 9054e10aa2ab |
| Uncommitted files at run start | 0 |
| Containers running at run start | 0 |
| Load average (1 min) at run start | 0.81 |
| OpenSSL | OpenSSL 3.0.13 30 Jan 2024 (Library: OpenSSL 3.0.13 30 Jan 2024) |
| Filesystem of the append-only logs | ext4 |
| fsync() after a 128-byte append, median | 6.08 ms |
<!-- END GENERATED:environment -->

Provenance of the run directory: every suite ran from the source commit above with a clean working tree, in the order
matrix, sweep, pipeline, connections, capacity, engine, transport, validation, then `capacity-evict` (Dragonfly with a
raised eviction flag) into the same directory with the same binaries. The server arguments are those of
`tools/run.py` and `kvpace/scripts/servers.sh` at that commit and are recorded in the manifest. The system ids
`kvstore`, `kvstore-rdma` and `kvstore-mc` in the raw data name arenakv over TCP, over Soft-RoCE and serving the
memcached text protocol. The CPU governor in `env.json` is that of CPU 0; the run started only after all 24 CPUs reported `performance`.
Later runs record the distinct governors of all CPUs. Containers unrelated to the benchmark were
stopped for the duration of the run; this run's `env.json` does not record it, later runs record the number of
running containers and the load average at the start.

Earlier runs stay in the repository unchanged:
`data/2026-10-21-run2` measured the server and transport before the TCP path changes described in
[`rdmaduct/docs/ARCHITECTURE.md`](../rdmaduct/docs/ARCHITECTURE.md), with the `powersave` governor, and did not
record the kernel-mode part of the server CPU time; `data/2026-10-20-run1` measured the earlier server version (one
thread per connection, index sized from the capacity), and its server CPU per KV-cache operation is computed from its
raw counters (see `tools/bench_data.py`).

## 1. Throughput of the servers

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/throughput-dark.svg">
  <img alt="Grouped bar charts of throughput for six systems in six panels: one and four server CPUs by 64-byte values, 16 KiB values and KV-cache requests" src="figures/throughput.svg">
</picture>

<!-- BEGIN GENERATED:matrix-throughput -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Operations per second (KV-cache rows: requests per second).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 267,326 (cv 1.3%) | 226,745 (cv 0.5%) | 261,541 (cv 1.1%) | 210,303 (cv 0.5%) | 209,497 (cv 0.6%) | 231,548 (cv 0.7%) |
| 64-byte values | 4 | 725,006 (cv 3.5%) | 246,569 (cv 3.2%) | 782,749 (cv 1.2%) | 475,480 (cv 2.7%) | 529,027 (cv 5.1%) | 740,397 (cv 3.3%) |
| 16 KiB values | 1 | 160,180 (cv 2.3%) | 118,380 (cv 2.3%) | 159,022 (cv 0.4%) | 142,610 (cv 0.2%) | 136,526 (cv 0.5%) | 147,010 (cv 0.4%) |
| 16 KiB values | 4 | 509,698 (cv 2.2%) | 142,196 (cv 6.8%) | 510,336 (cv 1.7%) | 336,293 (cv 2.5%) | 385,691 (cv 1.5%) | 491,118 (cv 1.5%) |
| KV-cache requests | 1 | 2,052 (cv 0.3%) | 1,449 (cv 2.4%) | 1,991 (cv 0.4%) | 1,749 (cv 2.3%) | 1,784 (cv 1.6%) | 1,881 (cv 0.3%) |
| KV-cache requests | 4 | 4,733 (cv 1.6%) | 1,764 (cv 3.6%) | 5,002 (cv 1.8%) | 3,940 (cv 1.6%) | 2,791 (cv 3.7%) | 4,266 (cv 3.7%) |
<!-- END GENERATED:matrix-throughput -->

## 2. Server CPU per operation

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/cpu-per-op-dark.svg">
  <img alt="Grouped bar charts of server CPU time per operation for five systems in six panels" src="figures/cpu-per-op.svg">
</picture>

<!-- BEGIN GENERATED:matrix-cpu -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Server CPU time per operation in microseconds (KV-cache rows: per block GET or PUT).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 3.74 (cv 1.3%) | 4.41 (cv 0.5%) | 3.82 (cv 1.1%) | 4.75 (cv 0.4%) | 4.78 (cv 0.6%) | 4.32 (cv 0.6%) |
| 64-byte values | 4 | 5.09 (cv 3.7%) | 9.19 (cv 3.2%) | 5.07 (cv 1.7%) | 8.42 (cv 2.7%) | 7.42 (cv 4.9%) | 5.36 (cv 3.1%) |
| 16 KiB values | 1 | 6.24 (cv 2.3%) | 8.44 (cv 2.4%) | 6.29 (cv 0.4%) | 7.01 (cv 0.3%) | 7.32 (cv 0.5%) | 6.80 (cv 0.3%) |
| 16 KiB values | 4 | 7.76 (cv 2.2%) | 10.92 (cv 5.5%) | 7.77 (cv 2.3%) | 11.89 (cv 2.6%) | 10.11 (cv 1.6%) | 8.09 (cv 1.6%) |
| KV-cache requests | 1 | 6.28 (cv 0.3%) | 9.20 (cv 2.5%) | 6.49 (cv 0.5%) | 7.51 (cv 2.4%) | 7.35 (cv 1.7%) | 6.91 (cv 0.3%) |
| KV-cache requests | 4 | 8.44 (cv 1.5%) | 11.96 (cv 2.9%) | 8.30 (cv 2.3%) | 12.16 (cv 1.7%) | 11.68 (cv 2.9%) | 8.55 (cv 2.2%) |
<!-- END GENERATED:matrix-cpu -->

Latency in the closed loop (queueing at 16 connections, not latency at a given load):

<!-- BEGIN GENERATED:matrix-p99 -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

GET latency p99 in microseconds in the closed loop (KV-cache rows: request latency).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 116 (cv 1.0%) | 125 (cv 0.6%) | 120 (cv 1.1%) | 140 (cv 0.6%) | 93 (cv 5.1%) | 139 (cv 1.9%) |
| 64-byte values | 4 | 55 (cv 27.9%) | 364 (cv 22.8%) | 56 (cv 16.8%) | 165 (cv 41.9%) | 113 (cv 45.3%) | 71 (cv 18.7%) |
| 16 KiB values | 1 | 198 (cv 5.1%) | 317 (cv 9.8%) | 197 (cv 0.5%) | 152 (cv 0.6%) | 144 (cv 1.2%) | 224 (cv 0.5%) |
| 16 KiB values | 4 | 92 (cv 7.8%) | 745 (cv 23.8%) | 88 (cv 10.6%) | 188 (cv 7.4%) | 175 (cv 22.1%) | 140 (cv 9.0%) |
| KV-cache requests | 1 | 21,601 (cv 1.5%) | 26,110 (cv 8.4%) | 22,807 (cv 2.7%) | 19,504 (cv 4.6%) | 19,608 (cv 7.0%) | 24,327 (cv 3.2%) |
| KV-cache requests | 4 | 10,853 (cv 25.1%) | 34,420 (cv 19.5%) | 7,661 (cv 18.6%) | 13,631 (cv 24.4%) | 46,137 (cv 5.5%) | 12,242 (cv 21.4%) |
<!-- END GENERATED:matrix-p99 -->

Server resident memory at the end of each run:

<!-- BEGIN GENERATED:matrix-rss -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Server resident memory at the end of the run in MiB.

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 17 (cv 0.2%) | 50 (cv 0.1%) | 17 (cv 0.2%) | 23 (cv 0.3%) | 31 (cv 0.2%) | 21 (cv 0.3%) |
| 64-byte values | 4 | 17 (cv 0.3%) | 50 (cv 0.1%) | 17 (cv 0.1%) | 24 (cv 0.5%) | 34 (cv 2.1%) | 22 (cv 0.5%) |
| 16 KiB values | 1 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 407 (cv 0.0%) | 339 (cv 0.2%) | 341 (cv 0.0%) |
| 16 KiB values | 4 | 318 (cv 0.0%) | 351 (cv 0.0%) | 318 (cv 0.0%) | 451 (cv 0.8%) | 345 (cv 0.0%) | 341 (cv 0.0%) |
| KV-cache requests | 1 | 2,996 (cv 0.2%) | 2,210 (cv 1.5%) | 2,910 (cv 0.4%) | 3,227 (cv 1.6%) | 2,671 (cv 1.1%) | 2,941 (cv 0.3%) |
| KV-cache requests | 4 | 7,224 (cv 1.1%) | 2,757 (cv 2.3%) | 7,588 (cv 1.7%) | 7,262 (cv 1.6%) | 5,042 (cv 1.7%) | 6,812 (cv 3.2%) |
<!-- END GENERATED:matrix-rss -->

## 3. Latency under offered load (open loop)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/sweep-dark.svg">
  <img alt="Two line charts: achieved operations per second versus offered load, and p99 latency versus offered load, for four servers" src="figures/sweep.svg">
</picture>

<!-- BEGIN GENERATED:sweep -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Open loop, one server CPU, 64-byte values, 16 connections. Latencies in microseconds, measured from the intended send time unless stated.

| System | Offered ops/s | Achieved ops/s | p50 | p99 | p99 service time | Max schedule lag (ms) |
|---|---|---|---|---|---|---|
| arenakv (TCP) | 20,000 | 20,000 (cv 0.0%) | 9 (cv 0.6%) | 293 (cv 88.2%) | 200 (cv 87.8%) | 3.7 (cv 12.5%) |
| arenakv (TCP) | 60,000 | 60,004 (cv 0.0%) | 88 (cv 67.8%) | 891 (cv 33.7%) | 532 (cv 16.8%) | 5.7 (cv 16.6%) |
| arenakv (TCP) | 100,000 | 100,002 (cv 0.0%) | 238 (cv 18.7%) | 2,923 (cv 62.4%) | 774 (cv 18.2%) | 16.1 (cv 33.8%) |
| arenakv (TCP) | 140,000 | 140,002 (cv 0.0%) | 216 (cv 14.5%) | 4,306 (cv 32.2%) | 696 (cv 5.2%) | 39.3 (cv 55.9%) |
| arenakv (TCP) | 180,000 | 180,001 (cv 0.0%) | 191 (cv 14.6%) | 6,842 (cv 134.7%) | 536 (cv 2.5%) | 21.7 (cv 122.1%) |
| arenakv (TCP) | 220,000 | 220,001 (cv 0.0%) | 131 (cv 7.8%) | 5,168 (cv 55.3%) | 364 (cv 4.4%) | 48.4 (cv 56.3%) |
| arenakv (TCP) | 260,000 | 260,009 (cv 0.0%) | 107 (cv 25.7%) | 21,044 (cv 101.4%) | 142 (cv 6.6%) | 36.0 (cv 79.7%) |
| Valkey | 20,000 | 20,000 (cv 0.0%) | 10 (cv 2.8%) | 79 (cv 23.9%) | 70 (cv 38.2%) | 3.8 (cv 14.8%) |
| Valkey | 60,000 | 60,000 (cv 0.0%) | 87 (cv 63.3%) | 935 (cv 18.8%) | 536 (cv 15.1%) | 24.0 (cv 56.6%) |
| Valkey | 100,000 | 100,000 (cv 0.0%) | 342 (cv 15.6%) | 5,898 (cv 13.1%) | 913 (cv 5.6%) | 25.9 (cv 68.8%) |
| Valkey | 140,000 | 139,997 (cv 0.0%) | 189 (cv 8.8%) | 4,279 (cv 38.2%) | 552 (cv 4.2%) | 44.1 (cv 69.6%) |
| Valkey | 180,000 | 179,937 (cv 0.1%) | 99 (cv 1.8%) | 3,655 (cv 134.1%) | 290 (cv 9.1%) | 32.6 (cv 95.9%) |
| Valkey | 220,000 | 209,894 (cv 0.6%) | 251,449 (cv 15.8%) | 443,757 (cv 12.5%) | 140 (cv 0.6%) | 473.1 (cv 10.8%) |
| Valkey | 260,000 | 211,590 (cv 0.4%) | 943,718 (cv 2.2%) | 1,504,916 (cv 1.7%) | 139 (cv 0.7%) | 1,532.2 (cv 1.4%) |
| Dragonfly | 20,000 | 20,000 (cv 0.0%) | 10 (cv 4.4%) | 132 (cv 62.2%) | 96 (cv 58.0%) | 3.7 (cv 17.5%) |
| Dragonfly | 60,000 | 60,011 (cv 0.0%) | 61 (cv 57.0%) | 1,656 (cv 78.3%) | 607 (cv 25.1%) | 25.2 (cv 66.3%) |
| Dragonfly | 100,000 | 99,995 (cv 0.0%) | 492 (cv 19.1%) | 6,901 (cv 16.8%) | 829 (cv 7.4%) | 17.0 (cv 17.6%) |
| Dragonfly | 140,000 | 140,003 (cv 0.0%) | 263 (cv 1.2%) | 7,976 (cv 82.3%) | 542 (cv 8.1%) | 54.3 (cv 106.7%) |
| Dragonfly | 180,000 | 179,945 (cv 0.1%) | 148 (cv 8.1%) | 18,383 (cv 127.5%) | 218 (cv 6.1%) | 55.1 (cv 72.2%) |
| Dragonfly | 220,000 | 209,622 (cv 0.5%) | 254,385 (cv 14.7%) | 413,534 (cv 13.7%) | 102 (cv 9.6%) | 421.7 (cv 14.3%) |
| Dragonfly | 260,000 | 210,161 (cv 0.3%) | 972,240 (cv 5.8%) | 1,553,570 (cv 3.8%) | 98 (cv 1.8%) | 1,578.5 (cv 3.8%) |
| memcached | 20,000 | 20,000 (cv 0.0%) | 9 (cv 1.9%) | 190 (cv 45.0%) | 143 (cv 47.9%) | 3.6 (cv 12.9%) |
| memcached | 60,000 | 60,000 (cv 0.0%) | 82 (cv 80.6%) | 723 (cv 19.4%) | 503 (cv 23.3%) | 9.8 (cv 48.9%) |
| memcached | 100,000 | 99,995 (cv 0.0%) | 336 (cv 17.8%) | 5,095 (cv 23.9%) | 1,010 (cv 9.0%) | 17.5 (cv 11.4%) |
| memcached | 140,000 | 140,004 (cv 0.0%) | 270 (cv 4.3%) | 6,875 (cv 100.8%) | 659 (cv 8.3%) | 41.2 (cv 148.9%) |
| memcached | 180,000 | 180,000 (cv 0.0%) | 183 (cv 9.7%) | 22,479 (cv 78.7%) | 467 (cv 3.7%) | 98.2 (cv 37.9%) |
| memcached | 220,000 | 219,690 (cv 0.3%) | 149 (cv 62.3%) | 59,690 (cv 103.2%) | 212 (cv 10.1%) | 99.5 (cv 79.0%) |
| memcached | 260,000 | 230,693 (cv 0.5%) | 585,525 (cv 6.0%) | 1,231,448 (cv 7.1%) | 140 (cv 1.0%) | 1,281.4 (cv 6.4%) |
<!-- END GENERATED:sweep -->

Why latency is measured from the intended send time:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/coordinated-omission-dark.svg">
  <img alt="Two line charts comparing p99 latency from the intended send time with the pure service time as offered load grows" src="figures/coordinated-omission.svg">
</picture>

## 4. Pipelining and connection count

Closed loop, one server CPU, 64-byte values, 90% GET, 4 client threads. Left: 16 connections with 1, 8 or 32
requests in flight each. Middle and right: 16 to 1024 connections, one request in flight each.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/concurrency-dark.svg">
  <img alt="Three line charts for five servers: throughput by pipeline depth, throughput by connection count, and p99 latency by connection count" src="figures/concurrency.svg">
</picture>

<!-- BEGIN GENERATED:pipeline -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, 16 connections from 4 client threads, with the given number of requests in flight per connection. Latencies are GET latencies in microseconds. Server kernel share: kernel-mode part of the server CPU time (stime in /proc/<pid>/stat), that is system calls and the TCP stack run on the server's behalf.

| System | Depth | ops/s | p50 | p99 | Server CPU µs/op | Server kernel share | Server RSS MiB |
|---|---|---|---|---|---|---|---|
| arenakv (TCP) | 1 | 306,433 (cv 0.4%) | 48 (cv 0.7%) | 101 (cv 0.6%) | 3.26 (cv 0.5%) | 86% (cv 0.5%) | 17 (cv 0.2%) |
| arenakv (TCP) | 8 | 2,014,072 (cv 0.7%) | 58 (cv 1.1%) | 122 (cv 0.5%) | 0.50 (cv 0.7%) | 73% (cv 0.8%) | 17 (cv 0.2%) |
| arenakv (TCP) | 32 | 5,130,043 (cv 0.3%) | 90 (cv 0.5%) | 192 (cv 0.4%) | 0.20 (cv 0.4%) | 52% (cv 1.9%) | 17 (cv 0.3%) |
| arenakv (memcached protocol) | 1 | 293,627 (cv 0.7%) | 50 (cv 1.1%) | 105 (cv 0.7%) | 3.40 (cv 0.7%) | 85% (cv 1.0%) | 17 (cv 0.3%) |
| arenakv (memcached protocol) | 8 | 1,688,995 (cv 0.3%) | 70 (cv 0.4%) | 146 (cv 0.7%) | 0.59 (cv 0.3%) | 63% (cv 0.4%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 32 | 3,551,958 (cv 0.4%) | 131 (cv 0.4%) | 281 (cv 0.9%) | 0.28 (cv 0.6%) | 37% (cv 1.8%) | 17 (cv 0.1%) |
| Valkey | 1 | 222,416 (cv 0.4%) | 67 (cv 0.6%) | 133 (cv 0.6%) | 4.49 (cv 0.5%) | 83% (cv 0.9%) | 23 (cv 0.3%) |
| Valkey | 8 | 1,168,070 (cv 0.3%) | 100 (cv 0.4%) | 168 (cv 0.5%) | 0.86 (cv 0.2%) | 56% (cv 1.2%) | 23 (cv 0.5%) |
| Valkey | 32 | 2,250,029 (cv 0.1%) | 186 (cv 0.3%) | 346 (cv 0.4%) | 0.44 (cv 0.0%) | 29% (cv 1.0%) | 23 (cv 0.4%) |
| Dragonfly | 1 | 216,633 (cv 0.4%) | 73 (cv 0.6%) | 91 (cv 2.5%) | 4.61 (cv 0.4%) | 74% (cv 0.9%) | 32 (cv 2.6%) |
| Dragonfly | 8 | 815,180 (cv 0.3%) | 143 (cv 0.4%) | 251 (cv 1.2%) | 1.23 (cv 0.3%) | 38% (cv 0.8%) | 32 (cv 0.2%) |
| Dragonfly | 32 | 1,296,459 (cv 0.8%) | 351 (cv 0.5%) | 673 (cv 3.9%) | 0.77 (cv 0.8%) | 20% (cv 3.6%) | 33 (cv 0.2%) |
| memcached | 1 | 265,205 (cv 0.3%) | 54 (cv 0.4%) | 119 (cv 0.5%) | 3.77 (cv 0.3%) | 82% (cv 0.5%) | 21 (cv 0.1%) |
| memcached | 8 | 1,227,908 (cv 0.2%) | 87 (cv 0.3%) | 279 (cv 0.4%) | 0.81 (cv 0.2%) | 56% (cv 1.4%) | 21 (cv 0.3%) |
| memcached | 32 | 1,583,650 (cv 0.4%) | 283 (cv 0.5%) | 569 (cv 1.2%) | 0.63 (cv 0.5%) | 45% (cv 1.6%) | 22 (cv 0.4%) |
<!-- END GENERATED:pipeline -->

The server CPU time per operation of the pipeline table, split into kernel mode and user mode (columns "Server CPU
µs/op" and "Server kernel share"):

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/cpu-split-dark.svg">
  <img alt="Stacked bar charts of server CPU time per operation for five servers at pipeline depths 1 and 32, split into kernel and user mode" src="figures/cpu-split.svg">
</picture>

<!-- BEGIN GENERATED:connections -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 64-byte values, 90% GET, connections spread over 4 client threads, one request in flight per connection. Latencies are GET latencies in microseconds. Server kernel share: kernel-mode part of the server CPU time (stime in /proc/<pid>/stat), that is system calls and the TCP stack run on the server's behalf.

| System | Connections | ops/s | p50 | p99 | Server CPU µs/op | Server kernel share | Server RSS MiB |
|---|---|---|---|---|---|---|---|
| arenakv (TCP) | 16 | 307,751 (cv 0.5%) | 48 (cv 0.5%) | 100 (cv 0.3%) | 3.25 (cv 0.4%) | 86% (cv 0.2%) | 17 (cv 0.1%) |
| arenakv (TCP) | 64 | 318,082 (cv 0.6%) | 193 (cv 0.8%) | 394 (cv 0.6%) | 3.15 (cv 0.7%) | 85% (cv 0.3%) | 17 (cv 0.1%) |
| arenakv (TCP) | 256 | 313,399 (cv 0.5%) | 813 (cv 0.3%) | 878 (cv 2.0%) | 3.19 (cv 0.4%) | 83% (cv 1.1%) | 19 (cv 0.2%) |
| arenakv (TCP) | 1,024 | 291,570 (cv 1.0%) | 3,464 (cv 0.9%) | 4,699 (cv 3.9%) | 3.43 (cv 1.0%) | 79% (cv 0.1%) | 27 (cv 0.1%) |
| arenakv (memcached protocol) | 16 | 295,386 (cv 0.5%) | 50 (cv 0.7%) | 105 (cv 0.3%) | 3.38 (cv 0.4%) | 85% (cv 1.0%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 64 | 301,878 (cv 0.4%) | 204 (cv 0.7%) | 413 (cv 0.4%) | 3.31 (cv 0.4%) | 84% (cv 0.3%) | 17 (cv 0.1%) |
| arenakv (memcached protocol) | 256 | 300,454 (cv 1.0%) | 846 (cv 1.1%) | 934 (cv 2.5%) | 3.33 (cv 0.9%) | 83% (cv 1.2%) | 19 (cv 0.2%) |
| arenakv (memcached protocol) | 1,024 | 291,933 (cv 0.2%) | 3,454 (cv 0.4%) | 4,594 (cv 3.7%) | 3.42 (cv 0.3%) | 81% (cv 1.0%) | 25 (cv 0.1%) |
| Valkey | 16 | 221,757 (cv 0.4%) | 67 (cv 0.4%) | 134 (cv 0.4%) | 4.51 (cv 0.4%) | 83% (cv 0.6%) | 23 (cv 0.5%) |
| Valkey | 64 | 229,629 (cv 0.5%) | 269 (cv 0.6%) | 546 (cv 0.6%) | 4.35 (cv 0.5%) | 82% (cv 0.7%) | 23 (cv 0.5%) |
| Valkey | 256 | 229,266 (cv 0.4%) | 1,086 (cv 0.4%) | 2,189 (cv 0.4%) | 4.36 (cv 0.4%) | 81% (cv 0.5%) | 24 (cv 0.6%) |
| Valkey | 1,024 | 224,847 (cv 0.6%) | 4,424 (cv 0.5%) | 8,821 (cv 0.4%) | 4.45 (cv 0.5%) | 80% (cv 0.4%) | 27 (cv 4.2%) |
| Dragonfly | 16 | 216,197 (cv 0.6%) | 73 (cv 0.4%) | 93 (cv 2.1%) | 4.63 (cv 0.6%) | 74% (cv 1.0%) | 32 (cv 2.3%) |
| Dragonfly | 64 | 213,789 (cv 0.4%) | 298 (cv 0.4%) | 334 (cv 1.2%) | 4.68 (cv 0.4%) | 74% (cv 0.8%) | 34 (cv 2.3%) |
| Dragonfly | 256 | 208,649 (cv 0.7%) | 1,227 (cv 0.9%) | 1,347 (cv 1.3%) | 4.79 (cv 0.7%) | 73% (cv 0.7%) | 42 (cv 0.2%) |
| Dragonfly | 1,024 | 181,502 (cv 3.0%) | 5,531 (cv 3.1%) | 7,661 (cv 4.1%) | 5.51 (cv 3.1%) | 65% (cv 1.9%) | 75 (cv 0.9%) |
| memcached | 16 | 264,996 (cv 0.5%) | 54 (cv 0.6%) | 120 (cv 1.2%) | 3.77 (cv 0.5%) | 82% (cv 0.6%) | 21 (cv 0.3%) |
| memcached | 64 | 269,749 (cv 0.6%) | 225 (cv 0.7%) | 461 (cv 1.5%) | 3.71 (cv 0.6%) | 81% (cv 0.8%) | 21 (cv 0.3%) |
| memcached | 256 | 264,057 (cv 2.3%) | 947 (cv 1.3%) | 1,619 (cv 20.6%) | 3.79 (cv 2.3%) | 79% (cv 1.3%) | 22 (cv 0.1%) |
| memcached | 1,024 | 258,312 (cv 1.7%) | 3,876 (cv 1.7%) | 5,964 (cv 5.2%) | 3.87 (cv 1.7%) | 76% (cv 0.4%) | 22 (cv 0.3%) |
<!-- END GENERATED:connections -->

## 5. KV-cache workload: reuse versus capacity

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/capacity-dark.svg">
  <img alt="Two line charts: reuse rate and requests per second versus configured cache capacity for four servers" src="figures/capacity.svg">
</picture>

<!-- BEGIN GENERATED:capacity-reuse -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, reuse rate (fraction of needed blocks served from the cache) by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.183 (cv 2.7%) | 0.430 (cv 0.2%) | 0.643 (cv 0.2%) | 0.822 (cv 0.1%) | 0.904 (cv 0.0%) |
| Valkey | 0.036 (cv 4.0%) | 0.115 (cv 1.0%) | 0.304 (cv 0.8%) | 0.789 (cv 0.5%) | 0.902 (cv 0.0%) |
| Dragonfly | n/a | n/a | 0.407 (cv 1.3%) | 0.689 (cv 0.7%) | 0.902 (cv 0.0%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.435 (cv 0.4%) | 0.600 (cv 0.7%) | 0.902 (cv 0.0%) |
| memcached | 0.354 (cv 0.3%) | 0.497 (cv 0.2%) | 0.664 (cv 0.2%) | 0.845 (cv 0.1%) | 0.903 (cv 0.0%) |
<!-- END GENERATED:capacity-reuse -->

<!-- BEGIN GENERATED:capacity-throughput -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, requests per second by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 2,342 (cv 0.8%) | 2,038 (cv 0.2%) | 1,994 (cv 0.4%) | 1,960 (cv 0.8%) | 1,952 (cv 0.4%) |
| Valkey | 1,981 (cv 0.4%) | 1,832 (cv 0.5%) | 1,752 (cv 0.9%) | 1,673 (cv 1.4%) | 1,625 (cv 0.9%) |
| Dragonfly | n/a | n/a | 172 (cv 1.8%) | 522 (cv 3.3%) | 1,586 (cv 1.4%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 1,648 (cv 0.8%) | 1,641 (cv 0.9%) | 1,592 (cv 1.5%) |
| memcached | 2,031 (cv 1.2%) | 1,901 (cv 1.0%) | 1,894 (cv 0.4%) | 1,888 (cv 0.3%) | 1,790 (cv 0.5%) |
<!-- END GENERATED:capacity-throughput -->

Share of requests that failed because the server rejected an operation:

<!-- BEGIN GENERATED:capacity-errors -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Share of KV-cache requests that failed (an operation was rejected by the server, for example with an out-of-memory error), by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |
| Valkey | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |
| Dragonfly | n/a | n/a | 96.27% | 85.35% | 0.00% |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.00% | 0.00% | 0.00% |
| memcached | 0.01% | 0.01% | 0.01% | 0.00% | 0.00% |
<!-- END GENERATED:capacity-errors -->

<!-- BEGIN GENERATED:capacity-skipped -->
Skipped configurations: dragonfly at 16mb (dragonfly refuses to start with less than 256 MiB per thread); dragonfly at 64mb (dragonfly refuses to start with less than 256 MiB per thread).
<!-- END GENERATED:capacity-skipped -->

## 6. arenakv engine

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/engine-dark.svg">
  <img alt="Engine thread scaling, tag comparison cost of the vector and scalar versions, and key hash throughput" src="figures/engine.svg">
</picture>

<!-- BEGIN GENERATED:engine-scale -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,733,462 (cv 1.1%) | 6,392,434 (cv 0.8%) | 10,982,242 (cv 1.5%) | 17,475,078 (cv 2.0%) | 21,333,074 (cv 1.6%) | 22,255,629 (cv 4.8%) |
| 4096-byte values | 1,503,683 (cv 1.7%) | 2,550,371 (cv 0.6%) | 3,919,707 (cv 0.9%) | 5,381,192 (cv 1.0%) | 5,727,927 (cv 1.4%) | 6,014,616 (cv 0.8%) |
<!-- END GENERATED:engine-scale -->

<!-- BEGIN GENERATED:engine-micro -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

| Measurement | Result |
|---|---|
| 16-lane tag comparison, vector (sse2) | 0.691 ns (cv 5.1%) |
| 16-lane tag comparison, scalar reference | 8.312 ns (cv 0.2%) |
| Speedup of the vector version | 12.1x (cv 5.1%) |
| Key hash, 8-byte key | 3.07 ns (cv 0.5%) |
| Key hash, 16-byte key | 4.20 ns (cv 1.8%) |
| Key hash, 32-byte key | 6.48 ns (cv 1.1%) |
| Key hash, 64-byte key | 11.17 ns (cv 1.2%) |
| Key hash, 256-byte key | 39.41 ns (cv 0.5%) |
| Key hash, 1024-byte key | 151.10 ns (cv 0.5%) |
| Key hash, 4096-byte key | 599.15 ns (cv 0.7%) |
<!-- END GENERATED:engine-micro -->

## 7. rdmaduct transport (loopback on one host)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/transport-dark.svg">
  <img alt="Two line charts: median round-trip time and streaming throughput versus message size for TCP and Soft-RoCE in polling and event-driven mode" src="figures/transport.svg">
</picture>

<!-- BEGIN GENERATED:transport-rtt -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Median round-trip time in microseconds (ping-pong, one message in flight). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 7.4 (cv 3.2%) | 6.9 (cv 5.2%) | 10.4 (cv 7.2%) |
| 256 | 7.5 (cv 2.6%) | 6.7 (cv 2.7%) | 10.4 (cv 4.7%) |
| 1024 | 7.7 (cv 4.6%) | 7.1 (cv 3.1%) | 11.2 (cv 7.5%) |
| 4096 | 8.5 (cv 2.4%) | 11.4 (cv 1.8%) | 15.3 (cv 2.9%) |
| 16384 | 10.8 (cv 3.7%) | 30.5 (cv 0.9%) | 35.9 (cv 1.7%) |
| 65536 | 22.7 (cv 1.2%) | 109.5 (cv 3.1%) | 114.0 (cv 5.3%) |
| 262144 | 48.9 (cv 3.4%) | 451.7 (cv 3.1%) | 454.2 (cv 0.8%) |
| 1048576 | 181.0 (cv 1.4%) | 1,889.5 (cv 0.8%) | 1,932.7 (cv 1.1%) |
<!-- END GENERATED:transport-rtt -->

<!-- BEGIN GENERATED:transport-rtt-p99 -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

p99 round-trip time in microseconds. Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 13.3 (cv 28.7%) | 9.2 (cv 10.5%) | 14.4 (cv 14.1%) |
| 256 | 12.3 (cv 29.1%) | 8.6 (cv 12.0%) | 13.2 (cv 7.8%) |
| 1024 | 13.9 (cv 27.6%) | 8.6 (cv 5.3%) | 15.9 (cv 18.1%) |
| 4096 | 12.0 (cv 26.8%) | 14.1 (cv 1.8%) | 19.3 (cv 13.9%) |
| 16384 | 15.7 (cv 27.8%) | 40.5 (cv 7.6%) | 53.4 (cv 29.7%) |
| 65536 | 48.8 (cv 87.7%) | 149.9 (cv 17.7%) | 166.8 (cv 6.3%) |
| 262144 | 57.7 (cv 7.5%) | 588.8 (cv 9.4%) | 628.0 (cv 3.7%) |
| 1048576 | 241.8 (cv 46.3%) | 2,416.7 (cv 15.8%) | 2,793.8 (cv 2.9%) |
<!-- END GENERATED:transport-rtt-p99 -->

<!-- BEGIN GENERATED:transport-stream -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

One-way streaming throughput in MB/s (batched sends, 64 buffers per direction). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 315 (cv 2.7%) | 46 (cv 5.7%) | 44 (cv 5.3%) |
| 256 | 1,161 (cv 4.0%) | 187 (cv 6.5%) | 167 (cv 10.1%) |
| 1024 | 3,137 (cv 11.1%) | 703 (cv 9.7%) | 679 (cv 5.6%) |
| 4096 | 7,913 (cv 5.9%) | 898 (cv 4.4%) | 935 (cv 2.0%) |
| 16384 | 10,631 (cv 12.8%) | 1,130 (cv 0.9%) | 1,139 (cv 1.1%) |
| 65536 | 10,538 (cv 8.1%) | 1,293 (cv 1.6%) | 1,284 (cv 0.5%) |
| 262144 | 6,007 (cv 4.6%) | 1,176 (cv 2.9%) | 1,176 (cv 1.2%) |
| 1048576 | 8,037 (cv 16.8%) | 1,118 (cv 1.6%) | 1,091 (cv 0.9%) |
<!-- END GENERATED:transport-stream -->

## 8. Validation of the load generator

`kvpace` against `memtier_benchmark` on the same server and settings (see
[`../kvpace/docs/validation.md`](../kvpace/docs/validation.md)):

<!-- BEGIN GENERATED:validation -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

One server CPU, 64-byte values, 90% GET. memtier_benchmark runs `-t 1 -c 1` for one connection and `-t 4 -c 4` for 16. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.

| Server | Connections | kvpace client model | kvpace ops/s | memtier ops/s | Ratio | kvpace p50 | memtier p50 | kvpace p99 | memtier p99 |
|---|---|---|---|---|---|---|---|---|---|
| Valkey (RESP) | 1 | 1 thread, 1 connection | 116,870 (cv 0.8%) | 89,611 (cv 1.1%) | 1.30 | 8.4 (cv 1.2%) | 15.0 (cv 0.0%) | 11 (cv 1.9%) | 15 (cv 0.0%) |
| Valkey (RESP) | 16 | 4 threads x 4 connections | 221,642 (cv 0.6%) | 227,090 (cv 0.4%) | 0.98 | 67.0 (cv 0.6%) | 71.0 (cv 0.0%) | 133 (cv 0.3%) | 98 (cv 4.5%) |
| Valkey (RESP) | 16 | 16 threads x 1 connection | 209,842 (cv 0.6%) | 227,090 (cv 0.4%) | 0.92 | 72.7 (cv 0.7%) | 71.0 (cv 0.0%) | 140 (cv 0.8%) | 98 (cv 4.5%) |
| arenakv (memcached protocol) | 1 | 1 thread, 1 connection | 136,302 (cv 0.5%) | 101,226 (cv 0.9%) | 1.35 | 7.2 (cv 0.5%) | 15.0 (cv 0.0%) | 9 (cv 2.0%) | 15 (cv 0.0%) |
| arenakv (memcached protocol) | 16 | 4 threads x 4 connections | 295,443 (cv 0.8%) | 293,834 (cv 0.5%) | 1.01 | 49.6 (cv 0.5%) | 50.2 (cv 8.7%) | 104 (cv 0.3%) | 105 (cv 3.4%) |
| arenakv (memcached protocol) | 16 | 16 threads x 1 connection | 258,669 (cv 1.9%) | 293,834 (cv 0.5%) | 0.88 | 57.6 (cv 1.5%) | 50.2 (cv 8.7%) | 120 (cv 1.3%) | 105 (cv 3.4%) |
<!-- END GENERATED:validation -->

## 9. Persistence and replication

The cost of keeping data: no persistence, an append-only log synced every second or on every write, and one
asynchronous replica, for arenakv over its native protocol and Valkey over RESP. The log is written to the host's
NVMe disk; the environment table above gives the median time of one fsync on it.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/durability-dark.svg">
  <img alt="Two bar charts on logarithmic axes: throughput and median PUT latency of arenakv and Valkey without persistence, with an append-only log synced every second, synced on every write, and with one replica" src="figures/durability.svg">
</picture>

<!-- BEGIN GENERATED:durability -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 16 connections, 50% PUT of 64-byte values (100,000 keys). The log is written to the NVMe disk of the host. The replica runs on another physical core; replication is asynchronous, so the primary's numbers include sending the changes but not waiting for them. Latencies in microseconds; ratio: throughput relative to no persistence of the same system.

| System | Durability | ops/s | Ratio | PUT p50 | PUT p99 | GET p50 | GET p99 | Server CPU µs/op |
|---|---|---|---|---|---|---|---|---|
| arenakv (TCP) | no persistence | 266,408 (cv 0.4%) | 1.00 | 56 (cv 0.8%) | 117 (cv 0.5%) | 56 (cv 0.8%) | 117 (cv 0.5%) | 3.75 (cv 0.4%) |
| arenakv (TCP) | log, fsync every second | 251,893 (cv 1.3%) | 0.95 | 57 (cv 1.0%) | 119 (cv 1.1%) | 57 (cv 1.0%) | 119 (cv 1.1%) | 3.97 (cv 1.3%) |
| arenakv (TCP) | log, fsync per write | 2,633 (cv 0.3%) | 0.01 | 12,177 (cv 0.5%) | 12,911 (cv 2.6%) | 16 (cv 7.1%) | 121 (cv 9.1%) | 6.08 (cv 8.6%) |
| arenakv (TCP) | one replica | 261,512 (cv 0.6%) | 0.98 | 57 (cv 0.9%) | 119 (cv 0.5%) | 57 (cv 0.8%) | 119 (cv 0.5%) | 3.82 (cv 0.5%) |
| Valkey | no persistence | 208,235 (cv 0.5%) | 1.00 | 74 (cv 0.4%) | 142 (cv 0.4%) | 73 (cv 0.3%) | 141 (cv 0.5%) | 4.80 (cv 0.4%) |
| Valkey | log, fsync every second | 196,569 (cv 0.6%) | 0.94 | 76 (cv 0.3%) | 148 (cv 0.3%) | 76 (cv 0.3%) | 147 (cv 0.6%) | 5.01 (cv 0.9%) |
| Valkey | log, fsync per write | 1,642 (cv 17.3%) | 0.01 | 11,089 (cv 24.2%) | 13,186 (cv 5.3%) | 11,089 (cv 24.2%) | 13,120 (cv 4.9%) | 9.28 (cv 17.9%) |
| Valkey | one replica | 196,479 (cv 0.6%) | 0.94 | 79 (cv 0.7%) | 140 (cv 5.4%) | 79 (cv 0.5%) | 127 (cv 8.0%) | 5.09 (cv 0.6%) |
<!-- END GENERATED:durability -->

## 10. Encryption

The cost of encryption on three paths: arenakv's native protocol over rdmaduct's encrypted endpoints, and RESP over
TLS for arenakv and Valkey, each measured unencrypted and encrypted on the same server binary.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="figures/encryption-dark.svg">
  <img alt="Two bar charts: throughput of arenakv native, arenakv RESP and Valkey RESP, unencrypted and encrypted, for 64-byte and 16 KiB values" src="figures/encryption.svg">
</picture>

<!-- BEGIN GENERATED:encryption -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Closed loop, one server CPU, 16 connections, 90% GET. arenakv native: rdmaduct encrypted endpoints (TLS 1.3 handshake, AES-256-GCM per message) over TCP; RESP: TLS 1.3 streams. All arenakv runs use the arenakvd of the TLS=1 build. Ratio: encrypted throughput over unencrypted. Latencies are GET latencies in microseconds.

| Path | Values | ops/s plain | ops/s encrypted | Ratio | Server CPU µs/op plain | Server CPU µs/op encrypted | p99 plain | p99 encrypted |
|---|---|---|---|---|---|---|---|---|
| arenakv native | 64-byte values | 267,096 (cv 0.7%) | 228,235 (cv 0.8%) | 0.85 | 3.75 (cv 0.7%) | 4.38 (cv 0.9%) | 116 (cv 0.4%) | 136 (cv 0.4%) |
| arenakv native | 16 KiB values | 162,115 (cv 1.3%) | 112,010 (cv 0.8%) | 0.69 | 6.17 (cv 1.3%) | 8.92 (cv 0.8%) | 194 (cv 1.1%) | 279 (cv 1.2%) |
| arenakv RESP | 64-byte values | 260,278 (cv 0.7%) | 183,277 (cv 1.0%) | 0.70 | 3.84 (cv 0.7%) | 5.45 (cv 1.1%) | 119 (cv 0.5%) | 169 (cv 0.5%) |
| arenakv RESP | 16 KiB values | 159,174 (cv 0.6%) | 76,108 (cv 0.5%) | 0.48 | 6.28 (cv 0.7%) | 13.14 (cv 0.5%) | 197 (cv 0.3%) | 407 (cv 0.3%) |
| Valkey RESP | 64-byte values | 210,823 (cv 1.1%) | 163,771 (cv 1.1%) | 0.78 | 4.74 (cv 1.1%) | 6.11 (cv 1.1%) | 139 (cv 0.8%) | 180 (cv 1.2%) |
| Valkey RESP | 16 KiB values | 142,592 (cv 0.4%) | 70,485 (cv 1.4%) | 0.49 | 7.01 (cv 0.4%) | 14.19 (cv 1.4%) | 158 (cv 6.7%) | 309 (cv 16.0%) |
<!-- END GENERATED:encryption -->

## Findings

Each statement below is checked against the data (tag `[C<n>]`, definition in [`tools/claims.py`](tools/claims.py)).
Differences are stated only for means or, where noted, for non-overlapping repetition ranges.

**Closed-loop throughput and server CPU (loopback, TCP)**

- arenakv over TCP, with its native protocol or with the memcached protocol, has the highest mean throughput in all
  six rows of the matrix [C1]. At one server CPU arenakv over the memcached protocol is faster than memcached in all
  three workloads, with non-overlapping ranges [C3]; both serve the same protocol to the same client, so the
  difference is the server.
- At four server CPUs the throughput of arenakv over its native protocol is higher than that of Valkey and of
  Dragonfly in all three workloads, with non-overlapping ranges [C2]. The four-CPU rows may be limited by the load
  generator (see [`METHODOLOGY.md`](METHODOLOGY.md#threats-to-validity)), so they show the order of the systems
  under this client, not server capacity.
- One of the two arenakv TCP rows has the lowest mean server CPU time per operation of the five TCP servers in every
  row [C4]. At four server CPUs arenakv over its native protocol uses less server CPU time per operation than Valkey and
  Dragonfly in all three workloads [C5]. The native protocol and the memcached protocol on the same server differ by
  less than 5% in server CPU time per operation in every row [C29]. In run2 (`data/2026-10-21-run2`) the native
  protocol cost more in every row [C32]; the TCP transport changes between the runs (one read per batch, byte-sized send
  batches, direct waiting without a second epoll set, delivery in place; see
  [`rdmaduct/docs/ARCHITECTURE.md`](../rdmaduct/docs/ARCHITECTURE.md)) removed that difference.
- arenakv over Soft-RoCE has lower mean throughput than arenakv over TCP in all six rows [C6]. Soft-RoCE is a software
  implementation sharing the CPUs with the load generator, so these rows say nothing about RDMA hardware.
- No operation failed in any matrix run [C22].
- With 64-byte values arenakv has a lower mean resident memory than each of Valkey, Dragonfly and memcached, below
  20 MiB, at one and at four server CPUs [C16]. In run1 it used more than eight times their memory [C33], because
  the index was sized from the configured capacity; the index now starts small and grows with the data.

**Latency under offered load (open loop, one server CPU, 64-byte values)**

- Up to 140,000 offered operations per second all four servers of the sweep achieve the offered rate within 1% [C8]. At
  260,000 offered operations per second, the highest rate of the sweep, arenakv (TCP) achieves the offered rate within
  1% in every repetition, while Valkey, Dragonfly and memcached achieve less than 240,000 operations per second in
  every repetition [C7]. The saturation rate of arenakv lies above the range of the sweep.
- At 260,000 offered operations per second the p99 latency measured from the intended send time exceeds the p99
  service time by more than a factor of 1,000 for Valkey, Dragonfly and memcached [C9]. A closed-loop or
  service-time-only measurement would hide this backlog; it is the reason latency is measured from the intended send
  time.
- Below saturation the p99 latency has a coefficient of variation above 20% between repetitions in at least half of the
  16 cells [C10]. Tail latencies at moderate load are noisy on this machine and must not be used to rank servers. Between
  100,000 and 220,000 operations per second the p99 curves of arenakv and Valkey are not monotonic, while those of
  Dragonfly and memcached rise at every step [C36]. This run used the `performance`
  governor, so frequency scaling does not explain the shape; client thread scheduling is a hypothesis that was not
  tested.

**Pipelining and connection count (closed loop, one server CPU, 64-byte values)**

- At pipeline depth 32 native arenakv has the highest throughput of the five servers, with non-overlapping ranges, and
  at least twice the mean throughput of Valkey, Dragonfly and memcached [C24].
- From depth 1 to 32 the throughput of native arenakv grows by a factor above 15, that of arenakv over the memcached
  protocol and of Valkey by a factor between 9 and 13, and that of Dragonfly and memcached by less than 6.5 [C25].
  Deeper pipelines let the server answer many requests with one `recv()` and one `send()`.
- From 16 to 1024 connections the mean throughput of native arenakv falls by between 5% and 6%, that of arenakv over
  the memcached protocol, memcached and Valkey changes by less than 3%, and that of Dragonfly falls by more than 15%
  [C26].
- At 1024 connections arenakv over the memcached protocol has the lowest mean GET p99 latency of the five servers
  [C27].

**Where the server CPU time goes (pipeline suite, one server CPU)**

- With one request in flight per connection, kernel mode accounts for more than 80% of the server CPU time of native
  arenakv in every repetition, and for more than 70% of the server CPU time of each of the five TCP servers [C28].
  Kernel mode is `stime` in `/proc/<pid>/stat`: system calls and the TCP stack run on the server's behalf. At this
  depth the cost of a request is dominated by the kernel network path, not by the cache.
- From one to 32 requests in flight the kernel share falls for each of the five servers, with non-overlapping ranges
  [C30]: one system call carries many requests, so the kernel cost per operation shrinks faster than the user-mode
  cost.
- The kernel-mode part is the cost that RDMA hardware is designed to remove from the request path: the NIC places
  messages in registered user memory, and a polling server receives them without a system call. This run cannot show
  that effect. Soft-RoCE implements RDMA inside the kernel on the same CPUs, so its rows include that kernel work as
  well [C6]. A measurement on RDMA hardware is listed in [`docs/ROADMAP.md`](../docs/ROADMAP.md).

**KV-cache workload, by configured capacity (one server CPU, 8 connections)**

- At 16, 64 and 256 MB the reuse rate of arenakv (TCP) is higher than that of Valkey, with non-overlapping ranges [C11].
  memcached has a higher mean reuse rate than arenakv (TCP) at every configured capacity from 16 MB to 1 GB, and the
  difference is below 0.03 at 256 MB and 1 GB [C12]. At 4 GB, where the working set fits, the mean reuse rates of all systems that ran
  differ by less than 0.005 [C13]. "Configured capacity" is accounted differently by each system (see the methodology),
  so this compares systems at equal configuration, not at equal total memory.
- With default flags Dragonfly in cache mode failed more than 90% of requests at 256 MB and more than 80% at 1 GB with
  out-of-memory errors; with `--max_eviction_per_heartbeat` raised no request failed [C14]. Both variants are shown;
  the raised flag was chosen after the failures were observed in run1 [C37] (see the methodology). memcached failed fewer
  than 0.1% of requests in every capacity configuration; arenakv and Valkey failed none [C15].

**Engine and transport**

- Engine throughput with 64-byte values grows by a factor between 4 and 6 from one to eight threads, with 4 KiB values
  by a factor below 4, and 4 KiB throughput grows by less than 15% from 8 to 16 threads (the added threads are
  hyper-thread siblings) [C17]. The vector tag comparison is at least 10 times faster than the scalar reference [C18]; the
  reference is compiled at `-O2` without auto-vectorization.
- Loopback transport: for messages up to 1 KiB the median round trip of Soft-RoCE with polling is lower than that of
  TCP; for messages of 16 KiB and larger TCP is lower than both Soft-RoCE modes [C19]. TCP streaming throughput is at
  least four times that of both Soft-RoCE modes at every measured message size [C20]. In run1 TCP was slower than
  Soft-RoCE with polling for messages up to 1 KiB [C34]; the TCP transport now sends a batch with one `sendmsg()` and buffers its reads.
  These are software-RDMA results.

**Load generator**

- With the client model of `memtier_benchmark` (4 threads with 4 connections each) kvpace measures a throughput within
  3% of `memtier_benchmark` with 16 connections, and at least 25% higher with one connection, on both validation
  servers [C21]. With one blocking thread per connection (the model of the matrix suite) it measures between 5% and 15% less with
  16 connections [C23]. The one-connection difference is the per-request cost of the clients themselves: kvpace
  measures a p50 latency of 7 to 9 microseconds where memtier reports 15 [C35]. Comparisons between servers must use one tool and one client model
  for all of them.

**Persistence and replication (closed loop, one server CPU, 50% PUT of 64-byte values)**

- With the append-only log synced every second arenakv and Valkey each keep at least 93% of their throughput without
  persistence [C39]; with one asynchronous replica arenakv keeps at least 97% and Valkey at least 93% [C42].
- Synced on every write, the log limits both systems to less than 2% of their throughput without persistence; arenakv
  is ahead of Valkey with non-overlapping ranges [C40]. The limit is the disk: the median PUT latency of arenakv lies
  between 1.5 and 2.5 times the median time of one fsync on the log disk [C43]. That is consistent with a write
  waiting for the sync in progress and then for the next one, which covers the writes of all waiting connections
  (group commit, see [`../arenakv/docs/OPERATIONS.md`](../arenakv/docs/OPERATIONS.md#append-only-log)).
- In that mode the GET median latency of arenakv stays below 50 microseconds while that of Valkey exceeds
  5 milliseconds [C41]: arenakv answers reads while writes wait for the disk, whereas Valkey's event loop syncs before
  it answers any client.

**Encryption (closed loop, one server CPU, 90% GET)**

- For 64-byte and 16 KiB values the encrypted native protocol of arenakv keeps a larger share of its unencrypted
  throughput than TLS keeps on the RESP path of arenakv and of Valkey [C44], and encryption adds less server CPU time
  per operation to it [C46]. Encrypted, it is faster than Valkey over TLS at both value sizes, with non-overlapping
  ranges [C45]. The paths differ in their design: the native path seals each message with AES-256-GCM while copying
  it into the send buffer and opens it in place on receipt, while TLS frames records in a byte stream with buffers
  of its own; which of these differences accounts for the cost was not measured.

## What these results do not show

- Performance on RDMA hardware. The RDMA rows use Soft-RoCE, which runs the protocol in the kernel on the same CPUs.
- Performance across a physical network, with other CPUs, kernels or memory systems.
- Tuned configurations of any system, or behavior under workloads other than the three defined in the methodology.
- Differences smaller than the spread between repetitions (shown as `cv` and whiskers).
- Persistence on other disks: synced writes are bounded by the fsync time of the one NVMe disk measured. The replica
  runs on the same host, so replication over a network and the replica's lag were not measured.
- Encryption over RDMA: the encryption suite uses TCP only.

## Reproducing and checking

```
make -C .. && make -C .. verify          # build and check the published run against its raw data
tools/run.py --suite all --reps 5 --out data/<new-run>     # a new run (needs Docker, RDMA_ADDR, BENCH_NIC)
tools/report.py data/<new-run> --update-docs               # regenerate figures, tables and doc blocks
tools/verify.py data/<new-run>                             # verify
```

Requirements, machine preparation, suites, output layout, publishing a run and troubleshooting:
[`RUNNING.md`](RUNNING.md). Details and how to challenge a result:
[`METHODOLOGY.md`](METHODOLOGY.md#challenging-a-result).
