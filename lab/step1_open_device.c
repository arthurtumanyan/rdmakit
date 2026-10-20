/*
 * STEP 1 - Find and open an RDMA device
 * =====================================
 * Everything in RDMA starts with a *device context* (struct ibv_context): the handle to the NIC.
 * On real hardware that is a Mellanox/Broadcom/... card; here it is "rxe0", the Soft-RoCE device the kernel
 * built on top of an ordinary Ethernet port. The API and the semantics are the same; performance is lower.
 *
 * What this program does:
 *   1. ibv_get_device_list()  - lists the RDMA devices known to libibverbs (reads /sys/class/infiniband).
 *   2. ibv_open_device()      - open one => ibv_context. All later objects (PD, CQ, QP, MR) hang off it.
 *   3. ibv_query_device()     - device capabilities (max QPs, max MR size, ...).
 *   4. ibv_query_port()       - state of the physical port (ACTIVE?, MTU, link layer).
 *   5. ibv_query_gid_ex()     - GIDs: the "addresses" of a RoCE port. For RoCE a GID is derived from
 *                               the interface's IP address. A remote peer needs the local GID to reach the port.
 *
 * Build/run:   make && build/$(uname -m)/step1_open_device [device-name]
 */
#include "common.h"

int main(int argc, char **argv) {
    // ---- 1. list devices -------------------------------------------------------------------------------
    int num;
    struct ibv_device **list = ibv_get_device_list(&num);
    CHECK_PTR(list, "ibv_get_device_list (is the rdma_rxe module loaded? try: rdma link show)");
    if (num == 0)
        die("no RDMA devices found");
    printf("Found %d RDMA device(s):\n", num);
    for (int i = 0; i < num; i++)
        printf("  [%d] %-8s node_guid=%016lx\n", i, ibv_get_device_name(list[i]),
               (unsigned long)__builtin_bswap64(ibv_get_device_guid(list[i])));

    // Pick the one named on the command line, or the first one.
    struct ibv_device *dev = list[0];
    if (argc > 1) {
        dev = NULL;
        for (int i = 0; i < num; i++)
            if (!strcmp(ibv_get_device_name(list[i]), argv[1]))
                dev = list[i];
        if (!dev)
            die("device %s not found", argv[1]);
    }

    // ---- 2. open it ------------------------------------------------------------------------------------
    // This talks to the kernel driver (/dev/infiniband/uverbs0) and returns a context.
    struct ibv_context *ctx = ibv_open_device(dev);
    CHECK_PTR(ctx, "ibv_open_device (permission on /dev/infiniband/uverbs*?)");
    printf("\nOpened %s\n", ibv_get_device_name(dev));

    // ---- 3. device capabilities ------------------------------------------------------------------------
    // Always query limits instead of hard-coding sizes: Soft-RoCE and a ConnectX-7 differ a lot.
    struct ibv_device_attr da;
    CHECK_ZERO(ibv_query_device(ctx, &da), "ibv_query_device");
    printf("\nDevice limits:\n");
    printf("  max_qp=%d  max_cq=%d  max_cqe=%d  max_mr=%d\n", da.max_qp, da.max_cq, da.max_cqe, da.max_mr);
    printf("  max_mr_size=%lu MiB  max_qp_wr=%d  max_sge=%d\n", (unsigned long)(da.max_mr_size >> 20),
           da.max_qp_wr, da.max_sge);
    printf("  max_qp_rd_atom=%d (outstanding RDMA READs per QP)  phys_port_cnt=%d\n", da.max_qp_rd_atom,
           da.phys_port_cnt);

    // ---- 4. port state ---------------------------------------------------------------------------------
    // Ports are numbered from 1 (not 0). Soft-RoCE has exactly one.
    struct ibv_port_attr pa;
    CHECK_ZERO(ibv_query_port(ctx, 1, &pa), "ibv_query_port");
    printf("\nPort 1:\n  state=%s  active_mtu=%d  link_layer=%s  gid_table_len=%d\n",
           ibv_port_state_str(pa.state), 128 << pa.active_mtu,
           pa.link_layer == IBV_LINK_LAYER_ETHERNET ? "Ethernet (RoCE)" : "InfiniBand", pa.gid_tbl_len);
    // active_mtu is an enum (IBV_MTU_256=1 ... IBV_MTU_4096=5): 128 << enum gives bytes.

    // ---- 5. GIDs ---------------------------------------------------------------------------------------
    // A GID is a 128-bit IPv6-style address. RoCE port => GID table filled from the netdev's IPs:
    //   RoCEv1: raw Ethernet frames (same L2 segment only). RoCEv2: RDMA inside UDP/IP (routable), used throughout this lab.
    // IPv4 addresses appear "v4-mapped" (::ffff:a.b.c.d). The index of the RoCEv2 + IPv4 GID is needed later.
    printf("\nValid GIDs:\n");
    for (int i = 0; i < pa.gid_tbl_len; i++) {
        struct ibv_gid_entry e;
        if (ibv_query_gid_ex(ctx, 1, i, &e, 0))
            continue; // empty slot
        const uint8_t *g = e.gid.raw;
        int v4 = !memcmp(g, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);
        printf("  idx %d  %s  ", i, e.gid_type == IBV_GID_TYPE_ROCE_V2 ? "RoCEv2" : "RoCEv1");
        if (v4)
            printf("IPv4 %d.%d.%d.%d\n", g[12], g[13], g[14], g[15]);
        else
            printf("IPv6 fe80:...%02x%02x:%02x%02x\n", g[12], g[13], g[14], g[15]);
    }

    // ---- cleanup: every ibv_open/alloc has a matching close/free ---------------------------------------
    ibv_close_device(ctx);
    ibv_free_device_list(list);
    return 0;
}
