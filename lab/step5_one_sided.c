/*
 * STEP 5 - One-sided operations: RDMA WRITE, RDMA READ, ATOMICS, WRITE_WITH_IMM
 * ============================================================================
 * This is what makes RDMA special. With SEND/RECV (step 4) both CPUs take part. With one-sided verbs the
 * client's NIC reads/writes the SERVER'S MEMORY directly; the server's CPU does not run a single instruction
 * and does not even get a completion.
 *
 *   RDMA WRITE   local buffer  -> remote_addr        needs remote MR flag REMOTE_WRITE
 *   RDMA READ    remote_addr   -> local buffer       needs remote MR flag REMOTE_READ
 *   ATOMIC FETCH_AND_ADD / CMP_AND_SWP on a remote 8-byte aligned word   needs REMOTE_ATOMIC
 *   WRITE_WITH_IMM  RDMA WRITE + a 32-bit immediate that DOES consume a RECV at the target and creates a
 *                   completion there  => the way to say "data is ready" without a separate SEND.
 *
 * The initiator must know the target's (address, rkey, length). These are shipped in the *private data* of the
 * connection handshake (rdma_accept can carry up to ~56 bytes), so no extra message is needed.
 *
 * Usage:  step5_one_sided server [port]      |      step5_one_sided client <server-ip> [port]
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include "common.h"

#define DEFAULT_PORT "7472"
#define REGION 4096

// Description of the server memory that is sent to the client as connection private data. Fixed layout, fixed
// endianness => use explicit-width types and (for real code) htonl/be64toh; both CPU architectures used here are little-endian.
struct region_info {
    uint64_t addr;
    uint32_t rkey;
    uint32_t len;
};

static struct rdma_cm_event *expect_event(struct rdma_event_channel *ec, enum rdma_cm_event_type want) {
    struct rdma_cm_event *ev;
    CHECK_ZERO(rdma_get_cm_event(ec, &ev), "rdma_get_cm_event");
    if (ev->event != want)
        die("unexpected CM event %s (wanted %s)", rdma_event_str(ev->event), rdma_event_str(want));
    return ev;
}

static void create_qp(struct rdma_cm_id *id, struct ibv_pd *pd, struct ibv_cq *cq) {
    struct ibv_qp_init_attr qa = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = 16, .max_recv_wr = 16, .max_send_sge = 1, .max_recv_sge = 1}};
    CHECK_ZERO(rdma_create_qp(id, pd, &qa), "rdma_create_qp");
}

// Post a single signaled one-sided/any WR and wait for its completion.
static void post_and_wait(struct ibv_qp *qp, struct ibv_cq *cq, struct ibv_send_wr *wr, const char *what) {
    struct ibv_send_wr *bad;
    CHECK_ZERO(ibv_post_send(qp, wr, &bad), what);
    wait_completion(cq, what);
}

// ------------------------------------------------------------------------------------------------ server
static void run_server(const char *port) {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *listener;
    CHECK_ZERO(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP), "create_id");
    struct addrinfo *ai;
    CHECK_ZERO(getaddrinfo(NULL, port, &(struct addrinfo){.ai_flags = AI_PASSIVE, .ai_family = AF_INET}, &ai),
               "gai");
    CHECK_ZERO(rdma_bind_addr(listener, ai->ai_addr), "bind");
    CHECK_ZERO(rdma_listen(listener, 4), "listen");
    printf("server: listening on %s\n", port);

    struct rdma_cm_event *ev = expect_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST);
    struct rdma_cm_id *id = ev->id;
    rdma_ack_cm_event(ev);

    struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
    struct ibv_cq *cq = ibv_create_cq(id->verbs, 16, NULL, NULL, 0);
    create_qp(id, pd, cq);

    // The memory the client may touch. First 8 bytes double as an atomic counter, the rest is a text area.
    char *mem = aligned_alloc(4096, REGION);
    memset(mem, 0, REGION);
    // The REMOTE_* flags authorize the peer; grant the minimum the protocol needs.
    struct ibv_mr *mr = ibv_reg_mr(pd, mem, REGION,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                                       IBV_ACCESS_REMOTE_ATOMIC);
    CHECK_PTR(mr, "reg_mr");
    uint64_t *counter = (uint64_t *)mem;
    *counter = 1000;
    strcpy(mem + 64, "server's original text");

    // WRITE_WITH_IMM consumes a receive WR on this side, so post one before accepting.
    char dummy[8];
    struct ibv_mr *dmr = ibv_reg_mr(pd, dummy, sizeof dummy, IBV_ACCESS_LOCAL_WRITE);
    struct ibv_sge sge = {.addr = (uintptr_t)dummy, .length = sizeof dummy, .lkey = dmr->lkey};
    struct ibv_recv_wr *bad;
    struct ibv_recv_wr rwr = {.wr_id = 7, .sg_list = &sge, .num_sge = 1};
    CHECK_ZERO(ibv_post_recv(id->qp, &rwr, &bad), "post_recv");

    // Accept and hand the client the (addr, rkey, len) triple as private data.
    struct region_info ri = {.addr = (uintptr_t)mem, .rkey = mr->rkey, .len = REGION};
    struct rdma_conn_param p = {.responder_resources = 4,
                                .initiator_depth = 4,
                                .rnr_retry_count = 7,
                                .private_data = &ri,
                                .private_data_len = sizeof ri};
    CHECK_ZERO(rdma_accept(id, &p), "accept");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_ESTABLISHED));
    printf("server: client connected. Memory: addr=%p rkey=0x%x; the client performs the operations.\n",
           (void *)mem, mr->rkey);

    // The only thing the server CPU does: wait for the final WRITE_WITH_IMM notification.
    struct ibv_wc wc = wait_completion(cq, "write_with_imm notification");
    printf("server: notified (opcode=%s imm=%u, %u bytes)\n",
           wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM ? "RECV_RDMA_WITH_IMM" : "?", ntohl(wc.imm_data),
           wc.byte_len);
    printf("server: my memory now holds:\n  counter = %lu\n  text    = \"%s\"\n  text2   = \"%s\"\n",
           (unsigned long)*counter, mem + 64, mem + 128);
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_DISCONNECTED));
}

// ------------------------------------------------------------------------------------------------ client
static void run_client(const char *ip, const char *port) {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *id;
    CHECK_ZERO(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP), "create_id");
    struct addrinfo *ai;
    CHECK_ZERO(getaddrinfo(ip, port, &(struct addrinfo){.ai_family = AF_INET}, &ai), "gai");
    CHECK_ZERO(rdma_resolve_addr(id, NULL, ai->ai_addr, 2000), "resolve_addr");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED));
    CHECK_ZERO(rdma_resolve_route(id, 2000), "resolve_route");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED));

    struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
    struct ibv_cq *cq = ibv_create_cq(id->verbs, 16, NULL, NULL, 0);
    create_qp(id, pd, cq);

    // Local scratch buffer: source of WRITEs, destination of READs and of the atomic's old value.
    // The local MR needs only LOCAL_WRITE (the NIC writes READ results into it).
    char *loc = aligned_alloc(4096, 4096);
    memset(loc, 0, 4096);
    struct ibv_mr *mr = ibv_reg_mr(pd, loc, 4096, IBV_ACCESS_LOCAL_WRITE);
    CHECK_PTR(mr, "reg_mr");

    // initiator_depth / responder_resources = how many READ/atomic ops may be in flight (one at a time here).
    struct rdma_conn_param p = {
        .responder_resources = 4, .initiator_depth = 4, .retry_count = 7, .rnr_retry_count = 7};
    CHECK_ZERO(rdma_connect(id, &p), "connect");
    struct rdma_cm_event *ev = expect_event(ec, RDMA_CM_EVENT_ESTABLISHED);
    // The server's private data arrives in the ESTABLISHED event. Copy it before acking (ack frees the event).
    if (ev->param.conn.private_data_len < sizeof(struct region_info))
        die("server sent no region info");
    struct region_info ri;
    memcpy(&ri, ev->param.conn.private_data, sizeof ri);
    rdma_ack_cm_event(ev);
    printf("client: remote memory addr=0x%lx rkey=0x%x len=%u\n", (unsigned long)ri.addr, ri.rkey, ri.len);

    struct ibv_sge sge = {.addr = (uintptr_t)loc, .lkey = mr->lkey};
    struct ibv_send_wr wr;

    // --- 1. RDMA READ the server's text (server CPU idle) --------------------------------------------
    sge.length = 32;
    wr = (struct ibv_send_wr){.wr_id = 1,
                              .sg_list = &sge,
                              .num_sge = 1,
                              .opcode = IBV_WR_RDMA_READ,
                              .send_flags = IBV_SEND_SIGNALED,
                              .wr.rdma = {.remote_addr = ri.addr + 64, .rkey = ri.rkey}};
    post_and_wait(id->qp, cq, &wr, "rdma read");
    printf("client: RDMA READ  -> \"%s\"\n", loc);

    // --- 2. RDMA WRITE new text into the server's memory ---------------------------------------------
    strcpy(loc, "overwritten remotely by the client");
    sge.length = strlen(loc) + 1;
    wr = (struct ibv_send_wr){.wr_id = 2,
                              .sg_list = &sge,
                              .num_sge = 1,
                              .opcode = IBV_WR_RDMA_WRITE,
                              .send_flags = IBV_SEND_SIGNALED,
                              .wr.rdma = {.remote_addr = ri.addr + 64, .rkey = ri.rkey}};
    post_and_wait(id->qp, cq, &wr, "rdma write");
    printf("client: RDMA WRITE done\n");

    // --- 3. ATOMIC fetch-and-add: increments the server's 8-byte counter, returns the OLD value locally --
    // Atomics are executed by the target NIC; many clients can safely increment the same counter with no locks.
    sge.length = 8;
    memset(loc, 0, 8);
    wr = (struct ibv_send_wr){.wr_id = 3,
                              .sg_list = &sge,
                              .num_sge = 1,
                              .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD,
                              .send_flags = IBV_SEND_SIGNALED,
                              .wr.atomic = {.remote_addr = ri.addr, .compare_add = 5, .rkey = ri.rkey}};
    post_and_wait(id->qp, cq, &wr, "fetch-and-add");
    printf("client: FETCH_AND_ADD(+5): old counter value was %lu\n", (unsigned long)*(uint64_t *)loc);

    // --- 4. latency of one-sided READs ----------------------------------------------------------------
    sge.length = 64;
    wr = (struct ibv_send_wr){.wr_id = 4,
                              .sg_list = &sge,
                              .num_sge = 1,
                              .opcode = IBV_WR_RDMA_READ,
                              .send_flags = IBV_SEND_SIGNALED,
                              .wr.rdma = {.remote_addr = ri.addr, .rkey = ri.rkey}};
    int N = 2000;
    double t0 = now_us();
    for (int i = 0; i < N; i++)
        post_and_wait(id->qp, cq, &wr, "read loop");
    printf("client: %d x 64-byte RDMA READ: %.1f us each (server CPU not involved)\n", N,
           (now_us() - t0) / N);

    // --- 5. WRITE_WITH_IMM: write text2 and notify the server (it has a RECV waiting) ----------------
    strcpy(loc, "this one comes with a doorbell");
    sge.length = strlen(loc) + 1;
    wr = (struct ibv_send_wr){.wr_id = 5,
                              .sg_list = &sge,
                              .num_sge = 1,
                              .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
                              .send_flags = IBV_SEND_SIGNALED,
                              .imm_data = htonl(42),
                              .wr.rdma = {.remote_addr = ri.addr + 128, .rkey = ri.rkey}};
    post_and_wait(id->qp, cq, &wr, "write_with_imm");
    printf("client: WRITE_WITH_IMM sent, disconnecting\n");

    CHECK_ZERO(rdma_disconnect(id), "disconnect");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_DISCONNECTED));
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0); // line-buffered, so logs show up when stdout is a file or pipe
    if (argc >= 2 && !strcmp(argv[1], "server"))
        run_server(argc > 2 ? argv[2] : DEFAULT_PORT);
    else if (argc >= 3 && !strcmp(argv[1], "client"))
        run_client(argv[2], argc > 3 ? argv[3] : DEFAULT_PORT);
    else {
        fprintf(stderr, "usage: %s server [port] | client <ip> [port]\n", argv[0]);
        return 1;
    }
    return 0;
}
