# Glossary

| Term | Meaning |
|---|---|
| RDMA | Remote Direct Memory Access: a network adapter reads or writes memory of a remote host without involving that host's CPU in the data transfer |
| verbs | the programming interface of RDMA adapters (libibverbs) |
| RoCE, RoCEv2 | RDMA over Converged Ethernet; version 2 carries RDMA in UDP/IP packets and is routable |
| Soft-RoCE (`rdma_rxe`) | Linux kernel implementation of RoCE on top of an ordinary Ethernet interface; no RDMA hardware needed |
| device context | handle to an RDMA device (`struct ibv_context`) |
| PD (protection domain) | container that ties memory regions and queue pairs together |
| MR (memory region) | registered memory that the adapter may access; registration pins pages and yields keys |
| lkey, rkey | local and remote key of a memory region; the rkey allows a peer to access the memory |
| QP (queue pair) | pair of work queues (send and receive) |
| RC (reliable connection) | queue pair type with acknowledged, in-order delivery to one peer |
| CQ (completion queue) | queue in which the adapter reports finished work requests |
| WR, WC | work request (posted by software) and work completion (reported by the adapter) |
| SGE | scatter/gather entry: address, length and local key of a buffer |
| SEND/RECV | two-sided operations: a send consumes a posted receive at the peer |
| RDMA WRITE, READ | one-sided operations: the adapter writes or reads remote memory directly |
| immediate data | 32-bit value delivered with a SEND or WRITE |
| doorbell | write that tells the adapter new work requests are available; batching posts amortizes it |
| signaled completion | work request that generates a completion; unsignaled ones do not |
| inline send | small payload copied into the work request instead of being read from registered memory |
| RNR | receiver not ready: a send arrived with no receive buffer posted |
| GID | 128-bit address of a RoCE port, derived from the interface IP address |
| PSN | packet sequence number |
| `rdma_cm` | connection manager (librdmacm) that exchanges queue pair parameters |
| completion channel | file descriptor that signals that a completion queue is non-empty |
| credit | permission to send one more message without overrunning the receiver's buffers |
| closed loop, open loop | load models: a client waits for each response before the next request, or sends on a schedule regardless of responses |
| coordinated omission | measurement error in which a stalled server delays the load generator and thereby hides the latency that scheduled requests would have experienced |
| reuse rate | fraction of needed cache blocks served from the cache instead of being recomputed |
| log-structured memory | storage that appends entries at a head and reclaims space at a tail |
