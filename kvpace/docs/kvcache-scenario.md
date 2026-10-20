# KV-cache scenario

`--scenario kvcache` replaces single-key operations by requests that model the access pattern of an inference KV-cache store. Parameter and structure definitions: `src/kvcache.h`.

## Request model
- A prompt is a chain of blocks: `shared_blocks` template blocks (identical for all requests of a prefix group) followed by `(t + 1) * turn_blocks` conversation blocks for turn `t` of a conversation.
- The store is probed with GET in chain order until the first miss. Every block from the first miss to the end of the chain is then written with PUT (the recomputed tail).
- Each thread holds `active_conv` conversation slots, selected with a Zipfian distribution (`--theta`). A slot is replaced by a new conversation with probability `new_conv_prob` per request or after `max_turns` turns. Prefix groups are drawn with the same distribution. Threads share no state.
- Block content is a deterministic function of the key; hits are compared byte for byte (`corrupt` counts mismatches).

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

- The request structure (template prefix, conversation turns, Zipfian reuse) is a model with explicit parameters; it
  is not derived from a production trace.
- Block content is a deterministic function of the key, so servers that compress values would benefit unrealistically
  (none of the supported servers compress by default).
- A request costs a PUT per missing block; in an inference system a missing block costs computation, not a write.
  The reuse rate, not the request latency, corresponds to saved computation.
