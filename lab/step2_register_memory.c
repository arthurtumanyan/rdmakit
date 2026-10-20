/*
 * STEP 2 - Protection Domain and Memory Registration
 * ==================================================
 * An RDMA NIC reads and writes host memory by DMA, bypassing the CPU and the kernel. For that to be safe and
 * possible the memory must be *registered* first:
 *   - the kernel PINS the pages (they can't be swapped out or moved - hence the unlimited "memlock" ulimit),
 *   - the NIC gets a virtual->physical translation table,
 *   - two keys are returned: lkey is used in local work requests to state that a buffer is allowed;
 *     rkey is handed to a remote peer so that it may RDMA read and write this memory.
 *     Whoever knows (address, rkey) can access that memory, so the rkey is effectively a capability.
 *
 * Objects:
 *   Protection Domain (PD): a container that ties QPs and MRs together. A QP can only use MRs from its own PD.
 *   Memory Region (MR):     the registered buffer.
 *
 * Access flags decide what the NIC is allowed to do with the MR:
 *   LOCAL_WRITE   - NIC may write incoming data (receive buffers, RDMA READ results). Needed for almost everything.
 *   REMOTE_WRITE  - peers may RDMA WRITE into it. (Requires LOCAL_WRITE.)
 *   REMOTE_READ   - peers may RDMA READ from it.
 *   REMOTE_ATOMIC - peers may do atomic fetch-add / compare-swap.
 *
 * This step also measures what registration costs. It is slow (pins and translates every page), which is why
 * real applications register once at startup and reuse the buffers, never per message.
 */
#include <unistd.h>
#include "common.h"

int main(int argc, char **argv) {
    // Open the first device (see step 1 for details).
    struct ibv_device **list = ibv_get_device_list(NULL);
    CHECK_PTR(list && list[0], "no RDMA device");
    struct ibv_context *ctx = ibv_open_device(list[0]);
    CHECK_PTR(ctx, "ibv_open_device");

    // ---- Protection Domain -----------------------------------------------------------------------------
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    CHECK_PTR(pd, "ibv_alloc_pd");
    printf("PD allocated (handle=%u)\n\n", pd->handle);

    // ---- A buffer to register --------------------------------------------------------------------------
    // Use page-aligned memory: pinning works on whole pages, so unaligned buffers waste/share pages.
    size_t len = 1 << 20;              // 1 MiB
    long page = sysconf(_SC_PAGESIZE); // 4096 on x86, 4096 or 16384 on Pis depending on kernel
    void *buf;
    CHECK_ZERO(posix_memalign(&buf, page, len), "posix_memalign");
    memset(buf, 0xAB, len); // touch it so pages really exist

    // ---- Register --------------------------------------------------------------------------------------
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, len, access);
    CHECK_PTR(mr, "ibv_reg_mr (EPERM/ENOMEM => check `ulimit -l`, must be unlimited)");
    printf("Registered %zu bytes at %p\n", len, buf);
    printf("  lkey = 0x%08x   (local use)\n", mr->lkey);
    printf("  rkey = 0x%08x   (give to remote peers together with addr=%p)\n", mr->rkey, buf);
    // A remote peer needs exactly three numbers to write here: address, length limit and rkey.
    // Steps 4 and 5 exchange them over a connection.

    // ---- Rule: REMOTE_WRITE without LOCAL_WRITE is rejected --------------------------------------------
    struct ibv_mr *bad = ibv_reg_mr(pd, buf, len, IBV_ACCESS_REMOTE_WRITE);
    printf("\nRemote-write-only registration %s",
           bad ? "was accepted (unexpected)\n" : "refused (as the spec demands): ");
    if (!bad)
        printf("%s\n", strerror(errno));
    else
        ibv_dereg_mr(bad);

    // ---- Cost of registration --------------------------------------------------------------------------
    printf("\nRegistration cost vs. size (page size %ld):\n", page);
    for (size_t sz = 4096; sz <= (64u << 20); sz *= 16) {
        void *b;
        CHECK_ZERO(posix_memalign(&b, page, sz), "alloc");
        memset(b, 1, sz);
        double t0 = now_us();
        struct ibv_mr *m = ibv_reg_mr(pd, b, sz, access);
        double t1 = now_us();
        if (!m) {
            printf("  %8zu KiB: failed (%s)\n", sz >> 10, strerror(errno));
            free(b);
            break;
        }
        ibv_dereg_mr(m);
        double t2 = now_us();
        printf("  %8zu KiB: reg %9.1f us   dereg %8.1f us\n", sz >> 10, t1 - t0, t2 - t1);
        free(b);
    }
    // Registration cost grows roughly linearly with size: register large pools once, never per request.

    // ---- Cleanup: strict reverse order. A PD can't be freed while MRs still use it (EBUSY). ------------
    ibv_dereg_mr(mr);
    free(buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(list);
    return 0;
}
