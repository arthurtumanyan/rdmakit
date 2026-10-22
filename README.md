# rdmakit

RDMA software in C, with the components measured against established systems:

- **`rdmaduct`**: a message transport over RDMA reliable connections or TCP.
- **`arenakv`**: a key-value cache for large blocks, built on that transport.
- **`kvpace`**: a load generator that drives `arenakv` and the established servers with the same code.

| Project | What it is | Status | Documentation |
|---|---|---|---|
| [`rdmaduct`](rdmaduct) | message transport over RDMA reliable connections or TCP, one API, identical behavior | experimental | [README](rdmaduct/README.md), [architecture](rdmaduct/docs/ARCHITECTURE.md) |
| [`arenakv`](arenakv) | key-value cache for large values: log-structured engine, server, client library | experimental, not for production | [README](arenakv/README.md), [running and querying](arenakv/docs/USAGE.md), [administration](arenakv/docs/OPERATIONS.md), [architecture](arenakv/docs/ARCHITECTURE.md), [protocol](arenakv/docs/PROTOCOL.md) |
| [`kvpace`](kvpace) | load generator: closed and open loop, pipelining, YCSB presets, TLS; RESP, memcached and native protocols; KV-cache scenario | experimental | [README](kvpace/README.md), [usage](kvpace/docs/USAGE.md), [architecture](kvpace/docs/ARCHITECTURE.md) |
| [`benchmarks`](benchmarks) | orchestrator, raw data, figures, tables, methodology, machine-checked claims | | [README](benchmarks/README.md), [running the suite](benchmarks/RUNNING.md), [methodology](benchmarks/METHODOLOGY.md) |
| [`lab`](lab) | six programs that teach RDMA programming, from opening a device to a key-value store over one-sided reads | | [README](lab/README.md) |

The three projects live in one repository because they depend on each other: `arenakv` uses `rdmaduct` as its
transport, `kvpace` drives `arenakv` through its native protocol, and the benchmark suite needs all three. Each
project directory builds and tests on its own and has its own documentation.

## Results at a glance

One machine (Intel Core i9-12900K, Linux 7.0, `performance` governor), server and load generator on separate cores,
loopback TCP, default server configurations, five repetitions per configuration. Every number in these figures is
redrawn from the raw data in [`benchmarks/data`](benchmarks/data) by `make verify`, which fails if a figure, a table
or a tagged statement such as [C1] disagrees with that data. Details and caveats: [Measured results](#measured-results).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="benchmarks/figures/throughput-dark.svg">
  <img alt="Grouped bar charts of throughput for six systems in six panels" src="benchmarks/figures/throughput.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="benchmarks/figures/concurrency-dark.svg">
  <img alt="Line charts of throughput by pipeline depth and by connection count, and p99 latency by connection count" src="benchmarks/figures/concurrency.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="benchmarks/figures/cpu-split-dark.svg">
  <img alt="Stacked bar charts of server CPU time per operation for five servers at pipeline depths 1 and 32, split into kernel and user mode" src="benchmarks/figures/cpu-split.svg">
</picture>

- arenakv over TCP has the highest mean throughput in all six matrix rows [C1]. At 32 pipelined requests per
  connection native arenakv has at least twice the throughput of Valkey, Dragonfly and memcached [C24].
- With one request in flight, more than 80% of the server CPU time of native arenakv is spent in the kernel, in
  system calls and the TCP stack, and more than 70% for every server measured [C28]. RDMA hardware is designed to
  take this kernel part off the request path: the NIC places messages in user memory and a polling server receives
  them without a system call. That effect is expected, not measured: no measurement on RDMA hardware exists yet, and
  the RDMA rows here use Soft-RoCE, a software implementation inside the kernel [C6].

## Why this repository exists

Two workloads that keep growing are poorly served by general-purpose caches. Model inference serving keeps
key-value cache blocks, and HPC pipelines stage intermediate data. Both write values of kilobytes to megabytes
that are rarely modified and read many times, and both often run on clusters with RDMA networks. The established
caches were designed for small objects over kernel TCP, and the established RDMA frameworks are large toolkits
whose behavior differs from one transport to another. Published comparisons are also hard to trust: numbers
differ between load generators as much as between servers, and the raw data is rarely available.

This repository addresses those three gaps with small implementations that can be read in an afternoon, plus
measurements that anyone can re-derive:

1. **A cache shaped for large blocks** (`arenakv`): log-structured memory, so a fixed-size block costs no
   per-object allocation. A vector-compared hash index and second-chance eviction, with a tested guarantee
   that a lookup never returns a stale value. It speaks the memcached and Redis protocols as well as its own
   native protocol over RDMA, so existing clients and tools can use it.
2. **A transport that is the same on RDMA and TCP** (`rdmaduct`): messages, one-sided reads, writes and atomics,
   and event-loop descriptors, with identical ordering, flow control and error semantics on both. Code developed on a laptop runs unchanged on an RDMA cluster.
3. **A measurement tool that removes the tool as a variable** (`kvpace`): one implementation drives all compared
   servers, measures open-loop latency from the intended send time, and models KV-cache reuse. Every published
   number is tied to raw data by a verifier.

None of these replaces the established systems for production use. [`docs/PRIOR_ART.md`](docs/PRIOR_ART.md) states
in detail what each project adds, what it lacks, and when to choose something else.

## Comparison with established systems

Statements about other systems reflect their public documentation at the time of writing.

### arenakv and general-purpose caches

| | arenakv | memcached | Valkey / Redis | Dragonfly |
|---|---|---|---|---|
| Target values | large, mostly immutable blocks | small to medium objects | data structures | data structures |
| Memory layout | log-structured arena per shard | slab allocator | per-object allocation (jemalloc) | per-object allocation (mimalloc) |
| Eviction | second chance in write order | segmented LRU | approximate LRU / LFU policies | cache mode (2Q-based) |
| Network transport | TCP, TLS, RDMA reliable connections | TCP, UDP, Unix sockets, TLS | TCP, Unix sockets, TLS | TCP, Unix sockets, TLS |
| Protocols | memcached text and meta, RESP2, native binary ([PROTOCOL.md](arenakv/docs/PROTOCOL.md)) | text, meta, binary | RESP2, RESP3 | RESP2, RESP3, memcached text |
| Expiry, CAS, conditional stores, increments | yes | yes | yes | yes |
| Data types | strings | strings | strings, lists, hashes, sets, sorted sets, streams | as Valkey |
| Authentication | shared credentials file | SASL, ASCII auth | ACL users | ACL users |
| Persistence | snapshots, append-only log (fsync always, everysec or no), warm restart | warm restart | RDB, AOF | snapshots |
| Replication, cluster | no (client-side consistent hashing) | no (client-side hashing) | yes | replication, cluster mode |
| Hash index | grows from a small size | grows | grows | grows |
| Source size | about 8,600 lines (engine, server, protocols, persistence, client) | large | large | large |
| Production use | not recommended | widely deployed | widely deployed | deployed |

### rdmaduct and RDMA communication frameworks

| | rdmaduct | UCX | libfabric |
|---|---|---|---|
| Programming model | two-sided messages, RMA read/write, atomics | tag matching, streams, active messages, RMA, atomics | messages, tagged messages, RMA, atomics |
| Transports | RDMA reliable connections (verbs + rdma_cm), TCP | many (verbs, shared memory, TCP, GPU) | many providers (verbs, TCP, shared memory, EFA, CXI) |
| Same semantics on every transport | yes; one test suite runs on both | capabilities depend on the transport | capabilities depend on the provider |
| Flow control | credit-based, tested against receiver-not-ready errors | internal | provider-specific |
| One-sided read/write, 64-bit atomics | yes (emulated on TCP) | yes | yes |
| Zero-copy send, pollable descriptors for event loops | yes | yes | yes |
| GPU memory, multi-rail, tag matching | no | yes | yes |
| Source size | about 3,800 lines | large framework | large framework |

### kvpace and key-value load generators

| | kvpace | memtier_benchmark | YCSB |
|---|---|---|---|
| Protocols | RESP, memcached text, arenakv native | RESP, memcached text and binary | many databases through bindings |
| Same measurement code for all servers | yes | yes (two protocols) | per binding |
| Several connections per thread, pipelining | yes | yes | no |
| YCSB core workloads | A, B, C, D, F | no | A to F |
| TLS, authentication | yes | yes | per binding |
| Open-loop arrivals | constant or Poisson schedule | per-connection rate limit | target throughput |
| Latency from the intended send time | yes, reported next to service time | no | yes (intended latency) |
| Multi-host runs | synchronized start, result merging | no | separate client processes |
| KV-cache prefix-chain scenario with reuse accounting | yes | no | no |
| Server CPU time and memory per operation | yes (`--server-pid`) | no | no |
| Hardware and software environment in every result file | yes | no | no |
| Cross-checked against another tool | memtier_benchmark ([validation](kvpace/docs/validation.md)) | | |

## Measured results

All numbers come from one machine with default configurations. The server and the load generator share the machine
and communicate over loopback. The RDMA rows use Soft-RoCE, a software implementation of RDMA; no measurement on
RDMA hardware exists yet. The tables below are generated from the raw data, and `make verify` checks that they,
the figures and every tagged statement ([C1] ...) match it. The complete data, method and caveats are in
[`benchmarks/README.md`](benchmarks/README.md) and [`benchmarks/METHODOLOGY.md`](benchmarks/METHODOLOGY.md).

The throughput figure above is drawn from this table:

<!-- BEGIN GENERATED:matrix-throughput -->
Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

Operations per second (KV-cache rows: requests per second).

| Workload | Server CPUs | arenakv (TCP) | arenakv (Soft-RoCE) | arenakv (memcached protocol) | Valkey | Dragonfly | memcached |
|---|---|---|---|---|---|---|---|
| 64-byte values | 1 | 267,146 (cv 2.1%) | 226,389 (cv 0.9%) | 258,139 (cv 1.7%) | 210,691 (cv 1.5%) | 204,423 (cv 3.4%) | 232,434 (cv 0.9%) |
| 64-byte values | 4 | 723,389 (cv 4.4%) | 244,918 (cv 4.5%) | 805,570 (cv 1.2%) | 477,558 (cv 4.0%) | 529,566 (cv 2.1%) | 725,576 (cv 4.6%) |
| 16 KiB values | 1 | 157,518 (cv 3.9%) | 118,128 (cv 1.9%) | 159,958 (cv 0.8%) | 143,522 (cv 0.4%) | 136,874 (cv 0.6%) | 144,302 (cv 2.9%) |
| 16 KiB values | 4 | 510,367 (cv 3.9%) | 133,901 (cv 3.7%) | 517,462 (cv 6.8%) | 342,350 (cv 4.3%) | 377,634 (cv 6.3%) | 493,253 (cv 3.5%) |
| KV-cache requests | 1 | 2,034 (cv 2.3%) | 1,445 (cv 1.7%) | 1,993 (cv 0.7%) | 1,758 (cv 1.1%) | 1,772 (cv 5.1%) | 1,862 (cv 2.6%) |
| KV-cache requests | 4 | 4,704 (cv 3.2%) | 1,762 (cv 6.0%) | 4,864 (cv 2.4%) | 3,885 (cv 1.6%) | 2,340 (cv 5.0%) | 4,427 (cv 4.3%) |
<!-- END GENERATED:matrix-throughput -->

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="benchmarks/figures/capacity-dark.svg">
  <img alt="Line charts of KV-cache reuse rate and throughput by configured capacity" src="benchmarks/figures/capacity.svg">
</picture>

<!-- BEGIN GENERATED:capacity-reuse -->
Data: `benchmarks/data/2026-10-21-run3` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `cb8f60b7eaa0`, summary digest `c9976f2a8da5b83b`. Mean over repetitions; `cv` is the coefficient of variation.

KV-cache scenario, reuse rate (fraction of needed blocks served from the cache) by configured capacity.

| System | 16 MB | 64 MB | 256 MB | 1 GB | 4 GB |
|---|---|---|---|---|---|
| arenakv (TCP) | 0.182 (cv 1.3%) | 0.429 (cv 0.3%) | 0.642 (cv 0.2%) | 0.822 (cv 0.2%) | 0.904 (cv 0.0%) |
| Valkey | 0.036 (cv 5.5%) | 0.115 (cv 3.0%) | 0.307 (cv 0.9%) | 0.789 (cv 0.2%) | 0.902 (cv 0.0%) |
| Dragonfly | n/a | n/a | 0.408 (cv 0.9%) | 0.685 (cv 0.5%) | 0.902 (cv 0.0%) |
| Dragonfly (eviction flag raised) | n/a | n/a | 0.436 (cv 0.5%) | 0.599 (cv 0.6%) | 0.902 (cv 0.0%) |
| memcached | 0.354 (cv 0.3%) | 0.497 (cv 0.2%) | 0.663 (cv 0.2%) | 0.845 (cv 0.1%) | 0.903 (cv 0.0%) |
<!-- END GENERATED:capacity-reuse -->

The pipelining, connection and CPU-split figures above have their tables in
[`benchmarks/README.md`](benchmarks/README.md#4-pipelining-and-connection-count).

Findings:

- arenakv over TCP, with its native protocol or the memcached protocol, has the highest throughput in every matrix
  row [C1] and the lowest server CPU time per operation in five of six rows [C4]. At one server CPU arenakv over the
  memcached protocol is faster than memcached itself in all three workloads [C3]. The native protocol over TCP is
  faster than Valkey and Dragonfly at four server CPUs [C2].
- With pipelined requests native arenakv leads at depth 32, with at least twice the throughput of Valkey, Dragonfly
  and memcached [C24]; from depth 1 to 32 it scales by a factor above 15, Dragonfly and memcached by less than 6.5
  [C25]. From 16 to 1024 connections arenakv, memcached and Valkey lose less than 5% of their throughput [C26], and
  arenakv over the memcached protocol has the lowest p99 latency at 1024 connections [C27].
- With one request in flight the kernel accounts for more than 80% of the server CPU time of arenakv and for more
  than 70% for every server [C28]; pipelining lowers that share for all of them [C30].
- In the KV-cache workload arenakv reuses more cached blocks than Valkey at small capacities [C11]; memcached reuses
  more up to 1 GB [C12]. Dragonfly's default eviction settings rejected most requests at 256 MB and 1 GB [C14].
- With 64-byte values arenakv has the smallest resident memory of the four servers, because its hash index starts
  small and grows with the data [C16].
- Under open-loop load arenakv sustains 260,000 operations per second, the highest offered rate, while Valkey,
  Dragonfly and memcached stay below 240,000 [C7]. Measuring latency from the intended send time exposes the
  backlogs of the saturated servers, which service-time measurements hide [C9].
- Soft-RoCE results describe software behavior only [C6] [C19] [C20].

## Quick start

Requirements: Linux, gcc 13 or newer (or clang 18 or newer), `make`. For RDMA: `libibverbs-dev`, `librdmacm-dev`
and an RDMA device; Soft-RoCE works ([`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)). Python 3 with matplotlib is
needed only for `make verify`.

```
make                 # build all projects
make test-tcp        # test without RDMA; `make test` includes the RDMA suites when a device is available
make verify          # recompute tables, figures and claims from the raw data and compare

# a cache server and a load generator, over TCP
arenakv/build/x86_64/arenakvd --transport tcp --port 7600 --capacity 1G &
kvpace/build/x86_64/kvpace --driver native --driver-opt transport=tcp --port 7600 --threads 8 --duration 5

# the same over RDMA (192.0.2.10 stands for the IPv4 address of an interface with an RDMA device)
arenakv/build/x86_64/arenakvd --transport rdma --addr 192.0.2.10 --port 7601 --capacity 1G &
kvpace/build/x86_64/kvpace --driver native --driver-opt transport=rdma --host 192.0.2.10 --port 7601 --threads 8
```

## Testing

| Project | Suites | Also run with |
|---|---|---|
| rdmaduct | handshake codec with two million fuzzed inputs, TCP framing attacked through a raw socket, fault injection, RDMA flow control under a slow consumer, abrupt close, connect/disconnect cycles with a descriptor-leak check, examples | ASan + UBSan |
| arenakv | engine model test against a reference map (eviction, overwrite, no stale reads, growing index), expiry, CAS, increments, memcached and RESP protocols with byte-by-byte feeds and one million fuzzed inputs, pipelining and backpressure on real sockets, snapshots, change feed replay, append-only log (rewrites under load, torn and damaged files, SIGKILL with every fsync policy), warm restart, TLS, native protocol over TCP and RDMA, third-party clients (valkey-cli, valkey-benchmark, memtier_benchmark, redis-py) | ASan + UBSan, TSan |
| kvpace | histogram error bounds, workload generators, Zipf generator, all drivers against in-process servers (synchronous and pipelined), TLS, runner timing with multiplexed connections, result merging, KV-cache workload | ASan + UBSan |

All suites pass on x86-64 (gcc 13, Ubuntu 24.04), including the RDMA suites on Soft-RoCE. Continuous integration ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) builds with gcc
and clang on x86-64 and arm64, runs the TCP suites and the sanitizers, compiles the RDMA build, and runs
`make verify`.

## Documentation

| Audience | Start here |
|---|---|
| users of a project | the project's README |
| developers | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md), the architecture document of each project, [`CONTRIBUTING.md`](CONTRIBUTING.md) |
| people checking performance claims | [`benchmarks/README.md`](benchmarks/README.md), [`benchmarks/METHODOLOGY.md`](benchmarks/METHODOLOGY.md) |
| newcomers to RDMA | [`lab/README.md`](lab/README.md) |
| maintainers | [`docs/ROADMAP.md`](docs/ROADMAP.md), [`SECURITY.md`](SECURITY.md) |

## Status and limitations

- The software is experimental and meant for trusted networks. `rdmaduct` neither authenticates peers nor encrypts
  traffic; `arenakvd` offers shared-secret authentication and TLS for its text protocols, but its native protocol is
  never encrypted ([`SECURITY.md`](SECURITY.md)).
- No measurement on RDMA hardware exists yet; see [`docs/ROADMAP.md`](docs/ROADMAP.md).

## License

Apache License 2.0, see [`LICENSE`](LICENSE).
