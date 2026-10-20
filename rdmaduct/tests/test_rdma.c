/*
 * test_rdma.c - echo suite over the RDMA transport.
 *
 * The receiver-not-ready check compares device-wide hardware counters, so it is only meaningful while no
 * other process uses the same device.
 *
 * DUCT_TEST_ADDR must hold the IPv4 address of a network interface that carries
 * an RDMA device (for Soft-RoCE: the address of the rxe netdev). The test
 * prints "skipped" and succeeds when RDMA support is not compiled in or the
 * address is not set.
 */
#include <stdlib.h>
#include <time.h>
#ifdef DUCT_HAVE_RDMA
#include <infiniband/verbs.h>
#endif

#include "suite.h"

#ifdef DUCT_HAVE_RDMA
struct idle_server {
    duct_listener *listener;
};

/* Accepts one connection and waits for a message that never arrives. */
static void *idle_server_main(void *arg) {
    struct idle_server *s = arg;
    duct_ep *ep;
    if (duct_accept(s->listener, 5000, &ep) == 0) {
        struct duct_rx rx;
        duct_recv(ep, &rx, 600);
        duct_ep_close(ep);
    }
    return NULL;
}

/* Process CPU time in milliseconds. */
static double cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/*
 * Measures the CPU time consumed by two endpoints (one per thread) that both wait 400-600 ms for a
 * message that never arrives. Returns milliseconds of process CPU time.
 */
static double idle_cpu_ms(const char *addr, enum duct_wait wait) {
    struct duct_opts opts;
    duct_opts_default(&opts);
    opts.transport = DUCT_TRANSPORT_RDMA;
    opts.wait = wait;
    opts.max_msg = 4096;
    opts.recv_depth = 8;
    opts.send_depth = 8;
    duct_ctx *ctx;
    struct idle_server srv;
    duct_ep *ep;
    if (duct_ctx_create(&opts, &ctx) != 0 || duct_listen(ctx, addr, "0", &srv.listener) != 0) {
        return -1;
    }
    char port[16];
    snprintf(port, sizeof(port), "%d", duct_listener_port(srv.listener));
    pthread_t th;
    pthread_create(&th, NULL, idle_server_main, &srv);
    if (duct_connect(ctx, addr, port, &ep) != 0) {
        return -1;
    }
    double t0 = cpu_ms();
    struct duct_rx rx;
    int rc = duct_recv(ep, &rx, 400);
    double used = cpu_ms() - t0;
    EXPECT(rc == -ETIMEDOUT, "idle receive returned %s", duct_strerror(rc));
    pthread_join(th, NULL);
    duct_ep_close(ep);
    duct_listener_close(srv.listener);
    duct_ctx_destroy(ctx);
    return used;
}

/* Reads a hardware counter of the first RDMA device; returns -1 when unavailable. */
static long read_counter(const char *name) {
    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (list == NULL || n == 0) {
        return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/1/hw_counters/%s",
             ibv_get_device_name(list[0]), name);
    ibv_free_device_list(list);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) {
        v = -1;
    }
    fclose(f);
    return v;
}
#endif

int main(void) {
#ifdef DUCT_HAVE_RDMA
    const char *addr = getenv("DUCT_TEST_ADDR");
    if (addr == NULL || addr[0] == '\0') {
        puts("test_rdma: skipped (DUCT_TEST_ADDR not set)");
        return 0;
    }
    long rnr0 = read_counter("send_rnr_err");
    long rcv0 = read_counter("rcvd_rnr_err");
    int rc = 0;
    for (int w = 0; w < 2; w++) {
        enum duct_wait wait = w == 0 ? DUCT_WAIT_POLL : DUCT_WAIT_EVENT;
        rc |= run_echo_suite(DUCT_TRANSPORT_RDMA, wait, addr);
    }
    const char *env = getenv("DUCT_STRESS_COUNT");
    uint64_t count = env != NULL ? strtoull(env, NULL, 10) : 300000;
    rc |= run_stream_stress(DUCT_TRANSPORT_RDMA, DUCT_WAIT_POLL, addr, count);
    rc |= run_stream_stress(DUCT_TRANSPORT_RDMA, DUCT_WAIT_EVENT, addr, count);
    double spin = idle_cpu_ms(addr, DUCT_WAIT_POLL);
    double sleep_ms = idle_cpu_ms(addr, DUCT_WAIT_EVENT);
    printf("idle CPU over 400 ms with two waiting threads: poll %.0f ms, event %.0f ms\n", spin, sleep_ms);
    EXPECT(spin > 300.0, "polling wait used only %.0f ms of CPU", spin);
    EXPECT(sleep_ms >= 0.0 && sleep_ms < 60.0, "event wait used %.0f ms of CPU", sleep_ms);
    long rnr1 = read_counter("send_rnr_err");
    long rcv1 = read_counter("rcvd_rnr_err");
    if (rnr0 >= 0 && rcv0 >= 0) {
        EXPECT(rnr1 == rnr0 && rcv1 == rcv0,
               "receiver-not-ready errors: send %ld -> %ld, received %ld -> %ld", rnr0, rnr1, rcv0, rcv1);
        rc |= suite_failures != 0;
    } else {
        puts("test_rdma: RNR counters unavailable, check skipped");
    }
    if (rc == 0) {
        puts("test_rdma: ok");
    }
    return rc;
#else
    puts("test_rdma: skipped (built without RDMA)");
    return 0;
#endif
}
