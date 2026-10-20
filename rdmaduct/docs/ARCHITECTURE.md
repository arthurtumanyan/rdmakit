# rdmaduct architecture

This document is for people who change or review the library. The public contract is in
`include/rdmaduct.h`; user-facing behavior is summarized in the [README](../README.md).

## Goals and non-goals

Goals: identical message semantics on RDMA reliable connections and on TCP; no hidden retries; a small code base
whose failure paths are tested; no dependencies beyond libc (TCP) or libc plus libibverbs and librdmacm (RDMA).

Non-goals: one-sided operations, multiple NICs, connection multiplexing, security, GPU memory. Each would change
the design (see [Extending](#extending)).

## Source map

| File | Role |
|---|---|
| `include/rdmaduct.h` | public API and error codes |
| `src/internal.h` | internal structures, handshake layout, time helpers, per-transport entry points |
| `src/duct.c` | context lifecycle, option validation, dispatch to the transport through function tables |
| `src/tcp.c` | TCP transport |
| `src/rdma.c` | RDMA transport (compiled only with `DUCT_HAVE_RDMA`) |
| `src/hello.c` | encoding and validation of the connection handshake |
| `tools/duct_echo.c`, `tools/duct_bench.c` | manual test tool, benchmark |
| `tests/` | suites; `suite.h` holds the transport-independent echo and stress tests |

Dispatch: `duct_ep` and `duct_listener` start with a pointer to a table of operations (`struct duct_ep_ops`,
`struct duct_listener_ops`); each transport embeds the base struct as its first member and casts back. The public
functions in `duct.c` check the common limits (for example the message size) and call through the table.

## Invariants

1. A message handed to `duct_send` is delivered whole, in order, or the endpoint becomes unusable.
2. An endpoint is used by one thread at a time; there are no locks on the data path. The context takes a lock
   only to bind the RDMA device on first use.
3. After a non-timeout error the endpoint returns the same error from every call (sticky).
4. Messages that were received before a failure or disconnect are delivered before the error is reported.
5. A sender never has more unreleased messages at the peer than the peer's `recv_depth` (RDMA); the effective
   send limit is the smaller of the two `max_msg` values, so a send slot of the local size always fits the message.
6. Memory registered for remote access is limited to the 4 KiB credit page; data buffers are registered for local
   access only.

## Objects
- Context (`duct_ctx`): options, resolved transport, RDMA device state. The RDMA device and protection domain are bound by the first connection; connections on another device fail with `DUCT_ENODEV`.
- Listener (`duct_listener`): bound address; produces endpoints.
- Endpoint (`duct_ep`): one connection. Operations are dispatched through a function table, so TCP and RDMA share the public API.

## Handshake
Both sides send a 32-byte message after the connection is established (TCP: first bytes of the stream; RDMA: connection-manager private data). Contents: magic, version 2, `max_msg`, `recv_depth`, and for RDMA the address and remote key of the sender's credit word. A sender never exceeds the peer's `max_msg` (`-EMSGSIZE`). Layout: `src/internal.h`.

## TCP framing
4-byte little-endian payload length followed by the payload. Sockets are non-blocking; waits use `poll()` against a deadline. Receive progress persists across calls, so a timeout inside a frame does not corrupt the stream. A timeout after a partial send marks the endpoint broken (`-ECONNABORTED`).

## RDMA endpoint
- One completion queue for send and receive, one RC queue pair created through `rdma_cm`.
- One anonymous mapping holds all buffers; it is registered once (`IBV_ACCESS_LOCAL_WRITE`). Layout: receive slots, then send slots, each `max_msg` bytes.
- Receive slots are posted at creation and re-posted by `duct_rx_release()`. Completed receives wait in a ring until `duct_recv()` delivers them in order.
- Messages are sent as SEND_WITH_IMM. Payloads up to the granted inline size (at least 4, requested 64 bytes) are sent inline from the caller's buffer; larger payloads are copied into the next send ring slot.
- Send ring: slots are used in order and reclaimed in order. Every `send_depth / 4`-th request is signaled; a signaled completion releases all earlier slots (reliable-connection completions are ordered). Completions are polled by `duct_send()` / `duct_send_batch()` when no slot or credit is free and by `duct_recv()`.
- `duct_send_batch()` posts up to 16 messages as one linked work request chain (one doorbell).
- Accepted connections are moved to a private event channel (`rdma_migrate_id`). Peer disconnect is detected from flushed work completions or from the endpoint's event channel, checked every 256 idle polls.
- Waiting: polling mode spins on the completion queue; event mode (`DUCT_WAIT_EVENT`) arms the queue, re-checks it, and sleeps in `ppoll()` on the completion channel and the connection-manager channel. Credit-word updates raise no completion, so a send blocked on credits wakes every 200 us. Idle CPU use over 400 ms with two waiting threads: 800 ms in polling mode, 0 ms in event mode (`tests/test_rdma.c`).
- Message limits: the effective send limit of an endpoint is `min(local max_msg, peer max_msg)`; send ring slots are sized by the local `max_msg`.
- Close: `duct_ep_close()` first waits (at most 1 s) until all posted sends have completed, using a signaled marker write whose completion implies delivery of all earlier sends, because disconnecting immediately can discard sends in flight. Messages received before a disconnect are delivered before `DUCT_ECLOSED` is reported: a disconnect event only marks the endpoint, and the error is raised once the completion queue has stayed empty for a grace period of 3 ms. The grace period is needed because, on Soft-RoCE on aarch64, a receive completion posted before the disconnect was occasionally not yet visible to the poller when the disconnect event was processed (about one in 1500 connect/echo/close cycles in `tests/test_faults.c`; none in 22500 cycles with the grace period). The mechanism has not been identified; the grace period is an empirical bound.

## Flow control
Each side posts `recv_depth` receive buffers. A sender may have at most `peer_recv_depth` messages outstanding. Counters (all 32-bit, wrapping, cumulative):

| Counter | Meaning |
|---|---|
| `released_total` | messages released by the local application (receive slot re-posted) |
| `sent_total` | data messages sent |
| `peer_ack_total` | highest `released_total` learned from the peer |

Credits available = `peer_recv_depth - (sent_total - peer_ack_total)`.

The local `released_total` reaches the peer in two ways: as the immediate data of every data message, and as an RDMA WRITE of the counter (little-endian, 4 bytes, inline) into the peer's credit word. The write is issued when `recv_depth / 2` releases are unannounced, or immediately when the application is about to wait in `duct_recv()` or is blocked in a send. The write consumes no receive buffer at the peer, so no receive slot is reserved for control traffic. The credit word lives in a separate registered page with remote-write access; the data arena is registered for local access only. A counter that would acknowledge more messages than were sent is a protocol violation (`DUCT_EPROTO`); a stale (smaller) counter is ignored.

Verification (flow control): `tests/suite.h` streams sequence-numbered messages of random sizes in random batch sizes to a consumer that sleeps and keeps three messages unreleased; `tests/test_rdma.c` asserts that the device counters `send_rnr_err` and `rcvd_rnr_err` do not change during the run.

## Error model
Negative return values: `-errno` or `DUCT_E*`. `-ETIMEDOUT` leaves the endpoint usable. Any other transport error is sticky: every later call on the endpoint returns the same code.

## Testing

| Suite | What it covers | Notes |
|---|---|---|
| `test_hello` | handshake encoding (byte layout), validation, two million random and bit-flipped inputs | no network |
| `test_tcp` | echo of all message sizes, bursts, oversize rejection, receive timeouts, unreleased-message rule, 50,000-message stream | loopback |
| `test_rdma` | the same echo suite and stream stress in both waiting modes, device receiver-not-ready counters unchanged, idle CPU use of both modes | needs an RDMA device; the counter check is only meaningful when no other process uses the device |
| `test_faults` | raw-socket attacks on TCP framing and handshake, 1500 random streams, refused connections, abrupt close in both waiting modes, asymmetric limits, connect/disconnect cycles with a descriptor-leak check | `DUCT_CYCLES` sets the cycle count (default 300) |
| examples | `hello_server`/`hello_client` over TCP and RDMA | `tests/run_examples.sh` |

Run under sanitizers with `make SAN=1 test`. A long streaming stress: `DUCT_STRESS_COUNT=100000000 make test-rdma`.

`tests/test_faults.c` drives the TCP framing and handshake through a raw socket: oversize frame length, frames split across pauses (receive resumes), truncation inside a frame, garbage and truncated handshakes, accept timeout, and 1500 random byte streams after a valid handshake (only `0`, `DUCT_EPROTO` and `DUCT_ECLOSED` outcomes allowed). For TCP and RDMA (both wait modes): connect to a closed port (`-ECONNREFUSED`), peer close while the other side is blocked in receive (detected within 3 s; sends on the dead endpoint fail), asymmetric `max_msg` and depth, and repeated connect/echo/close cycles with a descriptor-leak check. `tests/test_hello.c` decodes two million random and bit-flipped handshakes.

## Known issues

- After a peer disconnect the library waits up to 3 ms for receive completions that were posted before the
  disconnect (`CLOSE_GRACE_MS` in `rdma.c`). The cause of the late visibility was not identified; it appeared on
  Soft-RoCE on aarch64 at a rate of about one in 1500 connect/echo/close cycles without the grace period.
- Receive and send paths copy data between the caller and registered buffers.
- The credit-word update of a blocked sender is polled every 200 us in event mode because remote writes raise no
  completion.
- Only the first RDMA device used by a context is supported.

## Extending

- **New transport:** implement the two operation tables and a `connect`/`listen` pair, register it in
  `duct_ctx_create`, `duct_listen` and `duct_connect`, and run `suite.h` against it. The suite is the executable
  definition of the semantics.
- **One-sided operations:** needs a way to publish registered regions (a handshake extension, version 3) and a
  scoping rule for remote keys; the 4 KiB credit page shows the pattern. The credit protocol is independent of it.
- **Zero-copy send:** requires the caller to obtain buffers from the endpoint (a send-buffer API) so that data is
  built in registered memory; the send ring already tracks slot ownership.
