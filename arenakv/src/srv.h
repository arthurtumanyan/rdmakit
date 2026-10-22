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
#include "aof.h"

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
    int authed;     /* authentication done or not required */
    int close;      /* set by a protocol module: close after the pending output is written */
    size_t swallow; /* memcached: bytes of an oversized data block still to be discarded */
    char name[64];  /* RESP CLIENT SETNAME */
    uint64_t id;    /* connection number (RESP CLIENT ID) */
};

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

/* Returns 1 when authentication is required. */
int akvd_auth_required(const akvd *d);

/*
 * Checks credentials in constant time per entry. user == NULL (or ulen == 0) selects the default user.
 * Returns 1 when they match an entry.
 */
int akvd_auth_check(akvd *d, const char *user, size_t ulen, const char *pass, size_t plen);

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

/* State of the append-only log. Returns 0, or -EINVAL when no log is configured. */
int akvd_aof_info(akvd *d, aof_info *out);

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
