/* test_tcp.c - echo suite over the TCP transport on the loopback interface. */
#include "suite.h"

int main(void) {
    int rc = run_echo_suite(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1");
    rc |= run_stream_stress(DUCT_TRANSPORT_TCP, DUCT_WAIT_POLL, "127.0.0.1", 50000);
    if (rc == 0) {
        puts("test_tcp: ok");
    }
    return rc;
}
