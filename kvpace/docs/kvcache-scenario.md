# KV-cache scenario

`--scenario kvcache` replaces single-key operations by requests that model the access pattern of an inference KV-cache store. Parameter and structure definitions: `src/kvcache.h`.

## Request model
- A prompt is a chain of blocks: `shared_blocks` template blocks (identical for all requests of a prefix group) followed by `(t + 1) * turn_blocks` conversation blocks for turn `t` of a conversation.
- The store is probed with GET in chain order until the first miss. Every block from the first miss to the end of the chain is then written with PUT (the recomputed tail).
- Each thread holds `active_conv` conversation slots, selected with a Zipfian distribution (`--theta`). A slot is replaced by a new conversation with probability `new_conv_prob` per request or after `max_turns` turns. Prefix groups are drawn with the same distribution. Threads share no state.
- With `--conns-per-thread` or `--pipeline` above 1, each pipeline slot runs the requests of one such thread, with one block operation in flight; `T x C x P` slots make the requests of `T x C x P` synchronous threads.
- Block content is a deterministic function of the key; hits are compared byte for byte (`corrupt` counts mismatches).

## Trace replay

With `--trace FILE` the chains come from a request trace instead of the model: each line of the file (JSON, in the
layout of the Mooncake traces) gives an arrival time and the block ids of a prompt, and requests that share a
prefix share its ids. Each request is probed and written exactly as a modeled one; requests start in trace order at
their scaled trace times, so the arrival pattern and the prefix sharing are those of the trace. Block ids are
mapped to keys in a namespace of their own, so trace blocks never meet modeled blocks. Usage:
[`USAGE.md`](USAGE.md#5-workloads).

## Metrics
| Metric | Definition |
|---|---|
| `reuse_rate` | `block_hits / (block_hits + block_puts)`: fraction of needed blocks served from the cache instead of being recomputed |
| `hit_rate` (probe hit rate) | `block_hits / block_gets`: overstates effectiveness because every request has at most one missing GET |
| `request_latency_ns` | latency of whole requests, from the intended send time |
| `bytes_get`, `bytes_put` | payload volume read from and written to the store |

## Results

Measured reuse rates, throughput and latency of real servers under this scenario, by configured cache capacity,
are in [`../../benchmarks/README.md`](../../benchmarks/README.md); the method is in
[`../../benchmarks/METHODOLOGY.md`](../../benchmarks/METHODOLOGY.md). This document does not repeat numbers.

## Limits of the model

- The request structure of the model (template prefix, conversation turns, Zipfian reuse) has explicit parameters;
  it is not derived from a production trace. Trace replay (above) takes the structure from a trace instead.
- Block content is a deterministic function of the key, so servers that compress values would benefit unrealistically
  (none of the supported servers compress by default).
- A request costs a PUT per missing block; in an inference system a missing block costs computation, not a write.
  The reuse rate, not the request latency, corresponds to saved computation.
