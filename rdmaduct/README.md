# rdmaduct

A small C library for communication between two endpoints over **RDMA reliable connections** or **TCP**, with
the same API and the same behavior on both.

- Two-sided messages: reliable, ordered, message-preserving delivery; a message is received whole or not at all.
- Registered memory regions, zero-copy sends from them, and one-sided **RDMA READ, WRITE and 64-bit atomics**
  (fetch-and-add, compare-and-swap) on a peer's region. On TCP the peer's library emulates them.
- Posted operations with sequence numbers, so many operations are in flight behind one wait.
- **Event-loop integration:** every endpoint and listener has a pollable descriptor, so thousands of connections
  can share one epoll loop.
- Credit-based flow control on RDMA: a sender never overruns the receiver's posted buffers.
- Blocking calls with timeouts; polling or event-driven waiting for RDMA endpoints.
- Batched sends: one doorbell per up to 16 messages on RDMA, one `sendmsg()` per up to 32 messages on TCP; buffered
  TCP reads, so pipelined small messages cost a fraction of a system call each.
- Builds without RDMA libraries (`make RDMA=0`) for machines without RDMA hardware.
- Tested for faults and malformed input (see [Testing](#testing)).

**Status:** experimental. The API may change before a first release. No encryption or authentication.

## Why this exists

RDMA programming with libibverbs and librdmacm is verbose and full of failure modes: queue pair state machines,
receive buffers that must be posted before the peer sends, completion errors that only appear under load. The
established frameworks (UCX, libfabric) solve this and much more, but they are large toolkits whose capabilities
and behavior vary with the transport or provider in use. Applications that only need reliable messages face a
choice between hand-written verbs code and a framework far bigger than the problem.

`rdmaduct` is a deliberately small middle ground:

- **One API, identical semantics on RDMA and TCP.** Messages, one-sided operations, atomics, ordering, flow
  control, timeouts and the error model are the same on both transports, and the same test suites run unchanged
  on both. Code developed on a machine without RDMA hardware behaves the same on an RDMA cluster.
- **Flow control that cannot overrun the receiver.** Credits track the peer's posted buffers. A test with a slow
  consumer checks the device's receiver-not-ready counters.
- **Failure paths tested, not assumed:** two million fuzzed handshakes, a raw socket that attacks the TCP
  framing, abrupt closes in both waiting modes, and descriptor-leak checks over connect/disconnect cycles.
- **Small dependency set:** libc and pthreads for TCP; libibverbs and librdmacm for RDMA. About 3,500 lines with
  headers.

It does not do GPU memory, multi-rail, tag matching or collectives. For those, use UCX, libfabric or MPI
([`../docs/PRIOR_ART.md`](../docs/PRIOR_ART.md)).

### Comparison

| | rdmaduct | UCX | libfabric | rsocket (librdmacm) |
|---|---|---|---|---|
| Programming model | two-sided messages, RMA read/write, atomics | tag matching, streams, active messages, RMA, atomics | messages, tagged messages, RMA, atomics | stream sockets |
| Transports | RDMA reliable connections, TCP | many (verbs, shared memory, TCP, GPU) | many providers (verbs, TCP, shared memory, EFA, CXI) | RDMA |
| Same semantics on every transport | yes; one test suite runs on both | capabilities depend on the transport | capabilities depend on the provider | single transport |
| Message boundaries preserved | yes | yes | yes | no (byte stream) |
| One-sided read/write and 64-bit atomics | yes (emulated on TCP) | yes | yes | write only (`riowrite`) |
| Zero-copy send from registered memory | yes | yes | yes | no |
| Pollable descriptor for event loops | yes (endpoint and listener) | yes (worker) | yes (wait sets) | `rpoll()`, not epoll |
| GPU memory, multi-rail, tag matching | no | yes | yes | no |
| Source size | about 3,500 lines | large framework | large framework | part of librdmacm |

Statements about other systems reflect their public documentation at the time of writing.

### Measured results

Loopback on one host: both endpoints on the same machine, RDMA through Soft-RoCE (software RDMA, not RDMA
hardware). Tables are generated from the raw data in [`../benchmarks/data`](../benchmarks/data) and checked by
`make verify` at the repository root.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../benchmarks/figures/transport-dark.svg">
  <img alt="Two line charts: median round-trip time and streaming throughput versus message size for TCP and Soft-RoCE in polling and event-driven mode" src="../benchmarks/figures/transport.svg">
</picture>

<!-- BEGIN GENERATED:transport-rtt -->
Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

Median round-trip time in microseconds (ping-pong, one message in flight). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 9.6 (cv 37.9%) | 6.8 (cv 2.7%) | 10.3 (cv 9.7%) |
| 256 | 7.8 (cv 0.4%) | 6.7 (cv 3.4%) | 10.1 (cv 1.8%) |
| 1024 | 8.1 (cv 2.0%) | 7.2 (cv 2.9%) | 10.7 (cv 3.6%) |
| 4096 | 9.0 (cv 2.5%) | 11.2 (cv 1.8%) | 15.1 (cv 1.7%) |
| 16384 | 16.2 (cv 45.4%) | 30.4 (cv 1.4%) | 35.3 (cv 2.0%) |
| 65536 | 29.2 (cv 35.9%) | 106.5 (cv 0.9%) | 113.4 (cv 1.8%) |
| 262144 | 50.6 (cv 2.3%) | 442.1 (cv 2.7%) | 459.6 (cv 1.9%) |
| 1048576 | 185.7 (cv 1.7%) | 1,898.3 (cv 1.3%) | 1,938.9 (cv 1.1%) |
<!-- END GENERATED:transport-rtt -->

<!-- BEGIN GENERATED:transport-stream -->
Data: `benchmarks/data/2026-10-21-run2` (690 runs: 660 ok, 20 with operation errors, 0 failed, 10 skipped), commit `46efd6447c6d`, summary digest `95813d0d095ed976`. Mean over repetitions; `cv` is the coefficient of variation.

One-way streaming throughput in MB/s (batched sends, 64 buffers per direction). Loopback on one host.

| Message bytes | TCP | Soft-RoCE, polling | Soft-RoCE, event-driven |
|---|---|---|---|
| 64 | 285 (cv 26.5%) | 46 (cv 6.8%) | 47 (cv 2.6%) |
| 256 | 1,037 (cv 1.4%) | 188 (cv 2.7%) | 178 (cv 6.6%) |
| 1024 | 3,287 (cv 12.7%) | 730 (cv 2.2%) | 715 (cv 2.2%) |
| 4096 | 7,352 (cv 20.5%) | 915 (cv 0.5%) | 968 (cv 1.6%) |
| 16384 | 10,546 (cv 9.1%) | 1,155 (cv 1.3%) | 1,137 (cv 0.9%) |
| 65536 | 9,213 (cv 20.7%) | 1,279 (cv 3.0%) | 1,263 (cv 1.9%) |
| 262144 | 5,757 (cv 10.3%) | 1,170 (cv 4.4%) | 1,158 (cv 1.9%) |
| 1048576 | 6,329 (cv 43.2%) | 1,124 (cv 2.5%) | 1,087 (cv 2.4%) |
<!-- END GENERATED:transport-stream -->

- For messages up to 1 KiB the median round trip of Soft-RoCE with polling is lower than that of TCP; from 16 KiB TCP
  is lower than both Soft-RoCE modes [C19].
- TCP loopback streaming is at least four times faster than both Soft-RoCE modes at every measured size [C20]. TCP
  sends a batch with one `sendmsg()` and reads through a 64 KiB buffer, so small streamed messages cost a fraction of
  a system call each.
- These numbers describe the cost of a software RDMA implementation on one host. They say nothing about RDMA
  hardware, where the data path bypasses the kernel.

## Requirements

- Linux, a C compiler with `-std=gnu2x` (gcc 13 or newer, clang 18 or newer), `make`, pthreads.
- For RDMA: `libibverbs-dev` and `librdmacm-dev` (Debian/Ubuntu names) and an RDMA device. Soft-RoCE
  (`rdma_rxe`) works for development: `sudo modprobe rdma_rxe && sudo rdma link add rxe0 type rxe netdev <interface>`.
- Python 3 is not required.

## Build

```
make                 # build/<arch>/librdmaduct.a, duct_echo, duct_bench, examples
make RDMA=0          # TCP transport only: needs only libc and pthreads
make SAN=1 test      # AddressSanitizer + UndefinedBehaviorSanitizer
```

`RDMA=0` builds go to `build/<arch>-tcp/`.

Link with `build/<arch>/librdmaduct.a -lpthread` and, for RDMA builds, `-lrdmacm -libverbs`. Include path:
`include/`.

## Quick start

Server (`examples/hello_server.c`) and client (`examples/hello_client.c`):

```
build/x86_64/hello_server tcp 7800 &
build/x86_64/hello_client tcp 127.0.0.1 7800 abc "rdma transport"
# abc -> cba
# rdma transport -> tropsnart amdr
```

Use `rdma` instead of `tcp` on both sides to select RDMA; the host argument of the client must then be the IPv4
address of an interface that carries an RDMA device. The core of a client:

```c
struct duct_opts opts;
duct_opts_default(&opts);                 /* AUTO transport, 64 KiB messages, 32 buffers */
duct_ctx *ctx;
duct_ctx_create(&opts, &ctx);

duct_ep *ep;
duct_connect(ctx, "192.0.2.10", "7800", &ep);
duct_send(ep, "ping", 4, 1000);           /* timeout in milliseconds */

struct duct_rx rx;
duct_recv(ep, &rx, 1000);                 /* rx.data and rx.len are valid until released */
duct_rx_release(ep, &rx);

duct_ep_close(ep);
duct_ctx_destroy(ctx);
```

## Concepts

| Object | Meaning |
|---|---|
| context (`duct_ctx`) | options and shared state (RDMA device, protection domain); create one per process or per thread group |
| listener (`duct_listener`) | accepts incoming connections |
| endpoint (`duct_ep`) | one connection; used by one thread at a time |
| received message (`struct duct_rx`) | borrowed from the transport until `duct_rx_release()` |

## API summary

Full contracts are in [`include/rdmaduct.h`](include/rdmaduct.h).

| Function | Purpose |
|---|---|
| `duct_opts_default`, `duct_ctx_create`, `duct_ctx_destroy`, `duct_ctx_transport` | configure and create a context |
| `duct_listen`, `duct_listener_port`, `duct_accept`, `duct_listener_close` | server side |
| `duct_connect`, `duct_ep_close`, `duct_ep_peer_max_msg` | client side and endpoint management |
| `duct_send`, `duct_send_batch` | send one message or a batch (copying) |
| `duct_recv`, `duct_rx_release` | receive a message and return its buffer |
| `duct_ep_fd`, `duct_ep_arm`, `duct_progress`, `duct_ep_send_space`, `duct_listener_fd` | event-loop integration |
| `duct_mr_reg`, `duct_mr_dereg`, `duct_mr_export`, `duct_mr_addr` | registered regions and their descriptors |
| `duct_send_mr` | zero-copy send of part of a region |
| `duct_read`, `duct_write`, `duct_atomic_fadd`, `duct_atomic_cswap` | one-sided operations on a peer's region |
| `duct_ep_completed`, `duct_wait` | completion of posted operations |
| `duct_strerror` | text of an error code |

### Event loop

```c
/* add duct_ep_fd(ep) to an epoll set for EPOLLIN, then: */
for (;;) {
    struct duct_rx rx;
    while (duct_recv(ep, &rx, 0) == 0) {          /* drain without blocking */
        handle(rx.data, rx.len);
        duct_rx_release(ep, &rx);
    }
    if (duct_ep_arm(ep) == -EAGAIN) {
        continue;                                 /* work arrived while arming */
    }
    break;                                        /* wait for epoll to report the descriptor */
}
```

### One-sided operations

```c
/* target */
duct_mr *mr;
duct_mr_reg(ctx, buf, len, DUCT_ACCESS_REMOTE_READ | DUCT_ACCESS_REMOTE_WRITE, &mr);
struct duct_mr_desc d;
duct_mr_export(mr, &d);
duct_send(ep, &d, sizeof(d), 1000);               /* the descriptor travels as an ordinary message */

/* initiator (local region lmr registered with DUCT_ACCESS_LOCAL) */
uint64_t seq;
duct_read(ep, lmr, 0, &d, 4096, 65536, 1000, &seq);   /* 64 KiB from offset 4096 of the target region */
duct_wait(ep, seq, 1000);
```

On TCP the target's library executes the operation while the target application calls any function on that
endpoint (`duct_recv`, `duct_progress`, an event loop); on RDMA the network adapter executes it without the
target's CPU.

### Options (`struct duct_opts`)

| Field | Default | Meaning |
|---|---|---|
| `transport` | `DUCT_TRANSPORT_AUTO` | `TCP`, `RDMA`, or `AUTO` (RDMA when built with RDMA support and a device exists) |
| `max_msg` | 64 KiB | largest message this side accepts; receive buffers have this size |
| `recv_depth` | 32 | receive buffers per endpoint; also the number of unreleased messages the peer may have in flight (RDMA) |
| `send_depth` | 32 | send buffers per endpoint (RDMA) |
| `connect_timeout_ms` | 10000 | connection setup timeout; `-1` waits indefinitely |
| `wait` | `DUCT_WAIT_POLL` | RDMA waiting: spin on the completion queue, or sleep until an event |

The effective send limit of an endpoint is the smaller of the local and the peer's `max_msg`.

### Behavior guarantees

- Messages arrive in the order sent, whole, or the endpoint fails.
- `duct_send` copies the data; the caller's buffer is reusable on return.
- An endpoint may hold up to `recv_depth` received messages unreleased, on both transports.
- Posted operations complete in posting order; `duct_ep_completed()` is cumulative.
- Timeouts return `-ETIMEDOUT` and leave the endpoint usable; a timed-out send has sent nothing. Arguments that are
  invalid by local information return `-EINVAL` and also leave it usable. Every other transport error is sticky:
  the endpoint must be closed.
- Messages received before a disconnect are delivered before `DUCT_ECLOSED` is reported.
- `duct_ep_close` waits (at most one second) for sends in flight, so a message sent just before closing is not lost.
- A peer that stops releasing received messages stalls the sender (flow control); it does not cause errors.

### Error codes

Negative `errno` values (`-ETIMEDOUT`, `-EMSGSIZE`, `-ECONNREFUSED`, ...) and `DUCT_ECLOSED` (peer closed),
`DUCT_EPROTO` (protocol violation), `DUCT_EWC` (RDMA work completion error), `DUCT_ENODEV` (no usable RDMA device),
`DUCT_EACCESS` (a one-sided operation violated the remote region's range or rights).

## Waiting modes

| Mode | CPU while idle | Wake-up latency | Use when |
|---|---|---|---|
| `DUCT_WAIT_POLL` | one core busy per waiting thread | lowest | each waiting thread has its own core |
| `DUCT_WAIT_EVENT` | none | a few microseconds more; credit updates are re-checked every 200 µs | more waiting threads than cores, or idle connections |

Measurements of both modes are in [Measured results](#measured-results).

## Tools

| Tool | Purpose |
|---|---|
| `duct_echo` | echo server and round-trip client for manual tests |
| `duct_bench` | latency and throughput benchmark used by `benchmarks/` (see `tools/duct_bench.c`) |

## Testing

```
make test                                  # unit, TCP, fault-injection and (if a device exists) RDMA suites
DUCT_TEST_ADDR=<rdma-interface-ip> make test   # select the RDMA address explicitly
DUCT_STRESS_COUNT=100000000 make test-rdma     # long streaming stress
```

The suites cover the handshake codec (including two million fuzzed inputs), TCP framing and control frames
attacked through a raw socket (oversize frames, split frames, truncation, garbage, random emulated requests),
flow control under a slow consumer with a check that the device reports no receiver-not-ready errors, 64
connections through one epoll loop without missed wake-ups, one-sided operations and atomics including remote
access violations, abrupt close in both waiting modes, asymmetric limits, connect/disconnect cycles with a
descriptor-leak check, and the examples. Details:
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#testing).

Test results: all suites pass on x86-64 and aarch64, including the RDMA suite over Soft-RoCE in both waiting modes.
On x86-64 they also pass under AddressSanitizer with UndefinedBehaviorSanitizer. CI runs the TCP suites with gcc
and clang on x86-64 and arm64.

| Suite | Covers |
|---|---|
| `test_hello` | handshake encoding and validation, two million fuzzed inputs |
| `test_tcp` | echo suite over TCP on loopback |
| `test_rdma` | the same echo suite over RDMA, receiver-not-ready counter check under a slow consumer |
| `test_poll` | 64 connections per side driven by one epoll set per side with non-blocking sends; fails on any missed wake-up |
| `test_mr` | write/read back up to 256 KiB, 64 pipelined writes, fetch-and-add and compare-and-swap, zero-copy send, local validation, `DUCT_EACCESS` for forged and deregistered regions |
| `test_faults` | TCP framing, handshake and 1500 random control frames attacked through a raw socket; accept timeout, refused connections, asymmetric limits, abrupt close |
| `run_examples.sh` | the example server and client |

## Limitations

- An endpoint is used by one thread at a time; many endpoints can share one thread through their descriptors.
- No multiple NICs, no GPU memory, no tag matching or collectives.
- Received messages land in the endpoint's buffers (no zero-copy receive); `duct_send_mr` avoids the send copy.
- On TCP, one-sided operations progress only while the target application calls into the library on that endpoint.
- No encryption or authentication: use it on a trusted network.
- Received-completion visibility after a peer disconnect is handled with a short grace period (see
  `docs/ARCHITECTURE.md`); the underlying cause was not identified.
- Performance numbers exist only for Soft-RoCE and TCP on Ethernet; there are none for RDMA hardware.

## Layout

| Path | Content |
|---|---|
| `include/rdmaduct.h` | public API |
| `src/` | implementation (`duct.c` dispatch and validation, `tcp.c`, `rdma.c`, `hello.c` wire codecs, `internal.h`) |
| `tools/` | `duct_echo`, `duct_bench` |
| `examples/` | `hello_server`, `hello_client` |
| `tests/` | test suites |
| `docs/` | architecture and protocol documentation |

## Further reading

- [Architecture and protocol](docs/ARCHITECTURE.md)
- [Contributing](../CONTRIBUTING.md), [Security notes](../SECURITY.md)
