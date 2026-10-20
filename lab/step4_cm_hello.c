/*
 * STEP 4 - Connecting two machines with rdma_cm, and a two-sided SEND/RECV exchange
 * ================================================================================
 * Step 3 hand-wired two QPs. Between machines the QPN/GID/PSN exchange is done by the RDMA
 * Connection Manager (librdmacm) does. It looks like TCP sockets:
 *
 *     server                                  client
 *     rdma_create_id / rdma_bind_addr         rdma_create_id
 *     rdma_listen                             rdma_resolve_addr   (IP -> local RDMA device + GID)   ADDR_RESOLVED
 *                                             rdma_resolve_route  (RoCE: trivial)                   ROUTE_RESOLVED
 *     <- CONNECT_REQUEST event                [create PD, CQ, QP, MR; post a RECV]
 *     [create PD, CQ, QP, MR; post a RECV]    rdma_connect
 *     rdma_accept                             -> ESTABLISHED event
 *     -> ESTABLISHED event
 *
 * rdma_cm is event driven: every step completes asynchronously and reports through an "event channel" (an fd).
 * rdma_cm also moves the QP through INIT/RTR/RTS for the application (rdma_create_qp + connect/accept).
 *
 * Rule: post the RECV before the peer can send. Both sides therefore post it before
 * connect/accept. If a SEND arrives with no receive buffer the NIC answers RNR (receiver-not-ready) and retries.
 *
 * Usage:   step4_cm_hello server [port]            (on node A)
 *          step4_cm_hello client <server-ip> [port] (on node B)
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include "common.h"

#define DEFAULT_PORT "7471"
#define BUF 256

// Wait for the next CM event, check that it is the expected one, return it (the caller must call rdma_ack_cm_event).
static struct rdma_cm_event *expect_event(struct rdma_event_channel *ec, enum rdma_cm_event_type want) {
    struct rdma_cm_event *ev;
    CHECK_ZERO(rdma_get_cm_event(ec, &ev), "rdma_get_cm_event"); // blocks
    if (ev->event != want)
        die("unexpected CM event %s (wanted %s), status=%d", rdma_event_str(ev->event), rdma_event_str(want),
            ev->status);
    return ev;
}

// Everything one connection needs. Created after the id knows which device it is bound to (id->verbs).
struct conn {
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *mr;
    char *buf; // buf[0..BUF) = recv, buf[BUF..2*BUF) = send
};

static void conn_setup(struct conn *c, struct rdma_cm_id *id) {
    c->id = id;
    c->pd = ibv_alloc_pd(id->verbs);
    CHECK_PTR(c->pd, "alloc_pd");
    c->cq = ibv_create_cq(id->verbs, 16, NULL, NULL, 0);
    CHECK_PTR(c->cq, "create_cq");
    c->buf = aligned_alloc(4096, 4096);
    memset(c->buf, 0, 4096);
    c->mr = ibv_reg_mr(c->pd, c->buf, 4096, IBV_ACCESS_LOCAL_WRITE);
    CHECK_PTR(c->mr, "reg_mr");
    struct ibv_qp_init_attr qa = {
        .send_cq = c->cq,
        .recv_cq = c->cq,
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = 8, .max_recv_wr = 8, .max_send_sge = 1, .max_recv_sge = 1}};
    // rdma_create_qp creates the QP *and* attaches it to the id; it stays in INIT until connect/accept.
    CHECK_ZERO(rdma_create_qp(id, c->pd, &qa), "rdma_create_qp");
    // post the receive buffer now (see rule above)
    struct ibv_sge sge = {.addr = (uintptr_t)c->buf, .length = BUF, .lkey = c->mr->lkey};
    struct ibv_recv_wr *bad;
    struct ibv_recv_wr wr = {.wr_id = 1, .sg_list = &sge, .num_sge = 1};
    CHECK_ZERO(ibv_post_recv(id->qp, &wr, &bad), "post_recv");
}

static void conn_send(struct conn *c, const char *msg) {
    char *sb = c->buf + BUF;
    snprintf(sb, BUF, "%s", msg);
    struct ibv_sge sge = {.addr = (uintptr_t)sb, .length = strlen(sb) + 1, .lkey = c->mr->lkey};
    struct ibv_send_wr *bad;
    struct ibv_send_wr wr = {
        .wr_id = 2, .sg_list = &sge, .num_sge = 1, .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED};
    CHECK_ZERO(ibv_post_send(c->id->qp, &wr, &bad), "post_send");
    wait_completion(c->cq, "send");
}

static void conn_recv(struct conn *c) {
    struct ibv_wc wc = wait_completion(c->cq, "recv");
    printf("  received %u bytes: \"%s\"\n", wc.byte_len, c->buf);
}

static void conn_destroy(struct conn *c) {
    rdma_destroy_qp(c->id);
    ibv_dereg_mr(c->mr);
    free(c->buf);
    ibv_destroy_cq(c->cq);
    ibv_dealloc_pd(c->pd);
    rdma_destroy_id(c->id);
}

// ------------------------------------------------------------------------------------------------ server
static void run_server(const char *port) {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    CHECK_PTR(ec, "event channel");
    struct rdma_cm_id *listener;
    CHECK_ZERO(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP), "create_id");
    // RDMA_PS_TCP = reliable connected service (RC QPs), the RDMA equivalent of SOCK_STREAM.
    struct addrinfo *ai;
    CHECK_ZERO(getaddrinfo(NULL, port, &(struct addrinfo){.ai_flags = AI_PASSIVE, .ai_family = AF_INET}, &ai),
               "getaddrinfo");
    CHECK_ZERO(rdma_bind_addr(listener, ai->ai_addr), "rdma_bind_addr");
    CHECK_ZERO(rdma_listen(listener, 8), "rdma_listen");
    printf("server: listening on port %s\n", port);

    for (;;) { // serve connections one after another
        // The new connection arrives as event CONNECT_REQUEST carrying a NEW id (like accept() returning a new fd).
        struct rdma_cm_event *ev = expect_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST);
        struct rdma_cm_id *id = ev->id;
        rdma_ack_cm_event(ev);
        printf("server: connection request\n");
        struct conn c;
        conn_setup(&c, id);
        struct rdma_conn_param p = {.responder_resources = 1, .initiator_depth = 1, .rnr_retry_count = 7};
        CHECK_ZERO(rdma_accept(id, &p), "rdma_accept");
        ev = expect_event(ec, RDMA_CM_EVENT_ESTABLISHED);
        rdma_ack_cm_event(ev);
        printf("server: ESTABLISHED\n");

        conn_recv(&c); // wait for the client's greeting
        conn_send(&c, "hello client, greetings from the server over RDMA!");

        ev = expect_event(ec, RDMA_CM_EVENT_DISCONNECTED);
        rdma_ack_cm_event(ev); // client hung up
        printf("server: client disconnected\n");
        conn_destroy(&c);
    }
}

// ------------------------------------------------------------------------------------------------ client
static void run_client(const char *ip, const char *port) {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    CHECK_PTR(ec, "event channel");
    struct rdma_cm_id *id;
    CHECK_ZERO(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP), "create_id");
    struct addrinfo *ai;
    CHECK_ZERO(getaddrinfo(ip, port, &(struct addrinfo){.ai_family = AF_INET}, &ai), "getaddrinfo");

    // Resolve: which local RDMA device + GID reaches this IP? (looks up the routing table & neighbour cache)
    CHECK_ZERO(rdma_resolve_addr(id, NULL, ai->ai_addr, 2000), "rdma_resolve_addr");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED));
    CHECK_ZERO(rdma_resolve_route(id, 2000), "rdma_resolve_route");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED));
    printf("client: resolved, using device %s\n", ibv_get_device_name(id->verbs->device));

    struct conn c;
    conn_setup(&c, id);
    struct rdma_conn_param p = {
        .responder_resources = 1, .initiator_depth = 1, .retry_count = 7, .rnr_retry_count = 7};
    CHECK_ZERO(rdma_connect(id, &p), "rdma_connect");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_ESTABLISHED));
    printf("client: ESTABLISHED\n");

    double t0 = now_us();
    conn_send(&c, "hello server, this is the client");
    conn_recv(&c);
    printf("client: request/response took %.0f us\n", now_us() - t0);

    CHECK_ZERO(rdma_disconnect(id), "rdma_disconnect");
    rdma_ack_cm_event(expect_event(ec, RDMA_CM_EVENT_DISCONNECTED));
    conn_destroy(&c);
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
