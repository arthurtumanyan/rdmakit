# arenakv architecture

This document is for people who change or review the code. Usage is in the [README](../README.md).

## Components
| Component | Files | Role |
|---|---|---|
| Engine | `src/akv.c`, `include/akv.h` | sharded in-memory cache with log-structured arenas |
| Tag matching | `src/simd.h` | 16-lane 16-bit comparison: SSE2, NEON, scalar |
| Hash | `src/hash.h` | 64-bit multiply-fold hash, per-instance random seed |
| Protocol | `include/akvproto.h` | request/response messages carried by rdmaduct |
| Server | `src/server.c`, `include/akvd.h`, `tools/arenakvd.c` | accepts rdmaduct connections, one thread per connection |
| Client | `src/client.c`, `include/akvclient.h` | synchronous client used by tests and kvpace |

## Engine
- The key space is split into shards (power of two, default 64) by hash bits 32..41. Each shard has a mutex, an arena and an index.
- **Arena**: circular log of 8-byte-aligned entries (24-byte header, key, value). New entries are appended at the head. When the free space is insufficient, entries are released at the tail. A gap at the end of the arena that is too small for the next entry is filled with a pad entry (only 5 header bytes are written, since gaps can be 8 bytes).
- **Index**: open addressing, 128-byte buckets with 16 slots (16 tags of 16 bits, then 16 offsets of 48 bits). A key may live in either of two adjacent buckets (a window of 32 slots), so lookups examine at most two buckets and deletions need no tombstones. `akv_match16` yields a bitmask of tag matches; the key is compared only for matching tags. Buckets are allocated lazily (anonymous mapping), sized from `capacity / avg_entry`.
- **Overwrite**: an entry of equal total size is overwritten in place; otherwise the new version is appended and the old one becomes dead log space (recognized during eviction because the index no longer points to it).
- **Eviction**: the oldest log entry is examined. Dead entries are skipped. A live entry with the accessed bit (set by every read) is copied to the head once with the bit cleared (second chance, at most 64 consecutive reinsertions); otherwise it is removed from the index. If both buckets of a window are full on insert, the oldest entry of the window is dropped.
- **Guarantee**: a lookup returns the most recent value stored for the key or reports absence; a stale version is never returned. `tests/test_akv.c` checks this against a reference model under heavy eviction, tiny arenas, and index pressure.
- **Memory**: the log never shrinks; resident memory approaches the configured capacity once the log has wrapped.

## Server
- One accept thread; one thread per connection receives a request, executes it on the engine, sends the response, and releases the receive buffer (`src/server.c`). Requests are validated: bad lengths, unknown operations, reserved flags, empty keys yield `AKVP_BADREQ` and the connection stays open; a message shorter than a header closes the connection.
- The response size is bounded by the peer's limit; larger values yield `AKVP_TOOBIG`.
- Waiting: event mode by default. Polling mode spins one core per connection thread and degrades sharply when connection threads outnumber cores (measured: 24k ops/s with a 38 ms p99 for 16 connections on one core, against 191k ops/s in event mode).

## Entry layout

All entries start at multiples of 8 bytes in the arena.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | total entry size including the header (multiple of 8) |
| 4 | 1 | flags: accessed bit, pad marker |
| 6 | 2 | key length |
| 8 | 4 | value length |
| 16 | 8 | key hash |
| 24 | k | key |
| 24 + k | v | value, padded to a multiple of 8 |

A pad entry fills the gap at the end of an arena when the next entry does not fit before the wrap-around; only
its first 5 bytes are written, because the gap may be 8 bytes.

Index slot: a 16-bit tag (never 0 for an occupied slot) and a 48-bit value (arena offset / 8 + 1). Bucket: 16 tags
followed by 16 six-byte values (128 bytes). A key lives in one of the 32 slots of two adjacent buckets.

## Invariants

1. Every index slot refers to a live entry inside the range between the log tail and head; eviction clears the
   slot before the tail passes the entry.
2. A put either overwrites an entry of equal size in place, or appends the new version and then replaces the slot;
   a lookup therefore never sees an older version after a put has returned.
3. The log never contains an entry the index points to beyond the tail: the second-chance copy is made before the
   old position is released, and nested eviction during that copy does not reinsert (it would overwrite the scratch
   buffer).
4. The entry counter equals the number of occupied index slots (tests compare it with a reference model).

## Server

`src/server.c` implements `akvd.h`. One accept thread polls the listener with a 200 ms timeout and reaps finished
connection threads. Each connection thread loops on `duct_recv` (500 ms timeout, used to notice shutdown),
validates the request, calls the engine, sends one response and releases the receive buffer. A request that fails
validation is answered with `AKVP_BADREQ`; a message shorter than a request header closes the connection. The
response size is limited by the peer's `max_msg`; larger values are answered with `AKVP_TOOBIG`.

## Tests

| Test | Purpose |
|---|---|
| `test_primitives` | tag matching equals the scalar reference for all 2^16 lane patterns and random inputs; hash avalanche (about 32 output bits change per input bit) and distribution (chi-square) |
| `test_akv` | basic operations and argument validation; model check against a reference map (exact with ample memory; never-stale under eviction, tiny arenas and index pressure); second chance; same-size overwrite; eight-thread stress with self-describing values |
| `test_server` | protocol round trips, malformed requests, size limits on both sides, concurrent clients, connection accounting, shutdown with connected clients; TCP and, with `DUCT_TEST_ADDR`, RDMA in both waiting modes |

## Not implemented
- Event-loop or worker-per-core serving of many connections (connection threads cost context switches and, with polling, cores).
- Zero-copy GET from the registered arena; one-sided reads.
- Persistence, replication, clustering, authentication.
- Index growth (the index is sized once from `avg_entry`).
