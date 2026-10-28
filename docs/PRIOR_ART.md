# Why these projects exist, and when to use something else

Mature software exists in every area this repository touches. This document states what each project adds,
what it deliberately does not do, and which established system to choose when the project is not the right
tool. Statements about other systems reflect the authors' understanding at the time of writing; consult each
project's documentation for current behavior.

## Origin

The repository began as a way to learn RDMA programming (`lab/`, six programs from opening a device to a small
key-value store over one-sided reads). The three libraries and tools grew out of questions that came up in the
process:

- How small can a correct message transport over RDMA be, and can it run unchanged on machines without RDMA
  hardware? (`rdmaduct`)
- Do published key-value benchmark numbers differ between servers because of the servers or because of the
  load generators? (`kvpace`)
- What does a cache for large, mostly immutable blocks look like when the transport is a message API that can be
  RDMA, and how does it compare with general-purpose caches on the same hardware? (`arenakv`)

None of them is a replacement for the systems below. Each is a working, tested, measured implementation that
other people can read, run and challenge.

## rdmaduct: message transport over RDMA or TCP

| Established option | What it is | Choose it when |
|---|---|---|
| UCX (UCP/UCT) | communication framework used by MPI and many HPC stacks; multiple transports, tag matching, RMA, atomics, GPU memory | one-sided operations, multi-rail, GPU memory or integration with MPI are required |
| libfabric (OFI) | fabric abstraction with many providers (verbs, CXI/Slingshot, EFA, TCP, shared memory) | the target includes fabrics other than Ethernet or InfiniBand RDMA, or the provider ecosystem is required |
| `rsocket` (librdmacm) | stream sockets API over RDMA, usable with `LD_PRELOAD` | an existing sockets application should use RDMA without code changes |
| eRPC | RPC library for low-latency datacenter networks (DPDK or verbs) | very high request rates with RPC semantics are required |
| MPI | message passing with collectives | the application is an MPI program |
| ZeroMQ, gRPC | general messaging / RPC, mainly over TCP | no RDMA hardware path is needed |

What `rdmaduct` provides that the list above does not claim to: one small C library (about 3,800 lines including
headers) whose behavior is the same on RDMA reliable connections and on TCP for two-sided messages, registered
regions, zero-copy sends, one-sided reads, writes and 64-bit atomics (emulated by the peer's library on TCP) and
pollable descriptors for event loops, including credit-based flow control, ordering of delivery before a close
is reported, and a documented error model. It
depends only on libc for the TCP transport and additionally on libibverbs and librdmacm for RDMA. A program can
therefore be developed on a laptop without RDMA hardware (TCP, or Soft-RoCE), and the transport is tested
for faults and malformed input (see `rdmaduct/docs/ARCHITECTURE.md`).

What it does not do: multiple NICs or rails, GPU memory, tag matching, collectives. Encryption is optional
(`TLS=1`) and covers two-sided messages only; one-sided operations are not available on encrypted endpoints,
because the peer CPU does not take part in them.
On TCP, one-sided operations progress only while the target application calls into the library.

## kvpace: key-value benchmark

| Established option | What it is | Choose it when |
|---|---|---|
| `memtier_benchmark` | load generator for Redis and memcached protocols | the de-facto standard tool for those servers is wanted |
| `redis-benchmark` | simple load generator shipped with Redis/Valkey | quick checks |
| YCSB | workloads A to F across many databases | results must be comparable to published YCSB numbers, or database-style workloads are needed |
| `mutilate`, `wrk2` | latency-oriented load generators (open-loop, coordinated-omission aware) | distributed load generation or HTTP load is needed |
| CacheBench (CacheLib) | benchmark for the CacheLib caching library | CacheLib itself is being evaluated |

What `kvpace` adds:

- One tool that drives the RESP protocol, the memcached text protocol and the native protocol of `arenakv` with
  identical workload definitions and identical measurement code, so that a difference between servers is not a
  difference between tools. Its results were cross-checked against `memtier_benchmark`
  (`kvpace/docs/validation.md`).
- Open-loop load with latency measured from the intended send time, reported next to the pure service time.
- A KV-cache request model (prefix chains of shared blocks, reuse accounting) that measures how much recomputation a
  cache saves, not only operations per second.
- Result files that carry the environment of the run, which the benchmark suite in `benchmarks/` uses to make
  every published number checkable.

It also covers the client features of `memtier_benchmark` that matter for these servers: several connections per
thread with pipelining, operation mixes, key distributions, value-size lists, authentication, TLS and HdrHistogram
output, plus the YCSB core workloads A, B, C, D and F as presets.

What it does not do: coordinated distributed load generation (several hosts start together with `--start-at` and
their results are combined with `kvpace merge`, but no controller drives them), YCSB workload E (range scans),
cluster-aware clients, or the memcached binary protocol.

## arenakv: key-value cache for large blocks

| Established option | What it is | Choose it when |
|---|---|---|
| memcached | slab-allocated in-memory cache | general-purpose caching with a mature ecosystem |
| Valkey, Redis | in-memory data structure server | data structures, persistence, replication or scripting are needed |
| Dragonfly | multi-threaded, shared-nothing Redis-protocol server | Redis compatibility with better multi-core scaling is needed |
| Segcache (Pelikan), CacheLib | research and production caches with log- or segment-structured storage | the techniques of those designs are needed in a maintained library (Segcache: NSDI 2021; CacheLib: OSDI 2020) |
| RAMCloud, MICA | research systems that introduced log-structured memory and partitioned hash tables (FAST 2014, NSDI 2014) | the original designs and papers are of interest |
| Mooncake Store, LMCache and similar | KV-cache storage integrated with inference serving engines | connectors to a serving engine are required |

What `arenakv` provides: a small cache (about 10,200 lines including the server, three protocols, persistence, access control and the
client) that combines a log-structured arena, a growing bucketed hash index whose tags are compared with vector
instructions (SSE2, NEON), and second-chance eviction, with a tested guarantee that a lookup never returns a stale
value. The server speaks the memcached text and meta protocol and RESP2, so existing clients work, plus a native
protocol whose transport can be RDMA; it offers expiry, CAS, conditional stores, increments, authentication, TLS,
snapshots, an append-only log and warm restart. The measurements in `benchmarks/` compare it with Valkey, Dragonfly and memcached on the
same machine.

What the measurements show so far (data and caveats in `benchmarks/README.md`): over TCP on the measured machine
the store has the highest throughput in every row of the matrix, over its native protocol or the memcached protocol
[C1]; over the memcached protocol it is faster than memcached at one server CPU [C3], and over its native protocol it
is faster than Valkey and Dragonfly at four server CPUs [C2]. The margins per row are in the tables of
`benchmarks/README.md`; all were measured on one host over loopback. With one request in flight more than 80% of its
server CPU time is spent in the kernel [C28]. Its design targets a transport without kernel TCP overhead, and that
path has been exercised only on Soft-RoCE, a software implementation [C6], so no claim about RDMA hardware is made.

What it does not do: resharding, failover of Redis Cluster slot owners, data structures beyond byte-string keys and values,
scripting, the memcached binary protocol, integration with inference serving engines. It is not
recommended for production use.

## How to use this repository responsibly

- Treat measurements as evidence about one machine and one configuration, not as rankings.
- Prefer the established systems above for production unless a specific, measured requirement favors a
  component of this repository.
- Challenge the numbers: the data, the scripts and the claim checks are in the repository for that purpose
  (`benchmarks/METHODOLOGY.md`).
