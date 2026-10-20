// common.h - tiny helpers shared by all steps, so the step files can concentrate on RDMA itself.
#ifndef COMMON_H
#define COMMON_H
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <infiniband/verbs.h>

// Print an error (with errno text) and exit. Verbs functions return 0 / NULL and set errno.
static inline void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ERROR: ");
    vfprintf(stderr, fmt, ap);
    if (errno)
        fprintf(stderr, " (%s)", strerror(errno));
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
// CHECK(ptr_or_value, "what failed"): die when a pointer is NULL or an int is non-zero.
#define CHECK_PTR(x, msg)                                                                                    \
    do {                                                                                                     \
        errno = 0;                                                                                           \
        if (!(x))                                                                                            \
            die("%s", msg);                                                                                  \
    } while (0)
#define CHECK_ZERO(x, msg)                                                                                   \
    do {                                                                                                     \
        errno = 0;                                                                                           \
        int _r = (x);                                                                                        \
        if (_r) {                                                                                            \
            errno = _r > 0 ? _r : errno;                                                                     \
            die("%s", msg);                                                                                  \
        }                                                                                                    \
    } while (0)

static inline double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

// Busy-poll a completion queue until one work completion (WC) shows up, then check its status.
// A WC is the "receipt" the NIC writes into the CQ when a work request (WR) finished.
// Polling is the lowest-latency way to wait; it burns a CPU core while waiting (step 6 waits on a completion channel instead).
static inline struct ibv_wc wait_completion(struct ibv_cq *cq, const char *what) {
    struct ibv_wc wc;
    int n;
    double t0 = now_us();
    while ((n = ibv_poll_cq(cq, 1, &wc)) == 0)
        if (now_us() - t0 > 5e6)
            die("timeout (5 s) waiting for completion of: %s", what);
    if (n < 0)
        die("ibv_poll_cq failed");
    if (wc.status != IBV_WC_SUCCESS)
        die("%s failed: %s (vendor_err=0x%x)", what, ibv_wc_status_str(wc.status), wc.vendor_err);
    return wc;
}
#endif
