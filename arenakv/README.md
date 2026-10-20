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
- Authentication for all protocols, TLS for the text protocols (optional OpenSSL build), snapshots to disk with
  CRC-checked blocks, configuration file, log rotation, systemd unit and container image.
- Sharded engine: operations on different shards run in parallel. Tag comparison with SSE2 (x86-64) or NEON
  (aarch64), with a portable fallback.
- Client library (`akvclient.h`) with pipelining and consistent hashing over several servers, and a native driver
  for [`kvpace`](../kvpace).

**Status:** experimental, not for production use. No replication; strings only.

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
- **Small enough to audit:** about 6,500 lines including the server, three protocols, persistence and the client.

memcached, Valkey and Dragonfly are mature and remain the right default for general caching. The measurements
below show where `arenakv` is in the same range, where it is better, and where it is worse.
[`../docs/PRIOR_ART.md`](../docs/PRIOR_ART.md) discusses when to choose which.

### Comparison

| | arenakv | memcached | Valkey / Redis | Dragonfly |
|---|---|---|---|---|
| Target values | large, mostly immutable blocks | small to medium objects | data structures | data structures |
| Memory layout | log-structured arena per shard | slab allocator | per-object allocation (jemalloc) | per-object allocation (mimalloc) |
| Eviction | second chance in write order | segmented LRU | approximate LRU / LFU policies | cache mode (2Q-based) |
| Network transport | TCP, TLS, RDMA reliable connections | TCP, UDP, Unix sockets, TLS | TCP, Unix sockets, TLS | TCP, TLS |
| Protocols | memcached text and meta, RESP2, native binary ([PROTOCOL.md](docs/PROTOCOL.md)) | text, meta, binary | RESP2, RESP3 | RESP2, RESP3, memcached text |
| Expiry, CAS, conditional stores, increments | yes | yes | yes | yes |
| Data types | strings | strings | strings, lists, hashes, sets, sorted sets, streams | as Valkey |
| Authentication | shared credentials file | SASL, ASCII auth | ACL users | ACL users |
| Persistence | snapshots (CRC-checked, atomic rename) | warm restart | RDB, AOF | snapshots |
| Replication, cluster | no (client-side consistent hashing) | no (client-side hashing) | yes | replication |
| Hash index | grows from a small size | grows | grows | grows |
| Production use | not recommended | widely deployed | widely deployed | deployed |

Statements about other systems reflect their public documentation at the time of writing.

### Measured results

These numbers were measured with the previous server version (one thread per connection, index sized from
`--avg-entry`, native protocol only) and are kept until the benchmark suite is repeated with this version.
One machine with default configurations, server and load generator over loopback; "Soft-RoCE" is software RDMA.
Tables are generated from the raw data in [`../benchmarks/data`](../benchmarks/data). `make verify` at the repository
root checks them and every tagged statement. Method and caveats: [`../benchmarks/METHODOLOGY.md`](../benchmarks/METHODOLOGY.md).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/throughput-dark.svg">
  <img alt="Grouped bar charts of throughput for five systems in six panels" src="../benchmarks/figures/throughput.svg">
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

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/cpu-per-op-dark.svg">
  <img alt="Grouped bar charts of server CPU time per operation for five systems in six panels" src="../benchmarks/figures/cpu-per-op.svg">
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

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/capacity-dark.svg">
  <img alt="Line charts of KV-cache reuse rate and throughput by configured capacity" src="../benchmarks/figures/capacity.svg">
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

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/engine-dark.svg">
  <img alt="Engine thread scaling, tag comparison cost of the vector and scalar versions, and key hash throughput" src="../benchmarks/figures/engine.svg">
</picture>

<!-- BEGIN GENERATED:engine-scale -->
Data: `benchmarks/data/2026-10-20-run1` (353 runs: 336 ok, 11 with operation errors, 0 failed, 6 skipped), commit `fc0dc39b5653`, summary digest `aaaad406e27486af`. Mean over repetitions; `cv` is the coefficient of variation.

Engine operations per second (90% GET, uniform keys, key space preloaded, no eviction).

| Value size | 1 threads | 2 threads | 4 threads | 8 threads | 12 threads | 16 threads |
|---|---|---|---|---|---|---|
| 64-byte values | 3,728,244 (cv 7.2%) | 6,574,471 (cv 3.7%) | 11,454,697 (cv 1.4%) | 18,568,392 (cv 1.3%) | 22,585,490 (cv 3.9%) | 25,142,175 (cv 2.7%) |
| 4096-byte values | 1,526,878 (cv 4.2%) | 2,524,133 (cv 7.6%) | 3,982,740 (cv 1.9%) | 5,304,605 (cv 4.2%) | 5,715,945 (cv 3.7%) | 5,872,373 (cv 3.5%) |
<!-- END GENERATED:engine-scale -->

Findings:

- At four server CPUs arenakv over TCP is faster than Valkey and Dragonfly in all three workloads [C2] and uses less
  server CPU time per operation than both [C5]. memcached has the highest throughput in five of six rows [C1].
- At one server CPU arenakv is slower than all three on 64-byte values [C3].
- At small capacities arenakv reuses more cached blocks than Valkey [C11]; memcached is at least as good [C12].
- With 64-byte values arenakv used more than eight times the resident memory of the others [C16], because the hash
  index was sized from the configured capacity and `--avg-entry` and sparse keys touched its pages. The index now
  starts small and grows with the data (200,000 entries: 4 MiB of index in `tests/test_akv.c`); the benchmark has not
  been repeated yet.
- Under open-loop load arenakv saturates earlier than the other three servers [C7].
- Over Soft-RoCE throughput is lower than over TCP in five of six rows [C6]. Soft-RoCE is software; no RDMA
  hardware result exists.

## Requirements

Linux, a C compiler with `-std=gnu2x` (gcc 13 or newer, clang 18 or newer), `make`, pthreads, and `../rdmaduct`
(built automatically; `make RDMA=0` builds without the RDMA libraries). For RDMA: `libibverbs-dev`,
`librdmacm-dev` and an RDMA device (Soft-RoCE works for development).

## Build and test

```
make                  # build/<arch>/libarenakv.a, arenakvd, akv_bench
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
| `test_proto` | memcached and RESP commands with exact replies, also fed one byte at a time; framing errors; authentication; one million fuzzed inputs |
| `test_net` | pipelining, large values and backpressure on real sockets, 16 concurrent clients, connection limit, idle timeout, snapshots (round trip, corruption, truncation), TLS, native revision 2, consistent hashing |
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
| `--capacity SIZE` | 256M | cache memory (suffix K, M, G) |
| `--max-entry SIZE` | 1M | largest key + value |
| `--shards N` | 64 | engine shards, power of two |
| `--avg-entry BYTES` | 0 | presize the index for this mean entry size (0: grow on demand) |
| `--no-evict` | off | refuse stores when full instead of evicting |
| `--threads N` | online CPUs | worker threads |
| `--max-conns N`, `--idle-timeout SEC` | unlimited, never | connection limits |
| `--auth-file FILE` | none | credentials (`user:password` or a password per line) |
| `--snapshot FILE`, `--save-interval SEC`, `--no-save-on-stop` | none, off, save at stop | persistence |
| `--max-msg BYTES`, `--depth N` | 65536, 16 | native message size and buffers per connection |
| `--huge-pages` | off | advise transparent huge pages for the arenas |
| `--log-level`, `--log-file`, `--stats SEC` | info, stderr, off | logging (SIGHUP reopens the file) |
| `--config FILE` | none | read options from a file; the command line overrides it |

Operations guide (security, persistence, monitoring, deployment, compatibility notes):
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

/* pipelining: post up to akvc_window(c) requests, then collect the responses in order */
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
| `src/` | `akv.c` engine, `simd.h`, `hash.h`, `server.c` core, `proto_mc.c`, `proto_resp.c`, `snapshot.c`, `client.c` |
| `tools/` | `arenakvd` daemon, `akv_bench` micro-benchmarks |
| `tests/` | engine, protocol, network and server tests; `compat.sh` with third-party clients |
| `contrib/` | example configuration, systemd unit, Dockerfile |
| `docs/` | architecture, native protocol and operations |

## Further reading

[Architecture](docs/ARCHITECTURE.md) · [Protocol](docs/PROTOCOL.md) · [Operations](docs/OPERATIONS.md) ·
[Benchmarks](../benchmarks/README.md)
