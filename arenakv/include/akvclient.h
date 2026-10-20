/*
 * akvclient.h - synchronous client for the akvproto protocol over rdmaduct.
 *
 * One request at a time per client. A client is used by one thread. Functions return a non-negative akvp_status
 * (AKVP_OK, AKVP_NOTFOUND, ...) when the server answered, or a negative rdmaduct error code when the transport
 * failed (the client is then unusable and must be closed).
 */
#ifndef AKVCLIENT_H
#define AKVCLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "akvproto.h"
#include "rdmaduct.h"

typedef struct akvc akvc;

/* Connects to a server. opts selects transport and buffer sizes (max_msg must be at least as large as the largest message used). */
int akvc_connect(akvc **out, const struct duct_opts *opts, const char *host, const char *port);

/* Closes the connection and frees the client. Safe with NULL. */
void akvc_close(akvc *c);

/* Stores a value. */
int akvc_put(akvc *c, const void *key, size_t klen, const void *val, size_t vlen);

/*
 * Reads a value into buf (capacity cap). On AKVP_OK *vlen is the value length. When the value is larger than
 * cap (or than the message limit) the result is AKVP_TOOBIG.
 */
int akvc_get(akvc *c, const void *key, size_t klen, void *buf, size_t cap, size_t *vlen);

/* Removes a key. AKVP_NOTFOUND when it did not exist. */
int akvc_del(akvc *c, const void *key, size_t klen);

/* Reads the server counters (AKVP_STATS_FIELDS values, order of struct akv_stats). */
int akvc_stats(akvc *c, uint64_t out[AKVP_STATS_FIELDS]);

/* Sends a raw request message and returns the raw response (for tests of malformed input). The response stays valid until the next call. */
int akvc_raw(akvc *c, const void *msg, size_t len, const uint8_t **resp, size_t *resp_len);

#endif
