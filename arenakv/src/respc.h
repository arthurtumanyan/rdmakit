/*
 * respc.h - blocking RESP client with deadlines, for server-to-server requests (MIGRATE), the failover
 * monitor and the resharding tool.
 *
 * Every call of a connection shares its deadline (respc_deadline); a call that cannot finish before it
 * fails, so a dead or stalled peer costs at most the time given. Arguments are binary-safe (explicit
 * lengths). Replies are RESP2 values; bulk strings may be as large as the peer sends, up to
 * RESPC_MAX_BULK.
 */
#ifndef AKV_RESPC_H
#define AKV_RESPC_H

#include <stddef.h>
#include <stdint.h>

#define RESPC_MAX_BULK (512u << 20)

/* A reply: '+' and '-' (text in s), ':' (i), '$' (s and n; s == NULL for a null bulk), '*' (a and na; a ==
 * NULL for a null array). Strings are NUL-terminated. */
typedef struct respv {
    char t;
    long long i;
    char *s;
    size_t n;
    struct respv **a;
    size_t na;
} respv;

void respv_free(respv *v);

typedef struct respc respc;

/* Connects to host:port; the deadline of the connection is now + timeout_ms. Returns NULL on failure. */
respc *respc_open(const char *host, int port, unsigned timeout_ms);

/* Moves the deadline to now + timeout_ms. */
void respc_deadline(respc *c, unsigned timeout_ms);

/* Sends AUTH (user may be NULL or empty for the password-only form) and checks the reply. Returns 0 or -1. */
int respc_auth(respc *c, const char *user, const char *pass);

/* Sends one command (lens == NULL: NUL-terminated arguments). Returns 0 or -1. Replies are read with
 * respc_read, in order, so several commands can be sent before their replies are read. */
int respc_send(respc *c, int argc, const char *const *argv, const size_t *lens);

/* Reads the next reply. Returns NULL on a connection error, a timeout or a malformed reply. */
respv *respc_read(respc *c);

void respc_close(respc *c);

/* One command on a new connection (after AUTH when pass is set). Returns the reply or NULL. */
respv *respc_call(const char *host, int port, const char *user, const char *pass, unsigned timeout_ms,
                  int argc, const char *const *argv);

#endif
