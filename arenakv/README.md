# arenakv

An in-memory key-value **cache** for large values. One server (`arenakvd`) serves the same engine over three
protocols: its native binary protocol over [`rdmaduct`](../rdmaduct) (RDMA or TCP), the **memcached** text and meta
protocol, and **RESP2** (the Redis protocol), so existing Redis and memcached clients work unchanged. The engine keeps
entries in log-structured memory, finds them through a growing hash index whose tags are compared with vector
instructions, and evicts with a second chance.

- Fixed memory budget; the oldest entries are evicted when it is full (or stores are refused with `--no-evict`).
  A lookup returns the most recent value stored for a key or reports a miss; it never returns an older version.
- Per-entry client flags, CAS versions and millisecond expiry; conditional stores (add, replace, append, prepend,
  CAS), increments, touch, flush, multi-key reads.
- Event-loop server: worker threads serve thousands of pipelined connections; native connections use rdmaduct's
  pollable descriptors.
- Access control for all protocols: users with passwords and per-user rights over commands, categories and key
  patterns, in a subset of the Valkey ACL file syntax (`ACL LIST`, `ACL DRYRUN`, `ACL LOAD`); TLS for the text
  protocols and encryption of the native protocol and replication links over TCP and RDMA (optional OpenSSL build); configuration file, log rotation, systemd unit and container image.
- Persistence: snapshots, an append-only log of every change with the fsync policies of Valkey (`always`,
  `everysec`, `no`) and background rewrites, and warm restart (the cache memory survives a restart of the process).
  Every file block is CRC-checked.
- Sharded engine: operations on different shards run in parallel. Tag comparison with SSE2 (x86-64) or NEON
  (aarch64), with a portable fallback.
- Replication over the native transport (TCP or RDMA): replicas continue from their offset after a reconnect or a
  promotion; read-only replicas; `WAIT`; automatic failover by `akv_sentinel` monitors (Redis Sentinel protocol); the Redis Cluster protocol (`MOVED`, `CLUSTER SLOTS`) with live resharding by `akv_reshard` (`ASK`,
  `MIGRATE`, `CLUSTER SETSLOT`).
- Client library (`akvclient.h`) with pipelining, consistent hashing over several servers and failover to a
  promoted replica, and a native driver for [`kvpace`](../kvpace).

**Status:** experimental, not for production use (see [`docs/OPERATIONS.md`](docs/OPERATIONS.md) for what is
missing). Strings only.

## Why this exists

Inference serving and HPC staging both keep values of kilobytes to megabytes. These values are written once,
read many times and evicted under memory pressure, often on clusters with RDMA networks. General-purpose caches
were designed for small objects over kernel TCP: they allocate per object, keep per-object metadata, and cannot
use an RDMA transport. `arenakv` is a cache built for that access pattern:

- **Log-structured arenas.** Entries are appended to a per-shard log, so storing a block costs no allocator call
  and fragmentation cannot build up. Same-size overwrites are done in place.
- **Vector-compared index.** A bucket's 16 tags are compared in one SSE2 or NEON instruction sequence, with a
  scalar fallback.
- **No stale reads.** A lookup returns the latest value stored for a key or a miss, never an older version. A
  model test against a reference map checks this under eviction.
- **Transport-independent server.** The native protocol runs over [`rdmaduct`](../rdmaduct), so the same binary
  serves TCP and RDMA clients, next to standard memcached and Redis clients on the text protocols.
- **Small enough to audit:** about 10,200 lines including the server, three protocols, persistence, access control and the client.

memcached, Valkey and Dragonfly are mature and remain the right default for general caching. The measurements
below show where `arenakv` is in the same range, where it is better, and where it is worse.
[`../docs/PRIOR_ART.md`](../docs/PRIOR_ART.md) discusses when to choose which.

### Comparison

| | arenakv | memcached | Valkey / Redis | Dragonfly |
|---|---|---|---|---|
| Target values | large, mostly immutable blocks | small to medium objects | data structures | data structures |
| Memory layout | log-structured arena per shard | slab allocator | per-object allocation (jemalloc) | per-object allocation (mimalloc) |
| Eviction | second chance in write order | segmented LRU | approximate LRU / LFU policies | cache mode (2Q-based) |
| Network transport | TCP, TLS, RDMA reliable connections (encrypted with `--native-tls`) | TCP, UDP, Unix sockets, TLS | TCP, Unix sockets, TLS | TCP, Unix sockets, TLS |
| Protocols | memcached text and meta, RESP2 and RESP3, native binary ([PROTOCOL.md](docs/PROTOCOL.md)) | text, meta, binary | RESP2, RESP3 | RESP2, RESP3, memcached text |
| Expiry, CAS, conditional stores, increments | yes | yes | yes | yes |
| Transactions, pub/sub | MULTI/EXEC/WATCH, SUBSCRIBE/PSUBSCRIBE/PUBLISH (RESP) | no | yes | yes |
| Data types | strings | strings | strings, lists, hashes, sets, sorted sets, streams | as Valkey |
| Authentication | ACL users with per-user commands, categories and key patterns (subset of the Valkey ACL file syntax) | SASL, ASCII auth | ACL users | ACL users |
| Persistence | snapshots, append-only log (fsync always, everysec or no), warm restart | warm restart | RDB, AOF | snapshots |
| Replication, cluster | asynchronous replication over TCP or RDMA with partial resynchronization; automatic failover by monitors (`akv_sentinel`, Redis Sentinel protocol) or manual; Redis Cluster protocol with live resharding (`akv_reshard`, `ASK`, `MIGRATE`) | no (client-side hashing) | yes | replication, cluster mode |
| Hash index | grows from a small size | grows | grows | grows |
| Production use | not recommended | widely deployed | widely deployed | deployed |

Statements about other systems reflect their public documentation at the time of writing.

### Measured results

Measured with this server version (event-loop workers, growing index, native and memcached protocols; the row
"memcached protocol" is arenakvd serving the memcached text protocol to the same kvpace driver as memcached). One machine with default configurations, server and load generator over loopback; "Soft-RoCE" is software RDMA.
Tables are generated from the raw data in [`../benchmarks/data`](../benchmarks/data). `make verify` at the repository
root checks them and every tagged statement. Method and caveats: [`../benchmarks/METHODOLOGY.md`](../benchmarks/METHODOLOGY.md).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/throughput-dark.svg">
  <img alt="Grouped bar charts of throughput for five systems in six panels" src="../benchmarks/figures/throughput.svg">
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

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/cpu-per-op-dark.svg">
  <img alt="Grouped bar charts of server CPU time per operation for five systems in six panels" src="../benchmarks/figures/cpu-per-op.svg">
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

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/capacity-dark.svg">
  <img alt="Line charts of KV-cache reuse rate and throughput by configured capacity" src="../benchmarks/figures/capacity.svg">
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

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/engine-dark.svg">
  <img alt="Engine thread scaling, tag comparison cost of the vector and scalar versions, and key hash throughput" src="../benchmarks/figures/engine.svg">
</picture>

<!-- BEGIN GENERATED:engine-scale -->
Data: `benchmarks/data/2026-10-23-run5` (790 runs: 754 ok, 26 with operation errors, 0 failed, 10 skipped), commit `9054e10aa2ab`, summary digest `8262d6bae955c711`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,733,462 (cv 1.1%) | 6,392,434 (cv 0.8%) | 10,982,242 (cv 1.5%) | 17,475,078 (cv 2.0%) | 21,333,074 (cv 1.6%) | 22,255,629 (cv 4.8%) |
| 4096-byte values | 1,503,683 (cv 1.7%) | 2,550,371 (cv 0.6%) | 3,919,707 (cv 0.9%) | 5,381,192 (cv 1.0%) | 5,727,927 (cv 1.4%) | 6,014,616 (cv 0.8%) |
<!-- END GENERATED:engine-scale -->

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

Findings:

- Over TCP, with the native or the memcached protocol, arenakv has the highest throughput in every matrix row [C1];
  over the memcached protocol it is faster than memcached at one server CPU in all three workloads [C3]. Over its native protocol it is faster than Valkey and
  Dragonfly at four server CPUs in all three workloads [C2] and uses less server CPU time per operation than both
  [C5].
- At pipeline depth 32 the native protocol leads all servers, with at least twice the throughput of Valkey,
  Dragonfly and memcached [C24]. The native and the memcached protocol cost the same server CPU per operation within
  5% in every matrix row [C29].
- With one request in flight more than 80% of the server CPU time is kernel time (system calls and the TCP stack)
  [C28]; this is the cost the RDMA transport is designed to remove.
- At small capacities arenakv reuses more cached blocks than Valkey [C11]; memcached reuses more up to 1 GB [C12].
- With 64-byte values arenakv has a lower resident memory than Valkey, Dragonfly and memcached [C16]: the hash index
  starts small and grows with the data. (The previous version sized it from the capacity and used more than eight
  times the memory of the others.)
- Under open-loop load arenakv sustains 260,000 operations per second, the highest offered rate, while Valkey,
  Dragonfly and memcached stay below 240,000 [C7].
- Over Soft-RoCE throughput is lower than over TCP in all six rows [C6]. Soft-RoCE is software; no RDMA hardware
  result exists.

## Requirements

Linux, a C compiler with `-std=gnu2x` (gcc 13 or newer, clang 18 or newer), `make`, pthreads, and `../rdmaduct`
(built automatically; `make RDMA=0` builds without the RDMA libraries). For RDMA: `libibverbs-dev`,
`librdmacm-dev` and an RDMA device (Soft-RoCE works for development).

## Build and test

```
make                  # build/<arch>/libarenakv.a, arenakvd, akv_cli, akv_bench, akv_sentinel, akv_reshard
sudo make install     # /usr/local/bin (PREFIX= and DESTDIR= are honored)
make TLS=1            # with TLS listeners (needs libssl-dev; build/<arch>-tls/)
make test             # engine, protocol, network and server tests
make RDMA=0 test      # without RDMA libraries (build/<arch>-tcp/)
make SAN=1 test       # AddressSanitizer + UndefinedBehaviorSanitizer
make TSAN=1 test      # ThreadSanitizer (the test binaries run with ASLR disabled)
make compat           # valkey-cli, valkey-benchmark, memtier_benchmark, redis-py against the server (Docker)
DUCT_TEST_ADDR=<rdma-interface-ip> make test    # also exercises the native protocol over RDMA
```

Test results: all suites pass on x86-64, including the RDMA cases over Soft-RoCE in both waiting modes, under
AddressSanitizer with UndefinedBehaviorSanitizer (also with TLS) and under ThreadSanitizer. `make compat` passes with
valkey-cli, valkey-benchmark, memtier_benchmark (RESP and memcached text) and redis-py. CI runs the TCP cases with gcc
and clang on x86-64 and arm64.

| Suite | Covers |
|---|---|
| `test_akv` | model check against a reference map (exact with ample memory, only the latest version under eviction) with a presized and a growing index; store modes, flags, CAS; expiry, touch, keep-ttl; increments; flush; enumeration; no-evict; eight-thread stress |
| `test_primitives` | vector tag matching equals the scalar reference for all lane combinations; hash avalanche and bucket distribution |
| `test_proto` | memcached and RESP commands with exact replies, also fed one byte at a time; framing errors; authentication; per-user rights in all three protocols, the ACL commands and `ACL LOAD`; extreme expiry values; one million fuzzed inputs |
| `test_acl` | SHA-256 against the FIPS 180-4 vectors, glob patterns, every ACL rule and the order of rules, error messages with line numbers, logins, credentials files of earlier versions |
| `tests/fuzz` | libFuzzer harnesses for the RESP, memcached and native protocols, the snapshot and log loaders, the ACL parser and glob matching (`make fuzz`) |
| `test_net` | pipelining, large values and backpressure on real sockets, 16 concurrent clients, connection limit, idle timeout, snapshots (round trip, expiry across downtime, corruption, truncation), TLS, native revision 2, consistent hashing; with `TLS=1` an encrypted native client, an unencrypted client refused, a replica that verifies its primary and requires client certificates |
| `test_feed` | change feed replayed into a second engine gives the identical state (with eviction, expiry, flush, index-window drops); transaction ids; lock sets under concurrency; warm restart round trip, expiry across the downtime, changed configuration, missing detach, damaged header |
| `test_aof` | append-only log round trip, rewrites while four threads write, automatic rewrites, torn and zero-filled tails, damage before the end, incomplete transactions, durable sequence numbers |
| `durability.py` | `SIGKILL` of a writing server with `always` (no acknowledged write lost) and `everysec` (losses only from the last 0.25 s; the log is written every 10 ms), `SIGKILL` during a rewrite, warm restart with identical contents |
| `test_resp_net` | pub/sub over real sockets with four workers: RESP2 and RESP3 frames, patterns, subscriber mode, PUBSUB, removal on close and RESET, disconnection of a slow subscriber, 8 publishers to 64 subscribers; transactions and MSET on concurrent connections are never seen half applied |
| `replication.py` | a replica of 20,000 keys equal to its primary, read-only refusal, WAIT, ROLE, partial resynchronization after a link restart, full resynchronization after falling behind the backlog, expiry, failover with partial continuation from the promoted replica |
| `test_sentinel` | failover rules (choice of the replica, votes needed, one vote per epoch), the Sentinel replies that clients parse, a vote kept across a restart from the state file |
| `failover.py` | three monitors over a primary and two replicas: automatic failover to the replica with the highest offset after `SIGKILL` (no key lost, the other replica follows, redis-py's Sentinel finds the new primary), the old primary rejoining as a replica, no failover with one of three monitors and failover when they return, `SENTINEL FAILOVER` without loss of acknowledged keys |
| `reshard.py` | `CLUSTER SETSLOT`, `ASK`, `ASKING`, `TRYAGAIN`, `MIGRATE`, `DUMP`/`RESTORE` and their refusals; `akv_reshard` moving 2,000 slots while redis-py `RedisCluster` writers and kvpace `--driver resp-cluster` run (no acknowledged write lost, no error), every node and the replica on the new map; `valkey-cli --cluster reshard` (Docker) moving 200 more slots, the replica learning the owners from the replication stream; map files reread after restarts |
| `cluster.py` | slots against the Redis Cluster CRC16, CLUSTER SLOTS/SHARDS/NODES/INFO, MOVED, CROSSSLOT, READONLY reads on a replica, redis-py RedisCluster, `valkey-cli -c`, kvpace `--driver resp-cluster` |
| `test_server` | native protocol round trips, malformed requests, size limits, concurrent clients, shutdown with clients connected; over TCP, and over RDMA when `DUCT_TEST_ADDR` is set |

## Run the server

```
# drop-in cache for Redis and memcached clients
build/x86_64/arenakvd --port off --resp-port 6379 --memcached-port 11211 --capacity 8G

# native protocol over RDMA, persistence, authentication
build/x86_64/arenakvd --transport rdma --addr 192.0.2.10 --port 7600 --capacity 8G \
    --snapshot /var/lib/arenakv/arenakv.snap --save-interval 300 --auth-file /etc/arenakv/credentials

# everything from a file (contrib/arenakvd.conf)
build/x86_64/arenakvd --config /etc/arenakv/arenakvd.conf
```

```
$ valkey-cli -p 6379 SET block-17 payload EX 3600
OK
$ printf 'get block-17\r\n' | nc -q1 localhost 11211
VALUE block-17 0 7
payload
END
```

| Option | Default | Meaning |
|---|---|---|
| `--port PORT\|off`, `--addr`, `--transport tcp\|rdma\|auto`, `--wait poll\|event` | 7600, all, auto, event | native listener over rdmaduct |
| `--memcached-port`, `--resp-port`, `--text-addr` | off, off, all | text protocol listeners |
| `--tls-memcached-port`, `--tls-resp-port`, `--tls-cert`, `--tls-key`, `--tls-ca` | off | TLS listeners (TLS builds) |
| `--native-tls` | off | encrypt the native listener and replication links (TLS builds) |
| `--capacity SIZE` | 256M | cache memory (suffix K, M, G) |
| `--max-entry SIZE` | 1M | largest key + value |
| `--shards N` | 64 | engine shards, power of two |
| `--avg-entry BYTES` | 0 | presize the index for this mean entry size (0: grow on demand) |
| `--no-evict` | off | refuse stores when full instead of evicting |
| `--threads N` | CPUs of the affinity mask | worker threads |
| `--max-conns N`, `--idle-timeout SEC` | unlimited, never | connection limits |
| `--auth-file FILE` | none | credentials (`user:password` or a password per line), every user with all rights |
| `--aclfile FILE` | none | users with per-user rights; reloaded by `ACL LOAD` |
| `--pubsub-output-limit SIZE` | 32M | pending messages of a subscriber before it is disconnected |
| `--replicaof HOST:PORT`, `--primary-auth USER:PASS` | none | follow a primary (its native port) |
| `--cluster-config FILE`, `--cluster-node-id ID` | none | Redis Cluster protocol; the node rewrites FILE when resharding changes a slot's owner |
| `--snapshot FILE`, `--save-interval SEC`, `--no-save-on-stop` | none, off, save at stop | snapshots |
| `--appendonly DIR`, `--appendfsync always\|everysec\|no` | none, everysec | append-only log |
| `--warm-restart FILE` | none | keep the cache memory (on tmpfs) across restarts |
| `--max-msg BYTES`, `--depth N` | 65536, 16 | native message size and buffers per connection |
| `--huge-pages` | off | advise transparent huge pages for the arenas |
| `--log-level`, `--log-file`, `--stats SEC` | info, stderr, off | logging (SIGHUP reopens the file) |
| `--config FILE` | none | read options from a file; the command line overrides it |

Every option with its accepted values, the configuration file, and every command of the three protocols, with
example sessions, are in [`docs/USAGE.md`](docs/USAGE.md). Deployment (systemd, container, RDMA hosts), security
(authentication, TLS), persistence (backup, restore), monitoring fields, logging and troubleshooting are in
[`docs/OPERATIONS.md`](docs/OPERATIONS.md).

## Use the client library

```c
#include "akvclient.h"

struct duct_opts opts;
duct_opts_default(&opts);
opts.transport = DUCT_TRANSPORT_TCP;      /* or DUCT_TRANSPORT_RDMA */
opts.max_msg = 64 * 1024;

akvc *c;
if (akvc_connect(&c, &opts, "192.0.2.10", "7600") != 0) { /* handle error */ }

struct akvp_ext x = {.ttl_ms = 3600 * 1000, .uflags = 7};
akvc_store(c, AKVP_ADD, "block-17", 8, data, data_len, &x, NULL);   /* AKVP_EXISTS when present */

size_t n;
int rc = akvc_get(c, "block-17", 8, buf, sizeof(buf), &n);
if (rc == AKVP_OK) { /* buf[0..n) holds the value */ }
else if (rc == AKVP_NOTFOUND) { /* miss or expired */ }

/* pipelining: post up to akvc_window(c) requests, then collect the responses in order; the posted
 * requests leave in one batch at the first akvc_next() (or akvc_push()) */
uint32_t id;
akvc_post(c, AKVP_GET, "a", 1, NULL, 0, NULL, &id);
akvc_post(c, AKVP_GET, "b", 1, NULL, 0, NULL, &id);
struct akvc_resp r;
akvc_next(c, &r, 1000);                   /* response to "a" */
akvc_next(c, &r, 1000);                   /* response to "b" */

akvc_close(c);
```

Several servers: `akvc_pool_create(&pool, &opts, servers, n)` and `akvc_pool_pick(pool, key, klen)` (consistent
hashing; removing a server moves only its keys). Return values are an `akvp_status` (0 or positive) when the server
answered and a negative `rdmaduct` error code when the transport failed. Link with `build/<arch>/libarenakv.a`,
`../rdmaduct/build/<arch>/librdmaduct.a`, `-lpthread` and, for RDMA builds, `-lrdmacm -libverbs`.

## Use the engine directly

`include/akv.h` is a self-contained in-process cache (`akv_create`, `akv_store`, `akv_get_meta`, `akv_incr`,
`akv_touch`, `akv_del`, `akv_flush`, `akv_foreach`, `akv_stats_get`); it has no dependency on the server or the
transport.

## Engine guarantees and limits

- Thread safety: all engine functions are thread-safe; an operation locks one shard.
- Keys: 1 to 65535 bytes. Entry (key + value): at most `max_entry` (default 1 MiB) and at most half of a shard arena.
- Memory: the arena is the configured capacity (divided over the shards); the hash index is additional and grows
  with the number of entries. Resident memory approaches the configured capacity once the log has wrapped.
- Expiry uses a monotonic clock; expired entries are never returned.
- Eviction order approximates least recently used: entries are removed in write order unless they were read.
- A store may fail with "too big" (or "no memory" with `--no-evict`); a read after a successful store may still
  miss because of eviction or expiry.

## Layout

| Path | Content |
|---|---|
| `include/` | public headers: `akv.h` (engine), `akvproto.h` (protocol), `akvd.h` (server), `akvclient.h` |
| `src/` | `akv.c` engine, `simd.h`, `hash.h`, `server.c` core, `proto_mc.c`, `proto_resp.c`, `snapshot.c`, `aof.c` append-only log, `client.c` |
| `tools/` | `arenakvd` daemon, `akv_cli` native-protocol client, `akv_bench` micro-benchmarks, `akv_sentinel` failover monitor, `akv_reshard` slot mover |
| `tests/` | engine, protocol, network and server tests; `compat.sh` with third-party clients |
| `contrib/` | example configuration, systemd unit, Dockerfile |
| `docs/` | architecture, native protocol and operations |

## Further reading

[Running and querying](docs/USAGE.md) · [Administration](docs/OPERATIONS.md) · [Architecture](docs/ARCHITECTURE.md) ·
[Protocol](docs/PROTOCOL.md) ·
[Benchmarks](../benchmarks/README.md)
