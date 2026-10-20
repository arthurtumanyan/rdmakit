/*
 * driver.h - protocol driver interface.
 *
 * A driver implements one wire protocol (RESP, memcached, native). Each
 * connection is used by exactly one thread and has at most one request in
 * flight. All calls block until the response is complete or an error occurs.
 */
#ifndef PACE_DRIVER_H
#define PACE_DRIVER_H

#include <stddef.h>

/* Result codes returned by driver operations. */
#define PACE_OK 0
#define PACE_NOTFOUND 1
#define PACE_ERR (-1)

typedef struct pace_conn pace_conn;

typedef struct pace_driver {
    const char *name;

    /* Opens a connection. Returns NULL on failure. */
    pace_conn *(*connect)(const char *host, const char *port);

    /* Stores key -> value. Returns PACE_OK or PACE_ERR. */
    int (*put)(pace_conn *c, const void *key, size_t klen, const void *val, size_t vlen);

    /*
     * Reads key into buf (capacity cap) and stores the value length in *vlen.
     * Returns PACE_OK, PACE_NOTFOUND, or PACE_ERR (including values larger than cap).
     */
    int (*get)(pace_conn *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen);

    /* Closes the connection and releases its resources. */
    void (*close)(pace_conn *c);

    /*
     * Optional: sets a driver option (key=value pairs from the command line, applied before any
     * connection is made). Returns 0, or -1 for an unknown key or invalid value. NULL when the driver
     * has no options.
     */
    int (*set_option)(const char *key, const char *value);
} pace_driver;

/* RESP2 over TCP (Redis, Valkey, Dragonfly). Supports SET and GET. */
extern const pace_driver pace_driver_resp;

/* memcached text protocol over TCP. Supports set and get. */
extern const pace_driver pace_driver_memcached;

#ifdef PACE_HAVE_NATIVE
/* Native protocol of the arenakv server (akvproto) over rdmaduct. Options: transport, wait, max-msg, depth. */
extern const pace_driver pace_driver_native;
#endif

/* Returns the driver registered under name, or NULL. */
const pace_driver *pace_driver_find(const char *name);

#endif
