# Lab: RDMA programming in six steps

Six small C programs that introduce RDMA programming with libibverbs and librdmacm, each building on the previous
one, ending with a key-value store whose GET operations are one-sided RDMA reads. Each file starts with a
comment block that explains the concepts it uses; the code is commented step by step.

| Step | File | Topic | Machines |
|---|---|---|---|
| 1 | `step1_open_device.c` | list and open an RDMA device; query device limits, port state and GIDs | one |
| 2 | `step2_register_memory.c` | protection domains, memory registration, `lkey`/`rkey`, access flags, registration cost | one |
| 3 | `step3_loopback.c` | completion queues, queue pairs and their state transitions by hand, SEND/RECV, an RDMA WRITE | one (loopback) |
| 4 | `step4_cm_hello.c` | connection setup with `rdma_cm`, two-sided SEND/RECV | two |
| 5 | `step5_one_sided.c` | RDMA READ and WRITE, atomics, WRITE_WITH_IMM, passing address and `rkey` in the connection's private data | two |
| 6 | `step6_kvs.c` | a key-value store: GET by one-sided READ with checksum validation, PUT by SEND/RECV, one thread per client | two |

## Requirements

Linux, `gcc` or `clang`, `libibverbs-dev`, `librdmacm-dev`, and an RDMA device. Without RDMA hardware, create a
Soft-RoCE device on any Ethernet interface:

```
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev <interface>
rdma link show
```

The memory-lock limit must allow registering memory (`ulimit -l unlimited` for the session, or a limits entry).

## Build and run

```
make                             # build/<arch>/step*
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

The production-style versions of these ideas are in the sibling projects [`rdmaduct`](../rdmaduct) (message transport),
[`arenakv`](../arenakv) (cache server) and [`kvpace`](../kvpace) (load generator).
