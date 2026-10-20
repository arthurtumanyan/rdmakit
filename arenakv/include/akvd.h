/*
 * akvd.h - key-value server: serves the akvproto protocol from a akv engine over rdmaduct.
 *
 * One thread accepts connections; each connection is served by its own thread, which processes requests in
 * order (receive, execute on the engine, send the response). The server owns its engine and transport context.
 */
#ifndef AKVD_H
#define AKVD_H

#include <stdint.h>

#include "akv.h"
#include "rdmaduct.h"

typedef struct akvd akvd;

typedef struct akvd_config {
    akv_config store;
    enum duct_transport transport;
    enum duct_wait wait;
    const char *addr;    /* listen address; NULL = all IPv4 addresses */
    const char *port;    /* listen port; "0" selects a free port */
    uint32_t max_msg;    /* largest request/response message; 0 selects 64 KiB */
    uint32_t recv_depth; /* receive buffers per connection; 0 selects 16 */
    uint32_t send_depth; /* send buffers per connection; 0 selects 16 */
} akvd_config;

/* Fills cfg with defaults (see field comments, 256 MiB store, AUTO transport, port 7600). */
void akvd_config_default(akvd_config *cfg);

/* Creates the engine, starts listening and starts the accept thread. Returns NULL on failure (errno-style code in *err if given). */
akvd *akvd_start(const akvd_config *cfg, int *err);

/* Port the server listens on. */
int akvd_port(const akvd *d);

/* Engine of the server (for statistics). */
akv *akvd_store(akvd *d);

/* Number of connections currently served. */
unsigned akvd_connections(const akvd *d);

/* Stops accepting, closes all connections, waits for the threads and frees everything. */
void akvd_stop(akvd *d);

#endif
