/*
 * srv.h - internal interface between the server core (server.c) and the protocol modules
 * (proto_mc.c, proto_resp.c, snapshot.c).
 *
 * Protocol modules are pure functions over byte buffers: they parse complete commands from an input
 * buffer, execute them on the engine and append replies to an output buffer. They never touch sockets,
 * which keeps them testable with incremental byte feeds (tests/test_proto.c).
 */
#ifndef AKV_SRV_H
#define AKV_SRV_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "akv.h"
#include "akvd.h"
#include "acl.h"
#include "aof.h"
#include "pubsub.h"
#include "cluster.h"
#include "repl.h"

/* Growable output buffer. Bytes [off, len) are pending. */
struct obuf {
    uint8_t *p;
    size_t len;
    size_t off;
    size_t cap;
};

/* Appends n bytes. Returns 0 or -ENOMEM (the buffer is then unchanged). */
int ob_add(struct obuf *b, const void *data, size_t n);
/* Appends formatted text. Returns 0 or -ENOMEM. */
int ob_printf(struct obuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Returns a pointer to n writable bytes at the end of the buffer (not yet counted), or NULL. */
uint8_t *ob_reserve(struct obuf *b, size_t n);
/* Counts n bytes written into the space returned by ob_reserve(). */
void ob_commit(struct obuf *b, size_t n);
/* Pending byte count. */
static inline size_t ob_pending(const struct obuf *b) {
    return b->len - b->off;
}
/* Drops consumed bytes; frees the memory when the buffer is empty and larger than 64 KiB. */
void ob_compact(struct obuf *b);
void ob_free(struct obuf *b);

/*
 * Output size above which a protocol module stops executing further commands of the input until the
 * pending output has been written (pipeline backpressure).
 */
#define OB_SOFT_LIMIT (4u << 20)

/* Per-connection protocol state. */
struct session {
    int authed; /* authentication done or not required */
    /* Logged-in user; NULL without access control (everything allowed). */
    const struct acl_user *user;
    uint64_t acl_gen; /* access control generation the user was resolved in */
    int close;        /* set by a protocol module: close after the pending output is written */
    size_t swallow;   /* memcached: bytes of an oversized data block still to be discarded */
    char name[64];    /* RESP CLIENT SETNAME */
    uint64_t id;      /* connection number (RESP CLIENT ID) */
    int proto;        /* RESP protocol version: 2 (also when 0) or 3 (HELLO 3) */
    /* RESP transactions: MULTI ... EXEC */
    int in_multi;
    int multi_error; /* a command was refused while queueing: EXEC answers EXECABORT */
    struct obuf
        queued; /* queued commands: 4-byte argument count, then 4-byte length and bytes per argument */
    unsigned nqueued;
    struct obuf watched; /* WATCH: 8-byte CAS version (0 = absent), 2-byte key length, key */
    /* pub/sub: the connection that owns the session (NULL when driven without one) and its worker */
    void *conn;
    unsigned worker;
    struct obuf subs; /* subscriptions: 1-byte kind (0 channel, 1 pattern), 4-byte length, name */
    unsigned nsubs;
    /* AKVP_REPLSYNC: the request; the connection is handed over to a replication feed */
    struct obuf handoff;
    int handing_off;
    /* RESP WAIT: the reply is due when `wait_n` replicas acknowledged wait_off or at wait_deadline_ms */
    int readonly; /* RESP READONLY: a replica in cluster mode may serve reads of its primary's slots */
    int waiting;
    uint64_t wait_off;
    long long wait_n;
    int64_t wait_deadline_ms; /* CLOCK_MONOTONIC; 0 = no deadline */
};

/* Frees the buffers of a session (transaction queue, watched keys). */
void resp_session_free(struct session *ss);

/* Server statistics updated by all threads. */
struct srv_counters {
    unsigned long long cmd_get;
    unsigned long long cmd_set;
    unsigned long long cmd_touch;
    unsigned long long cmd_flush;
    unsigned long long get_hits;
    unsigned long long get_misses;
    unsigned long long commands;
    unsigned long long total_connections;
    unsigned long long rejected_connections;
    unsigned long long auth_failures;
    unsigned curr_connections;
};

/* Counter update (relaxed atomic add). */
#define SRV_COUNT(d, field, n) __atomic_fetch_add(&akvd_counters(d)->field, (n), __ATOMIC_RELAXED)

struct srv_counters *akvd_counters(akvd *d);

/* Returns 1 when access control is configured (an ACL file or a credentials file). */
int akvd_acl_enabled(const akvd *d);

/* Returns 1 when a new connection must authenticate (access control without a password-less default). */
int akvd_auth_required(const akvd *d);

/* Sets up the access state of a new session: logged in as the password-less default user, if any. */
void akvd_session_init(akvd *d, struct session *ss);

/*
 * Logs the session in. user == NULL (or ulen == 0) selects "default". Returns 1 on success; 0 for an
 * unknown or disabled user or a wrong password (counted in auth_failures).
 */
int akvd_login(akvd *d, struct session *ss, const char *user, size_t ulen, const char *pass, size_t plen);

/*
 * Applies an ACL reload to the session: re-resolves its user by name. Returns 0, or -1 when the user was
 * removed or disabled (the session is then marked for closing). Costs one atomic load when nothing changed.
 */
static inline int akvd_session_refresh(akvd *d, struct session *ss);

/* Current generation of the access control list (incremented by every reload). */
uint64_t akvd_acl_gen(const akvd *d);

/* Slow path of akvd_session_refresh(). */
int akvd_session_reresolve(akvd *d, struct session *ss);

static inline int akvd_session_refresh(akvd *d, struct session *ss) {
    return ss->user == NULL || ss->acl_gen == akvd_acl_gen(d) ? 0 : akvd_session_reresolve(d, ss);
}

/* The current access control list (NULL without access control); valid until the server stops. */
const acl *akvd_acl(akvd *d);

/* Reloads the ACL file. Returns 0, -EINVAL (no ACL file configured) or -EBADMSG (message in err). */
int akvd_acl_reload(akvd *d, char *err, size_t errlen);

/*
 * Executes one native request into out (capacity cap) for the session. Returns the response length, or 0
 * when the request cannot be decoded (the connection is closed).
 */
size_t akvd_native_execute(akvd *d, struct session *ss, const uint8_t *msg, size_t len, uint8_t *out,
                           size_t cap);

/*
 * Reads key into a thread-local buffer that grows to the largest value read by the thread, applying
 * touch_ttl_ms as akv_get_meta() does. Returns 0 (hit; *val is valid until the thread's next call),
 * -ENOENT, -EINVAL or -ENOMEM.
 */
int srv_fetch(akvd *d, const char *key, size_t klen, akv_meta *m, int64_t touch_ttl_ms, const uint8_t **val,
              size_t *vlen);

/* 1 when the engine was configured without eviction. */
int akvd_no_evict(const akvd *d);

/* Largest key + value size accepted by the engine. */
uint32_t akvd_max_entry(const akvd *d);

/* Seconds since the server started. */
uint64_t akvd_uptime(const akvd *d);

/* Number of worker threads. */
unsigned akvd_threads(const akvd *d);

/* A new transaction id (non-zero) for the change records of one EXEC. */
uint64_t akvd_next_txn(akvd *d);

/*
 * Marks the end of transaction txn in the append-only log (and, later, the replication stream), so that
 * its changes are applied together or not at all. Called while the transaction's lock set is held.
 */
void akvd_txn_end(akvd *d, uint64_t txn);

/* State of the append-only log. Returns 0, or -EINVAL when no log is configured. */
int akvd_aof_info(akvd *d, aof_info *out);

/*
 * Subscribes the session to a channel (pattern = 0) or a channel pattern. Returns 1 when added, 0 when
 * already subscribed, -ENOTSUP for a session without a connection, or -ENOMEM.
 */
int akvd_subscribe(akvd *d, struct session *ss, const char *name, size_t n, int pattern);

/* Removes a subscription. Returns 1 when it existed. */
int akvd_unsubscribe(akvd *d, struct session *ss, const char *name, size_t n, int pattern);

/* Removes every subscription of the session. */
void akvd_unsubscribe_all(akvd *d, struct session *ss);

/* Publishes msg on channel ch. Returns the number of receivers. */
size_t akvd_publish(akvd *d, const char *ch, size_t cn, const char *msg, size_t mn);

/* The pub/sub registry (PUBSUB CHANNELS, NUMSUB, NUMPAT). */
pubsub *akvd_pubsub(akvd *d);

/* ---- replication (replication.c) ---- */

struct repl;

struct repl *repl_create(akvd *d, size_t backlog);
void repl_destroy(struct repl *r);
repl_backlog *repl_backlog_of(struct repl *r);

/* Starts a feed thread that owns ep (the request of AKVP_REPLSYNC in req). Returns 0 or a negative error. */
int repl_feed_start(struct repl *r, duct_ep *ep, const uint8_t *req, size_t len);

/* Follows the primary host:port, or (host == NULL) stops following and becomes a primary. */
int repl_follow(struct repl *r, const char *host, const char *port);

int repl_is_replica(struct repl *r);

/* 1 on the thread that applies the stream of a primary (its changes are not put into the backlog). */
int repl_link_thread(void);

/* Replicas that acknowledged offset off. */
size_t repl_acked(struct repl *r, uint64_t off);

/* Appends the "# Replication" fields of INFO. */
void repl_info(struct repl *r, struct obuf *o);

/* The cluster map (NULL outside cluster mode); valid until the server stops. */
const cluster *akvd_cluster(akvd *d);

/* Reloads the cluster map file. Returns 0 or -EBADMSG (message in err). */
int akvd_cluster_reload(akvd *d, char *err, size_t errlen);

/* The server's configuration. */
const akvd_config *akvd_cfg(const akvd *d);

/* Transport options of the server's native listener (for the replication client). */
void akvd_duct_opts(const akvd *d, struct duct_opts *o);

/* The replication state, created when the server starts. */
struct repl *akvd_repl(akvd *d);

/* 1 when client writes must be refused (a read-only replica). */
int akvd_read_only(const akvd *d);

/* Makes the backlog record changes from now on (the first replica or REPLICAOF). */
void akvd_repl_activate(akvd *d);

/* Wakes workers with connections in WAIT (a replica acknowledged an offset or disconnected). */
void akvd_wake_waiters(akvd *d);

/* fsync policy of the append-only log as Valkey names it ("always", "everysec", "no"). */
const char *akvd_appendfsync(const akvd *d);

/* Outcome of the warm-restart attach at start. */
void akvd_warm_info(akvd *d, akv_warm_info *out);

/* Server version string. */
#define AKVD_VERSION "0.2.0"

/*
 * Protocol modules. Each executes the complete commands at the start of in[0, len), appends the replies
 * to out and returns the number of bytes consumed. An incomplete command at the end is left unconsumed.
 * Execution stops early when ob_pending(out) exceeds OB_SOFT_LIMIT. ss->close is set when the connection
 * must be closed once the pending output has been written (quit, protocol errors that lose framing).
 * max_input is the size above which an incomplete command is treated as a protocol error.
 */
size_t mc_execute(akvd *d, struct session *ss, const uint8_t *in, size_t len, struct obuf *out);
size_t resp_execute(akvd *d, struct session *ss, const uint8_t *in, size_t len, struct obuf *out);

/* Frees the calling thread's buffers of the protocol modules and of srv_fetch(); called at worker exit. */
void resp_thread_exit(void);
void srv_thread_exit(void);

/* Largest input a text connection buffers for one command. */
size_t akvd_max_input(const akvd *d);

/* Converts a memcached exptime (seconds, or a Unix time above 30 days, negative = expired) to a TTL. */
int64_t mc_exptime_to_ttl(int64_t exptime);

/*
 * Snapshot file format (little-endian):
 *   header  8 bytes magic "AKVSNAP1", 4 bytes version (1), 4 bytes reserved, 8 bytes creation Unix time
 *   blocks  4 bytes magic "BLK1", 4 bytes record count, 4 bytes payload length, 4 bytes CRC32C of the
 *           payload, then the payload: records of klen (2), reserved (2), uflags (4), vlen (4),
 *           ttl_ms (8, -1 = no expiry), key, value
 *   end     a block with record count 0 and payload length 8 holding the total record count (8)
 * The file is written to "<path>.tmp", synced, and renamed over <path>.
 */
int snap_save(akv *s, const char *path, uint64_t *records);
/* Loads a snapshot into s. Returns 0, -ENOENT when the file does not exist, -EBADMSG when it is corrupt
 * or truncated (entries read before the damage stay loaded), or -errno. */
int snap_load(akv *s, const char *path, uint64_t *records);

/* CRC32C (Castagnoli), software implementation. */
uint32_t akv_crc32c(uint32_t crc, const void *data, size_t n);

#endif
