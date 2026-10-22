# Roadmap

Status of the work and what remains, with the measurement gate that decides whether the key-value store is
developed further. Dates refer to the history of this repository.

## Status

| Area | Done | Open |
|---|---|---|
| rdmaduct | RDMA and TCP transports, credit-based flow control, batching, event-driven waiting, graceful close, pollable descriptors, registered regions, zero-copy send, one-sided read/write and atomics (emulated on TCP), fault injection and fuzzing of handshake, framing and control frames, coverage-guided fuzzing of the handshake, the TCP stream and encrypted records, optional encryption (TLS 1.3 handshake, AES-256-GCM per message) | GPU memory, multiple devices, TCP progress thread |
| kvpace | closed and open loop; several connections per thread with pipelining; operation mixes and YCSB workloads A, B, C, D, F; five key distributions and value-size lists; KV-cache scenario; RESP, memcached and native drivers with authentication and TLS; CSV and HdrHistogram output; synchronized multi-host start and result merging; server CPU sampling; Redis Cluster driver | trace replay, coordinated distributed generation |
| arenakv | engine with expiry, CAS, conditional stores, increments and a growing index; event-loop server; memcached text and meta, RESP2 and native protocols; users with per-user rights (ACL), RESP3, transactions, pub/sub, replication with manual failover, Redis Cluster protocol, TLS, encrypted native protocol and replication links, snapshots, append-only log, warm restart; fuzzing, static analysis and hardened builds in CI; client with pipelining and consistent hashing; tests under three sanitizers and with third-party clients | zero-copy GET from registered memory, one-sided GET, incremental rehashing, automatic failover, resharding |
| benchmarks | orchestrator, environment capture, figures, tables, claim verification, kernel and user split of the server CPU time, a run with the `performance` CPU governor | runs on RDMA hardware, cross-host runs |

## Gate for the store

The store is developed further only if it shows a clear, repeatable advantage over the established systems under at
least one KV-cache scenario, on hardware where the transport can show it. Over TCP on the measured machine arenakv has
the highest throughput in every matrix row, is faster than memcached over the same protocol at one server CPU, and is
ahead of Valkey and Dragonfly at four (see [`benchmarks/README.md`](../benchmarks/README.md), [C1] [C2] [C3]). In the
KV-cache rows arenakv is ahead of memcached by less than 15% [C31], which is not the clear advantage the gate asks
for.

With one request in flight, kernel mode accounts for more than 80% of the server CPU time of arenakv [C28], so the
remaining per-request cost over TCP is mostly the kernel network path. That is the part a transport without kernel
involvement removes, and it is why the next measurement needs RDMA hardware; Soft-RoCE results are not evidence.

## Next steps

1. Repeat the benchmark suite on RDMA hardware (ConnectX class), with the `performance` governor, on two hosts.
2. Worker-per-core serving model for `arenakvd` with polling completion queues.
3. Zero-copy GET (send from the registered arena) and a one-sided GET path with checksummed buckets, measured against
   the RPC path.
4. Integration points for inference serving engines, only if steps 1 to 3 show an advantage.
