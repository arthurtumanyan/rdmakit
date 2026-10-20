# Architecture overview

The repository contains three libraries or tools and one measurement suite that connect them.

```
                 +-------------------+          +--------------------+
                 |     kvpace       |          |     benchmarks/    |
                 | load generator    |<---------| run.py, report.py  |
                 | RESP / memcached  |          | data, figures,     |
                 | / native drivers  |          | claim checks       |
                 +---------+---------+          +--------------------+
                           | native protocol (akvproto)
                           v
 +-----------------+   +---+--------+   messages   +----------------+
 | arenakv engine  |<--+  arenakvd  |<------------>|    rdmaduct      |
 | (akv.c)         |   | (server.c) |   TCP or     | RDMA RC or TCP |
 +-----------------+   +------------+   RDMA       +----------------+
```

| Component | Depends on | Provides |
|---|---|---|
| `rdmaduct` | libc; libibverbs and librdmacm for RDMA | message API with identical semantics on RDMA and TCP |
| `arenakv` engine | libc, pthreads | in-process cache (`akv.h`) |
| `arenakv` server and client | engine, `rdmaduct` | `arenakvd`, `akvclient.h`, protocol `akvproto.h` |
| `kvpace` | libc, pthreads; optional: `arenakv` client and `rdmaduct` | load generation and measurement |
| `benchmarks` | Python 3, matplotlib, Docker (third-party servers), the three projects | orchestration, reports, verification |
| `lab` | libibverbs, librdmacm | teaching programs |

Dependencies point downward only: `rdmaduct` knows nothing about the cache; the engine knows nothing about the
transport; `kvpace` links the store's client library only for the optional native driver.

## Data flow of a request

1. `kvpace` (or any client) builds a request (`akvproto.h`) and sends it through `rdmaduct` (TCP stream framing or an
   RDMA SEND with immediate data).
2. `arenakvd` receives it on a connection thread, validates it, calls the engine (`akv_get`, `akv_put`, `akv_del`)
   and sends one response message.
3. The engine hashes the key, selects a shard, locks it, finds the entry through the tag-matched index, and copies the
   value out (GET) or appends or overwrites an entry (PUT) in the shard's log-structured arena.

## Design decisions

| Decision | Reason | Cost |
|---|---|---|
| One message API over RDMA and TCP | develop and test on machines without RDMA; compare transports under the same server | the API is limited to what both provide (no one-sided operations) |
| Credit-based flow control with credits written into a remote word | no receive buffer is reserved for control messages; receiver-not-ready cannot occur | an extra registered page with remote write access; event mode polls it every 200 microseconds |
| Log-structured arenas with in-place same-size overwrite | fast appends, no per-entry allocator, no fragmentation inside a shard | dead space until the log wraps for variable-size overwrites; resident memory approaches the capacity |
| Index of 128-byte buckets with 16-bit tags compared by vector instructions | one or two cache lines per lookup | the index is sized once from an expected entry size |
| Second-chance eviction | approximates LRU without per-read list updates | entries read once survive one more pass |
| Thread per connection in the server | simple, no event loop to maintain | not suited to thousands of connections; polling mode needs a core per connection |
| Raw data plus generated reports plus claim checks | published numbers can be re-derived and challenged | more infrastructure around every measurement |

Detailed designs: [`rdmaduct`](../rdmaduct/docs/ARCHITECTURE.md), [`arenakv`](../arenakv/docs/ARCHITECTURE.md),
[`kvpace`](../kvpace/docs/ARCHITECTURE.md), [`benchmarks`](../benchmarks/METHODOLOGY.md).
