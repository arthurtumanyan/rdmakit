# Roadmap

Status of the work and what remains, with the measurement gate that decides whether the key-value store is
developed further. Dates refer to the history of this repository.

## Status

| Area | Done | Open |
|---|---|---|
| rdmaduct | RDMA and TCP transports, credit-based flow control, batching, event-driven waiting, graceful close, pollable descriptors, registered regions, zero-copy send, one-sided read/write and atomics (emulated on TCP), fault injection and fuzzing of handshake, framing and control frames | GPU memory, multiple devices, TCP progress thread, coverage-guided fuzzing |
| kvpace | closed and open loop; several connections per thread with pipelining; operation mixes and YCSB workloads A, B, C, D, F; five key distributions and value-size lists; KV-cache scenario; RESP, memcached and native drivers with authentication and TLS; CSV and HdrHistogram output; synchronized multi-host start and result merging; server CPU sampling | trace replay, cluster-aware clients, coordinated distributed generation |
| arenakv | engine with expiry, CAS, conditional stores, increments and a growing index; event-loop server; memcached text and meta, RESP2 and native protocols; authentication, TLS, snapshots; client with pipelining and consistent hashing; tests under three sanitizers and with third-party clients | zero-copy GET from registered memory, one-sided GET, incremental rehashing, replication |
| benchmarks | orchestrator, environment capture, figures, tables, claim verification | runs on RDMA hardware, runs with the `performance` CPU governor, cross-host runs |

## Gate for the store

The store is developed further only if it shows a clear, repeatable advantage over the established systems under at
least one KV-cache scenario, on hardware where the transport can show it. Over TCP on the measured machine it is in
the same range as the established servers (see [`benchmarks/README.md`](../benchmarks/README.md), [C1] [C2] [C3]); an
advantage was not expected there. The remaining candidates are a transport without kernel TCP overhead, server CPU per large block, and
memory efficiency. They need RDMA hardware to be evaluated; Soft-RoCE results are not evidence.

## Next steps

1. Repeat the benchmark suite on RDMA hardware (ConnectX class), with the `performance` governor.
2. Worker-per-core serving model for `arenakvd` with polling completion queues.
3. Zero-copy GET (send from the registered arena) and a one-sided GET path with checksummed buckets, measured against
   the RPC path.
4. Integration points for inference serving engines, only if steps 1 to 3 show an advantage.
