# Lab: RDMA programming in twelve steps

Part 1 (steps 1 to 6) introduces RDMA programming with libibverbs and librdmacm, each program building on the
previous one, ending with a key-value store whose GET operations are one-sided RDMA reads. Part 2 (steps 7 to 12)
covers the same ideas, and the ones a server needs beyond them, with the [`rdmaduct`](../rdmaduct) library; these
programs run unchanged over TCP, so they need no RDMA device. Each file starts with a comment block that explains
the concepts it uses; the code is commented step by step.

## Part 1: libibverbs and librdmacm

| Step | File | Topic | Machines |
|---|---|---|---|
| 1 | `step1_open_device.c` | list and open an RDMA device; query device limits, port state and GIDs | one |
| 2 | `step2_register_memory.c` | protection domains, memory registration, `lkey`/`rkey`, access flags, registration cost | one |
| 3 | `step3_loopback.c` | completion queues, queue pairs and their state transitions by hand, SEND/RECV, an RDMA WRITE | one (loopback) |
| 4 | `step4_cm_hello.c` | connection setup with `rdma_cm`, two-sided SEND/RECV | two |
| 5 | `step5_one_sided.c` | RDMA READ and WRITE, atomics, WRITE_WITH_IMM, passing address and `rkey` in the connection's private data | two |
| 6 | `step6_kvs.c` | a key-value store: GET by one-sided READ with checksum validation, PUT by SEND/RECV, one thread per client | two |

## Part 2: the same ideas with rdmaduct

| Step | File | Topic |
|---|---|---|
| 7 | `step7_duct_echo.c` | context, listen, accept and connect, send, receive and release; timeouts and error codes; the same program over TCP and RDMA; `--tls` in `TLS=1` builds |
| 8 | `step8_duct_flow.c` | credit-based flow control and back-pressure made visible with non-blocking sends; batching with `duct_send_batch`; throughput by message size |
| 9 | `step9_duct_event_loop.c` | one thread serving many clients through `duct_ep_fd`, `duct_ep_arm` and epoll; the CPU time of an event loop against busy polling |
| 10 | `step10_duct_zero_copy.c` | `duct_mr_reg`, `duct_send_mr` and `duct_wait`: zero-copy sends from a ring of registered buffers, with the rule that a buffer stays unchanged until its send completes |
| 11 | `step11_duct_one_sided.c` | a region descriptor sent in a message; one-sided read and write; a shared counter with fetch-and-add and a compare-and-swap spinlock from two clients; why one-sided operations over TCP need the target to call into the library |
| 12 | `step12_duct_kv.c` | step 6 rebuilt on rdmaduct: PUT and DEL as messages, GET by one-sided read with checksum validation, GET as a message for comparison, one server thread for all clients |

Every part-2 program has the options `--transport tcp|rdma` (default `tcp`), `--port`, and `--wait poll|event`
(RDMA). Over TCP server and client run on one machine with `127.0.0.1`; over RDMA the client connects to the
address of the server's RDMA interface.

```
make RDMA=0                           # steps 7 to 12 only, no RDMA libraries needed: build/<arch>-tcp/
B=build/x86_64-tcp

$B/step7_duct_echo server --once &
$B/step7_duct_echo client 127.0.0.1 hello lab

$B/step8_duct_flow server &
$B/step8_duct_flow client 127.0.0.1 --mib 16

$B/step9_duct_event_loop server --clients 16 &            # once more with --busy, and compare the CPU line
$B/step9_duct_event_loop client 127.0.0.1 --clients 16

$B/step10_duct_zero_copy server &
$B/step10_duct_zero_copy client 127.0.0.1

$B/step11_duct_one_sided server --clients 2 &
$B/step11_duct_one_sided client 127.0.0.1                 # then with --no-lock, and with a --no-progress server

$B/step12_duct_kv server &
$B/step12_duct_kv client 127.0.0.1 put city Yerevan
$B/step12_duct_kv client 127.0.0.1 get city
$B/step12_duct_kv client 127.0.0.1 bench 2000
```

`make test` runs steps 7 to 12 as server and client and checks their output (`tests/run.sh`): over TCP always,
and over RDMA as well in RDMA builds when an RDMA device is found.

## Requirements

Part 2 over TCP: Linux, `gcc` or `clang`, `make`. Part 1, and part 2 over RDMA: `libibverbs-dev`,
`librdmacm-dev`, and an RDMA device. Without RDMA hardware, create a
Soft-RoCE device on any Ethernet interface:

```
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev <interface>
rdma link show
```

The memory-lock limit must allow registering memory (`ulimit -l unlimited` for the session, or a limits entry).

## Build and run (part 1)

```
make                             # build/<arch>/step*: steps 1 to 6 and, on rdmaduct, 7 to 12
build/x86_64/step1_open_device
build/x86_64/step2_register_memory
build/x86_64/step3_loopback

# two-machine steps: server first
build/x86_64/step4_cm_hello server
build/x86_64/step4_cm_hello client <server-ip>
build/x86_64/step5_one_sided server
build/x86_64/step5_one_sided client <server-ip>

build/x86_64/step6_kvs server
build/x86_64/step6_kvs client <server-ip>          # interactive: put k v | get k | del k | bench [N] | quit
build/x86_64/step6_kvs client <server-ip> 7473 put city Yerevan
```

The two-machine steps also work on one machine when the client connects to the address of the local RDMA interface
(not `127.0.0.1`: loopback has no RDMA device).

## Notes

- The GID index used by the RoCEv2 path is found by scanning (step 3); programs using `rdma_cm` do not need it.
- Soft-RoCE executes the RDMA protocol in the kernel, so timings from these programs describe the software path.
- Typical errors: `IBV_WC_RNR_RETRY_EXC_ERR` (the peer had no receive posted), `IBV_WC_REM_ACCESS_ERR` (wrong `rkey`,
  address or missing remote access flag), `IBV_WC_WR_FLUSH_ERR` (the queue pair is already in the error state; the
  first error is the informative one), `ibv_reg_mr: Cannot allocate memory` (memory-lock limit too low).

## Exercises

1. Register the buffer of step 5 with `IBV_ACCESS_REMOTE_READ` only and attempt an RDMA WRITE. Which completion status results?
2. Remove the `ibv_post_recv` of step 3 and observe the failure of the SEND; set `rnr_retry` to 0.
3. Implement a remote spinlock with `IBV_WR_ATOMIC_CMP_AND_SWP` and run it from two clients.
4. Extend the values of step 6 beyond 32 bytes (value pointer and length in the slot, second READ).
5. Add an `INCR` operation implemented with FETCH_AND_ADD instead of a SEND.
6. Replace polling in the step 6 client by a completion channel and compare CPU use.
7. Replace the CRC check by a version-number scheme (seqlock) and compare.

Part 2:

8. Step 7: connect to a port where no server listens, over TCP and over RDMA. Which codes are returned, and after
   how long?
9. Step 8: change `DEPTH` of the server only. How does the number of messages accepted before back-pressure change
   on RDMA, and on TCP? Which buffers explain each number?
10. Step 9: serve 200 clients and compare the server's CPU time per request with and without `--busy`.
11. Step 10: remove the `duct_wait` before a slot is rewritten and run over RDMA. Does the server report corrupt
    messages? Why can the answer differ between Soft-RoCE and an adapter?
12. Step 11: replace the compare-and-swap spinlock with a ticket lock (fetch-and-add on a ticket word, then wait
    until a second word equals the ticket).
13. Step 12: run four clients with `bench` at the same time as one client that issues PUTs of the same keys in a
    loop, over RDMA. Count the torn reads; then make the server skip the release fences and count again.

The production-style versions of these ideas are in the sibling projects [`rdmaduct`](../rdmaduct) (message transport),
[`arenakv`](../arenakv) (cache server) and [`kvpace`](../kvpace) (load generator).
