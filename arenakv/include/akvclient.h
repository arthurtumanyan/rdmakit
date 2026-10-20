/*
 * akvclient.h - client for the native arenakv protocol (akvproto.h) over rdmaduct.
 *
 * A client is one connection and is used by one thread at a time. The synchronous functions send one
 * request and wait for its response. akvc_post() and akvc_next() pipeline requests: several requests are
 * sent before their responses are read, in order. At most akvc_window() requests may be outstanding;
 * akvc_post() returns -EBUSY beyond that, because the server stops reading requests while its responses
 * cannot be delivered.
 *
 * Return values: a non-negative akvp_status (AKVP_OK, AKVP_NOTFOUND, ...) when the server answered, or a
 * negative rdmaduct or -errno code when the transport failed (the client is then unusable and must be
 * closed).
 *
 * akvc_pool distributes keys over several servers with consistent hashing (160 points per server on a
 * 64-bit ring, as in libketama; the hash function is arenakv's own, so the placement differs from
 * libketama clients). Adding or removing a server moves about 1/n of the keys.
 */
#ifndef AKVCLIENT_H
#define AKVCLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "akvproto.h"
#include "rdmaduct.h"

typedef struct akvc akvc;

/* Connects to a server. opts selects transport and buffer sizes (max_msg bounds every request and response). */
int akvc_connect(akvc **out, const struct duct_opts *opts, const char *host, const char *port);

/* Closes the connection and frees the client. Safe with NULL. */
void akvc_close(akvc *c);

/* Authenticates ("user:password" or a password of the default user). AKVP_NOAUTH when rejected. */
int akvc_auth(akvc *c, const char *credentials);

/* Stores a value unconditionally, without flags or expiry. */
int akvc_put(akvc *c, const void *key, size_t klen, const void *val, size_t vlen);

/*
 * Stores a value with an operation of AKVP_PUT, AKVP_ADD, AKVP_REPLACE, AKVP_APPEND, AKVP_PREPEND or
 * AKVP_CAS; ext gives the time to live, client flags and expected CAS (NULL = none). On AKVP_OK the new
 * CAS version is written to *cas_out (if not NULL).
 */
int akvc_store(akvc *c, uint8_t op, const void *key, size_t klen, const void *val, size_t vlen,
               const struct akvp_ext *ext, uint64_t *cas_out);

/*
 * Reads a value into buf (capacity cap). On AKVP_OK *vlen is the value length. When the value is larger than
 * cap (or than the message limit) the result is AKVP_TOOBIG.
 */
int akvc_get(akvc *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen);

/*
 * As akvc_get() and also returns the metadata (flags, cas, remaining ttl in ms or -1) when meta is not
 * NULL. touch_ttl_ms other than AKV_TTL_KEEP (INT64_MIN) sets a new expiry (get-and-touch).
 */
int akvc_get_meta(akvc *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen, uint32_t *flags,
                  uint64_t *cas, int64_t *ttl_ms, int64_t touch_ttl_ms);

/* Removes a key. AKVP_NOTFOUND when it did not exist. */
int akvc_del(akvc *c, const void *key, size_t klen);

/* AKVP_OK when the key exists, AKVP_NOTFOUND otherwise. */
int akvc_exists(akvc *c, const void *key, size_t klen);

/* Sets the expiry (> 0 ms, 0 none, < 0 delete). */
int akvc_touch(akvc *c, const void *key, size_t klen, int64_t ttl_ms);

/* Adds delta to a decimal value (is_signed selects RESP semantics, see akv_incr()); *result receives the new value. */
int akvc_incr(akvc *c, const void *key, size_t klen, int64_t delta, int is_signed, int64_t *result);

/* Removes all entries of the server. */
int akvc_flush(akvc *c);

/* Reads the server counters (AKVP_STATS_FIELDS values, order in akvproto.h). */
int akvc_stats(akvc *c, uint64_t out[AKVP_STATS_FIELDS]);

/* Sends a raw request message and returns the raw response (for tests of malformed input). The response stays valid until the next call. */
int akvc_raw(akvc *c, const void *msg, size_t len, const uint8_t **resp, size_t *resp_len);

/* ---- pipelining ---------------------------------------------------------------------------------------- */

/* A response collected by akvc_next(). payload stays valid until the next call on the client. */
struct akvc_resp {
    uint32_t reqid;
    uint8_t status;
    const uint8_t *payload;
    size_t len;
};

/* Number of requests that may be outstanding. */
unsigned akvc_window(const akvc *c);

/*
 * Sends a request without waiting for its response; *reqid (if not NULL) receives its identifier.
 * Returns 0, -EBUSY when akvc_window() requests are outstanding, AKVP_TOOBIG when the request exceeds the
 * message limit, or a transport error.
 */
int akvc_post(akvc *c, uint8_t op, const void *key, size_t klen, const void *val, size_t vlen,
              const struct akvp_ext *ext, uint32_t *reqid);

/* Waits for the oldest outstanding response. Returns 0, -EINVAL when none is outstanding, or a transport error. */
int akvc_next(akvc *c, struct akvc_resp *r, int timeout_ms);

/* ---- several servers ----------------------------------------------------------------------------------- */

typedef struct akvc_pool akvc_pool;

/* Connects to n servers given as "host:port". Returns 0 or the error of the first failed connection. */
int akvc_pool_create(akvc_pool **out, const struct duct_opts *opts, const char *const *servers, unsigned n);

/* Returns the client of the server that owns key. */
akvc *akvc_pool_pick(akvc_pool *p, const void *key, size_t klen);

/* Index of the server that owns key (0 .. n-1). */
unsigned akvc_pool_index(const akvc_pool *p, const void *key, size_t klen);

/* Closes all connections. Safe with NULL. */
void akvc_pool_close(akvc_pool *p);

#endif
