/*
 * driver.h - protocol driver interface.
 *
 * A driver implements one wire protocol (RESP, memcached, native). It offers two interfaces:
 *
 *   synchronous   one request in flight; every call blocks until its response is complete. Used by the
 *                 one-connection-per-thread runner and by the KV-cache scenario.
 *   asynchronous  non-blocking: requests are encoded into the connection's output (submit), written and
 *                 read by io(), and replies are taken in request order by next(). Used by the multiplexed
 *                 runner (several connections per thread, pipelining).
 *
 * Counter keys: INCR operates on keys with the letter 'c' after the prefix (pace_wl_key_text), created on
 * first use (RESP INCR, memcached "ma ... N0", native INCRS), so that counters never meet the random
 * binary values of the other operations.
 */
#ifndef PACE_DRIVER_H
#define PACE_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "workload.h"

/* Result codes returned by driver operations. */
#define PACE_OK 0
#define PACE_NOTFOUND 1
#define PACE_ERR (-1)

typedef struct pace_conn pace_conn;
typedef struct pace_aconn pace_aconn;

/* Request of the asynchronous interface. Data is copied by submit(). */
typedef struct pace_req {
    pace_op_type type; /* PACE_OP_GET, SET, DEL, INCR or MGET */
    const char *key;
    size_t klen;
    const void *val; /* SET */
    size_t vlen;
    const char *const *keys; /* MGET */
    const size_t *klens;
    unsigned nkeys;
} pace_req;

/* Reply of the asynchronous interface. */
typedef struct pace_reply {
    int status;    /* PACE_OK, PACE_NOTFOUND or PACE_ERR */
    unsigned hits; /* MGET: keys found */
} pace_reply;

typedef struct pace_adriver {
    /* Opens a non-blocking connection (authenticated when credentials are set). NULL on failure. */
    pace_aconn *(*connect)(const char *host, const char *port);
    /* Descriptor to watch with epoll. */
    int (*fd)(pace_aconn *c);
    /* Largest number of outstanding requests the connection accepts; 0 = no limit. */
    unsigned (*window)(pace_aconn *c);
    /* Encodes a request. Returns PACE_OK or PACE_ERR. */
    int (*submit)(pace_aconn *c, const pace_req *r);
    /* Writes pending output and reads available input without blocking. Returns PACE_OK or PACE_ERR. */
    int (*io)(pace_aconn *c);
    /* Takes the reply of the oldest outstanding request: 1 = reply, 0 = incomplete, PACE_ERR = failure. */
    int (*next)(pace_aconn *c, pace_reply *out);
    /* Epoll events to wait for (EPOLLIN, and EPOLLOUT while output is pending); read after arm(). */
    unsigned (*events)(pace_aconn *c);
    /* Prepares waiting: 0 = wait on the descriptor, 1 = progress is possible without waiting. */
    int (*arm)(pace_aconn *c);
    void (*close)(pace_aconn *c);
} pace_adriver;

typedef struct pace_driver {
    const char *name;
    /* Opens a connection (authenticated when credentials are set). Returns NULL on failure. */
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
    /* Optional synchronous operations: PACE_OK, PACE_NOTFOUND or PACE_ERR. */
    int (*del)(pace_conn *c, const void *key, size_t klen);
    int (*incr)(pace_conn *c, const void *key, size_t klen);
    int (*mget)(pace_conn *c, const char *const *keys, const size_t *klens, unsigned n, unsigned *hits);
    /* Asynchronous interface, NULL when the driver has none. */
    const pace_adriver *async;
} pace_driver;

/* RESP2 over TCP or TLS (Redis, Valkey, Dragonfly, arenakv). */
extern const pace_driver pace_driver_resp;
/* memcached text protocol over TCP or TLS (INCR uses the meta command ma). */
extern const pace_driver pace_driver_memcached;
#ifdef PACE_HAVE_NATIVE
/* Native protocol of the arenakv server (akvproto) over rdmaduct. Options: transport, wait, max-msg, depth. */
extern const pace_driver pace_driver_native;
#endif

/* Returns the driver registered under name, or NULL. */
const pace_driver *pace_driver_find(const char *name);

/*
 * Credentials applied by every driver after connecting: RESP "AUTH secret" (or "AUTH user pass" for
 * "user:pass"), memcached ASCII authentication with "user pass" (user "default" without a colon), native
 * AKVP_AUTH. NULL disables authentication.
 */
void pace_driver_set_auth(const char *secret);
const char *pace_driver_auth(void);

/* RESP database selected after connecting (SELECT); 0 sends nothing. */
void pace_driver_set_db(unsigned db);
unsigned pace_driver_db(void);

#endif
