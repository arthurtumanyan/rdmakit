/*
 * duct_common.h - helpers shared by lab steps 7 to 12, which use the rdmaduct library instead of libibverbs.
 *
 * Every step accepts the same options after its mode word:
 *   --transport tcp|rdma   transport of the context (default: tcp; rdma needs an RDMA device, and the server
 *                          address must then be that of the RDMA interface, not 127.0.0.1)
 *   --wait poll|event      how blocking calls wait on RDMA (default: poll); TCP always sleeps in poll()
 *   --port PORT            listening or connecting port (default: the step's own)
 * Steps add their own options; duct_lab_parse() stops at the first argument it does not know and returns its
 * index.
 *
 * Errors: rdmaduct returns 0 or a negative code; duct_strerror() names it. lab_check() exits with the name of the
 * failing call and that text, which is the error handling the steps use for calls that must not fail.
 */
#ifndef LAB_DUCT_COMMON_H
#define LAB_DUCT_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdmaduct.h"

struct lab_opts {
    enum duct_transport transport;
    enum duct_wait wait;
    const char *port;
};

/* Exits with status 1 when rc is an error; what names the call. */
static inline void lab_check(int rc, const char *what) {
    if (rc < 0) {
        fprintf(stderr, "%s: %s\n", what, duct_strerror(rc));
        exit(1);
    }
}

/* Parses the common options from argv[i] on. Returns the index of the first argument not consumed. */
static inline int duct_lab_parse(int argc, char **argv, int i, struct lab_opts *o) {
    while (i + 1 < argc) {
        if (strcmp(argv[i], "--transport") == 0) {
            if (strcmp(argv[i + 1], "tcp") == 0) {
                o->transport = DUCT_TRANSPORT_TCP;
            } else if (strcmp(argv[i + 1], "rdma") == 0) {
                o->transport = DUCT_TRANSPORT_RDMA;
            } else {
                fprintf(stderr, "--transport: tcp or rdma\n");
                exit(2);
            }
        } else if (strcmp(argv[i], "--wait") == 0) {
            if (strcmp(argv[i + 1], "poll") == 0) {
                o->wait = DUCT_WAIT_POLL;
            } else if (strcmp(argv[i + 1], "event") == 0) {
                o->wait = DUCT_WAIT_EVENT;
            } else {
                fprintf(stderr, "--wait: poll or event\n");
                exit(2);
            }
        } else if (strcmp(argv[i], "--port") == 0) {
            o->port = argv[i + 1];
        } else {
            break;
        }
        i += 2;
    }
    return i;
}

/* Fills d with the library defaults and the transport and waiting mode of o. */
static inline void lab_duct_opts(const struct lab_opts *o, struct duct_opts *d) {
    duct_opts_default(d);
    d->transport = o->transport;
    d->wait = o->wait;
}

/* Name of a transport for printing. */
static inline const char *lab_transport_name(enum duct_transport t) {
    return t == DUCT_TRANSPORT_RDMA ? "rdma" : "tcp";
}

/* Monotonic time in seconds. */
static inline double lab_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif
