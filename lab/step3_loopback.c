/*
 * STEP 3 - Completion Queues, Queue Pairs, and SEND/RECV (no network needed)
 * ====================================================================
 * The core RDMA objects:
 *
 *   Queue Pair (QP)  = Send Queue + Receive Queue. Work requests (WR) are *posted* to them. The NIC executes
 *                      WRs asynchronously. RC ("Reliable Connection") QP = like TCP: one QP talks to exactly one
 *                      peer QP, in-order, acked, retransmitted by the NIC.
 *   Completion Queue (CQ) = where the NIC reports finished WRs (work completions, WC). Send and receive queues
 *                      each report into a CQ (can be the same one).
 *   Scatter/Gather Entry (SGE) = {addr, length, lkey}: points into a registered MR. A WR holds a list of them.
 *
 * Life of a message:
 *   receiver:  ibv_post_recv(WR -> buffer)       "I have room for a message here"  (must come first)
 *   sender:    ibv_post_send(WR SEND -> buffer)  NIC reads buffer, ships it
 *   receiver:  CQ gets a WC => buffer holds the data.   sender: CQ gets a WC => buffer reusable.
 *
 * A QP must be walked through states before it can transmit:
 *   RESET -> INIT (port, access flags) -> RTR "ready to receive" (who is the peer? its QPN, GID, PSN)
 *         -> RTS "ready to send" (timeouts, retries, local PSN)
 * Normally rdma_cm does this (step 4). Doing it by hand once shows what must be exchanged between two
 * machines: **QP number, GID (address), starting packet sequence number (PSN)**. Here both QPs live in the same
 * process on the same NIC, so both sides can be filled in directly. Soft-RoCE delivers the packets through the
 * normal IP stack (loopback), so this exercises the full RoCEv2 path.
 */
#include "common.h"

#define MSG "hello through the NIC!"

// Find the GID index of the RoCEv2 + IPv4 address (see step 1).
static int find_gid_index(struct ibv_context *ctx, union ibv_gid *out) {
    struct ibv_port_attr pa;
    ibv_query_port(ctx, 1, &pa);
    for (int i = 0; i < pa.gid_tbl_len; i++) {
        struct ibv_gid_entry e;
        if (ibv_query_gid_ex(ctx, 1, i, &e, 0))
            continue;
        if (e.gid_type == IBV_GID_TYPE_ROCE_V2 && !memcmp(e.gid.raw, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12)) {
            *out = e.gid;
            return i;
        }
    }
    die("no RoCEv2 IPv4 GID (does the netdev have an IPv4 address?)");
    return -1;
}

// Create an RC QP and move it RESET -> INIT.
static struct ibv_qp *make_qp(struct ibv_pd *pd, struct ibv_cq *cq) {
    struct ibv_qp_init_attr ia = {
        .send_cq = cq,
        .recv_cq = cq, // both queues report into the same CQ
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = 16,
                .max_recv_wr = 16, // queue depths
                .max_send_sge = 1,
                .max_recv_sge = 1}, // SGEs per WR
        // sq_sig_all = 0 -> only WRs flagged IBV_SEND_SIGNALED generate a send completion
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &ia); // state: RESET
    CHECK_PTR(qp, "ibv_create_qp");
    struct ibv_qp_attr a = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags =
            IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ, // what the *peer* may do to the local side
    };
    CHECK_ZERO(ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS),
               "RESET->INIT");
    return qp;
}

// INIT -> RTR -> RTS, pointing at the peer described by (qpn, psn, gid).
static void connect_qp(struct ibv_qp *qp, uint32_t peer_qpn, uint32_t peer_psn, union ibv_gid peer_gid,
                       int gid_idx, uint32_t my_psn) {
    struct ibv_qp_attr rtr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024, // must be <= both sides' active MTU
        .dest_qp_num = peer_qpn,
        .rq_psn = peer_psn,      // expect the peer's first packet to carry this PSN
        .max_dest_rd_atomic = 4, // incoming RDMA READs/atomics served in parallel
        .min_rnr_timer = 12,     // back-off requested from the sender when no receive buffer is posted
        .ah_attr =
            {
                // "address handle": how to reach the peer
                .is_global = 1, // RoCE always uses the GRH (global route header = GID based)
                .port_num = 1,
                .grh = {.dgid = peer_gid, .sgid_index = gid_idx, .hop_limit = 64},
            },
    };
    CHECK_ZERO(ibv_modify_qp(qp, &rtr,
                             IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                 IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER),
               "INIT->RTR");
    struct ibv_qp_attr rts = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = my_psn,
        .timeout = 14, // ack timeout = 4.096us * 2^14 ~ 67 ms
        .retry_cnt = 7,
        .rnr_retry = 7,     // 7 = retry forever on "receiver not ready"
        .max_rd_atomic = 4, // outstanding RDMA READs as initiator
    };
    CHECK_ZERO(ibv_modify_qp(qp, &rts,
                             IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                 IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC),
               "RTR->RTS");
}

int main(void) {
    struct ibv_device **list = ibv_get_device_list(NULL);
    CHECK_PTR(list && list[0], "no RDMA device");
    struct ibv_context *ctx = ibv_open_device(list[0]);
    CHECK_PTR(ctx, "open");
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    CHECK_PTR(pd, "pd");

    // CQ with room for 16 completions. (NULL, 0 = no completion channel, no comp vector: polling only.)
    struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    CHECK_PTR(cq, "ibv_create_cq");

    // One registered buffer, split in two halves: [0..255] used by the sender, [256..511] by the receiver.
    char *buf = aligned_alloc(4096, 4096);
    memset(buf, 0, 4096);
    struct ibv_mr *mr =
        ibv_reg_mr(pd, buf, 4096, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    CHECK_PTR(mr, "reg_mr");
    char *send_buf = buf;
    char *recv_buf = buf + 256;

    // Two QPs, "A" (sender) and "B" (receiver), and the data each needs to know about the other.
    union ibv_gid gid;
    int gid_idx = find_gid_index(ctx, &gid);
    struct ibv_qp *A = make_qp(pd, cq);
    struct ibv_qp *B = make_qp(pd, cq);
    printf("QP A: qpn=%u   QP B: qpn=%u   using GID index %d\n", A->qp_num, B->qp_num, gid_idx);
    connect_qp(A, B->qp_num, 200, gid, gid_idx, 100); // A sends with PSN 100, expects B's PSN 200
    connect_qp(B, A->qp_num, 100, gid, gid_idx, 200);
    printf("both QPs are in RTS\n");

    // ---- SEND / RECV -----------------------------------------------------------------------------------
    // 1) The receiver posts a receive buffer first. A SEND that arrives with no receive posted causes a receiver-not-ready (RNR) error.
    struct ibv_sge rsge = {.addr = (uintptr_t)recv_buf, .length = 256, .lkey = mr->lkey};
    struct ibv_recv_wr *bad_r;
    struct ibv_recv_wr rwr = {.wr_id = 0xB0B, .sg_list = &rsge, .num_sge = 1};
    CHECK_ZERO(ibv_post_recv(B, &rwr, &bad_r), "ibv_post_recv");

    // 2) sender posts the SEND. wr_id is an opaque cookie returned in the completion to tell work requests apart.
    strcpy(send_buf, MSG);
    struct ibv_sge ssge = {.addr = (uintptr_t)send_buf, .length = sizeof(MSG), .lkey = mr->lkey};
    struct ibv_send_wr *bad_s;
    struct ibv_send_wr swr = {.wr_id = 0xA11CE,
                              .sg_list = &ssge,
                              .num_sge = 1,
                              .opcode = IBV_WR_SEND,
                              .send_flags = IBV_SEND_SIGNALED};
    double t0 = now_us();
    CHECK_ZERO(ibv_post_send(A, &swr, &bad_s), "ibv_post_send");

    // 3) two completions arrive: one for A's send, one for B's receive (order not guaranteed between QPs).
    for (int i = 0; i < 2; i++) {
        struct ibv_wc wc = wait_completion(cq, "send/recv");
        printf("completion wr_id=0x%lx opcode=%s byte_len=%u\n", (unsigned long)wc.wr_id,
               wc.opcode == IBV_WC_RECV ? "RECV" : "SEND", wc.byte_len);
    }
    printf("round trip %.0f us; receiver got: \"%s\"\n\n", now_us() - t0, recv_buf);

    // ---- bonus: RDMA WRITE - A writes straight into B's buffer, B's CPU/receive queue is NOT involved ---
    // This is the "R" in RDMA and the topic of step 5. A needs the buffer address and rkey of B (known here: same process).
    strcpy(send_buf, "written by one-sided RDMA WRITE");
    ssge.length = strlen(send_buf) + 1;
    struct ibv_send_wr w = {.wr_id = 1,
                            .sg_list = &ssge,
                            .num_sge = 1,
                            .opcode = IBV_WR_RDMA_WRITE,
                            .send_flags = IBV_SEND_SIGNALED,
                            .wr.rdma = {.remote_addr = (uintptr_t)recv_buf, .rkey = mr->rkey}};
    memset(recv_buf, 0, 256);
    CHECK_ZERO(ibv_post_send(A, &w, &bad_s), "post rdma write");
    wait_completion(cq, "rdma write"); // only the sender gets a completion
    printf("after RDMA WRITE, B's buffer holds: \"%s\"  (B posted no receive!)\n", recv_buf);

    // ---- cleanup (reverse order) -----------------------------------------------------------------------
    ibv_destroy_qp(A);
    ibv_destroy_qp(B);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(list);
    return 0;
}
