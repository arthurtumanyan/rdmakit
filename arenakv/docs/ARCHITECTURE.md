# arenakv architecture

This document is for people who change or review the code. Usage is in [USAGE.md](USAGE.md), administration in
[OPERATIONS.md](OPERATIONS.md), the native protocol in [PROTOCOL.md](PROTOCOL.md).

## Components

| Component | Files | Role |
|---|---|---|
| Engine | `src/akv.c`, `include/akv.h` | sharded in-memory cache with log-structured arenas, expiry, CAS, growing index |
| Tag matching | `src/simd.h` | 16-lane 16-bit comparison: SSE2, NEON, scalar |
| Hash | `src/hash.h` | 64-bit multiply-fold hash, per-instance random seed |
| Native protocol | `include/akvproto.h` | request/response messages carried by rdmaduct (revision 2) |
| Server core | `src/server.c`, `include/akvd.h`, `src/srv.h` | listeners, accept thread, epoll workers, native requests, credentials, logging, snapshot scheduling, TLS |
| memcached protocol | `src/proto_mc.c` | text and meta commands over byte buffers |
| RESP protocol | `src/proto_resp.c` | RESP2 arrays and inline commands over byte buffers |
| Persistence | `src/snapshot.c` | snapshot writer and loader, CRC32C |
| Client | `src/client.c`, `include/akvclient.h` | native client with pipelining and a consistent-hashing pool |
| Daemon | `tools/arenakvd.c` | options, configuration file, signals |

## Engine

- The key space is split into shards (power of two, default 64) by hash bits 32..41. Each shard has a mutex, an
  arena and an index.
- **Arena**: circular log of 8-byte-aligned entries (40-byte header, key, value). New entries are appended at the
  head; when the free space is insufficient, entries are released at the tail. A gap at the end of the arena that is
  too small for the next entry is filled with a pad entry (only 5 header bytes are written, since gaps can be 8
  bytes).
- **Index**: open addressing, 128-byte buckets with 16 slots (16 tags of 16 bits, then 16 offsets of 48 bits). A key
  may live in either of two adjacent buckets (a window of 32 slots), so lookups examine at most two buckets and
  deletions need no tombstones. `akv_match16` yields a bitmask of tag matches; the key is compared only for matching
  tags.
- **Index growth**: a shard starts with 16 buckets (2 KiB) unless `avg_entry` presizes it. The index doubles when
  entries exceed 75% of the slots, or when a key finds its window full, up to a bound of one slot per smallest
  possible entry. Doubling allocates the new table, re-inserts every slot by its stored hash and frees the old table,
  all under the shard lock (the shard is unavailable during the rehash). Only at the bound does a full window drop its
  oldest live entry (an index eviction).
- **Store modes** (`akv_store`): SET, ADD, REPLACE, APPEND, PREPEND, CAS, with client flags and a time to live.
  APPEND and PREPEND copy the old value first, because allocating the new version may evict or move the old one.
- **Overwrite**: an entry of equal total size is overwritten in place (value, flags, CAS, expiry); otherwise the new
  version is appended and the old one becomes dead log space (recognized during eviction because the index no longer
  points to it).
- **Expiry**: each entry stores an expiry time in milliseconds of the engine clock (CLOCK_MONOTONIC_COARSE since
  creation, plus 1; 0 = none). A lookup that meets an expired entry clears its slot; eviction reclaims expired entries
  without counting them as evictions. The coarse clock is read from the vDSO without the timestamp counter; its
  resolution is one scheduler tick (`clock_getres`, 1 to 10 ms), so an entry can outlive its time to live by up to one
  tick. memcached checks expiry against a clock of one-second resolution.
- **Prefetch**: `akv_prefetch()` loads the first index bucket of a key without the lock. The server calls it for a
  group of up to 8 pipelined native requests before executing them, so that their index cache misses overlap.
- **CAS**: per-shard counter shifted left by 10 bits, ORed with the shard number; versions are unique in the engine.
- **Increments** (`akv_incr`): memcached semantics (unsigned, wrap on increment, floor at 0, absent key is an error)
  or RESP semantics (signed, absent key counts as 0, overflow error); the result is stored as decimal text with flags
  and expiry kept.
- **Eviction**: the oldest log entry is examined. Dead entries are skipped; expired ones are reclaimed. A live entry
  with the accessed bit (set by every read) is copied to the head once with the bit cleared (second chance, at most 64
  consecutive reinsertions); otherwise it is removed from the index. With `no_evict` a live, unexpired entry at the
  tail makes the store fail with `-ENOMEM`.
- **Enumeration** (`akv_foreach`): each shard's live entries are copied while its lock is held; the callback runs
  without locks. Used by snapshots, `KEYS` and `SCAN`.
- **Flush**: resets head and tail and returns arena and index pages with `MADV_DONTNEED` (private anonymous memory
  reads back as zero, which empties the index).
- **Guarantee**: a lookup returns the most recent value stored for the key or reports absence; a stale version is
  never returned. `tests/test_akv.c` checks this against a reference model under heavy eviction, tiny arenas, index
  pressure and a growing index.

## Entry layout

All entries start at multiples of 8 bytes in the arena.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | total entry size including the header (multiple of 8) |
| 4 | 1 | flags: accessed bit, pad marker |
| 6 | 2 | key length |
| 8 | 4 | value length |
| 12 | 4 | client flags |
| 16 | 8 | key hash |
| 24 | 8 | CAS version |
| 32 | 8 | expiry, milliseconds of the engine clock (0 = none) |
| 40 | k | key |
| 40 + k | v | value, padded to a multiple of 8 |

Index slot: a 16-bit tag (never 0 for an occupied slot) and a 48-bit value (arena offset / 8 + 1).

## Invariants

1. Every index slot refers to an entry inside the range between the log tail and head; eviction clears the slot
   before the tail passes the entry.
2. A store either overwrites an entry of equal size in place, or appends the new version and then replaces the slot;
   a lookup therefore never sees an older version after a store has returned.
3. The second-chance copy is made before the old position is released, and nested eviction during that copy does not
   reinsert (it would overwrite the scratch buffer).
4. The entry counter equals the number of occupied index slots (tests compare it with a reference model), including
   expired entries not yet reclaimed.
5. Index growth re-inserts every occupied slot; a rehash never changes which entry a key refers to.

## Server

Threads (`src/server.c`):

- **accept**: `poll()` on all listeners (native rdmaduct listener through `duct_listener_fd`, memcached, RESP, TLS
  variants). New connections are counted, checked against `max_conns`, and queued round robin to a worker, whose
  eventfd wakes it.
- **workers**: one epoll set each. Native connections are driven through direct waiting (`duct_ep_direct_fd`, on
  TCP the socket itself) and `duct_ep_arm`; the registered events follow `duct_ep_wait_events`. The worker drains
  requests with `duct_recv(..., 0)` in groups of up to 8 (their index buckets are prefetched first), executes them
  into a per-worker batch area (1 MiB, or `max_msg` if larger) and sends up to 64 responses with one
  `duct_send_some()`, which on TCP accepts a batch of small responses in full (one `send()`). Responses that are not
  accepted are queued and the connection stops reading until the queue is empty, so a client that does not read
  cannot make the server buffer without bound. Text connections use non-blocking sockets (or OpenSSL with
  non-blocking BIOs); input accumulates until the protocol module consumes complete commands, replies accumulate in an
  output buffer, and reading stops while more than 4 MiB of replies are pending.
- **fairness**: a connection is served for at most 64 requests or reads per turn. A connection with work left that
  raises no descriptor event (messages already received by rdmaduct, complete commands in its buffer, TLS-buffered
  input) goes on the worker's ready list and is served in the next iteration, which polls epoll without waiting. A
  connection closed while on the ready list is released when the list is processed.
- **saver**: periodic snapshots; `BGSAVE` runs on its own thread; a mutex serializes snapshots.

Protocol modules (`proto_mc.c`, `proto_resp.c`) are functions over byte buffers: `*_execute(server, session,
input, length, output)` returns the number of bytes consumed and leaves an incomplete command unconsumed. They never
touch sockets, which is what `tests/test_proto.c` relies on. Per-thread buffers (values read for replies, RESP
argument vectors) are freed when a worker exits.

Credentials are loaded once at start; `akvd_auth_check` compares every entry in time that depends only on the
lengths.

## Client

- Synchronous calls are implemented as one `akvc_post` followed by `akvc_next`.
- Pipelining: `akvc_post` refuses more than `akvc_window()` = `min(recv_depth, send_depth) - 1` outstanding requests,
  which keeps the client from blocking on credits while the server waits for the client to read responses.
- Posted requests are packed into a staging area (2 x `max_msg`) and sent with one `duct_send_batch()` by
  `akvc_push`, `akvc_next` or `akvc_arm`, or when 32 requests or `max_msg` bytes are staged.
- Event loops wait on `akvc_direct_fd()` for the events of `akvc_wait_events()` (rdmaduct direct waiting), or on
  `akvc_fd()` for `EPOLLIN`.
- `akvc_pool`: 160 points per server on a 64-bit ring (hash of `"host:port-i"` with a fixed seed), binary search for
  the first point at or after the key's hash. Removing a server moves only the keys it owned (tested).

## Tests

| Test | Purpose |
|---|---|
| `test_primitives` | tag matching equals the scalar reference for all 2^16 lane patterns and random inputs; hash avalanche and distribution |
| `test_akv` | basic operations; model check against a reference map (exact with ample memory; never-stale under eviction, tiny arenas, index pressure), with a presized and with a growing index; second chance; same-size overwrite; eight-thread stress; index growth to 200,000 entries; store modes, flags and CAS; expiry, touch, persist, keep-ttl, get-and-touch; increments in both semantics; flush; enumeration; stores without eviction |
| `test_proto` | memcached and RESP commands with exact replies, each script also fed one byte at a time, framing errors, oversized data blocks, authentication in all forms, one million fuzzed inputs |
| `test_net` | real connections: 2000 pipelined commands per protocol, 512 KiB values, 64 MiB of replies under backpressure, 16 concurrent clients, connection limit, idle timeout, snapshot round trip with flags and expiry, corrupt and truncated snapshots, BGSAVE, TLS with certificate verification (TLS builds), native revision 2 operations, pipelining window, native authentication, consistent hashing over four servers |
| `test_server` | native protocol round trips, malformed requests, size limits, concurrent clients, connection accounting, shutdown with connected clients; TCP and, with `DUCT_TEST_ADDR`, RDMA in both waiting modes |
| `tests/compat.sh` (`make compat`) | valkey-cli, valkey-benchmark (pipelined), memtier_benchmark (RESP and memcached text), redis-py against the daemon; needs Docker |

All tests run under AddressSanitizer with UndefinedBehaviorSanitizer (`make SAN=1 test`, also with `TLS=1`) and the
engine and server tests under ThreadSanitizer (`make TSAN=1 test`).

## Not implemented

- Automatic failover, live resharding, and gossip between cluster nodes (the slot map is a file).
- Data types other than strings (lists, hashes, sets, sorted sets, streams), scripting.
- memcached binary protocol.
- Zero-copy GET from the registered arena and a one-sided GET path (the transport provides the operations; the
  engine's entry layout and locking would have to support remote readers).
- Incremental index rehashing (growth pauses one shard for the duration of its rehash).
