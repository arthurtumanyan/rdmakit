/*
 * akvproto.h - native wire protocol between arenakvd and its clients (revision 2).
 *
 * The protocol runs on top of rdmaduct messages: one request message, one response message. All integers
 * are little-endian. Requests carry an identifier that is echoed in the response; a connection's requests
 * are executed and answered in order, so a client may pipeline requests within the transport's credits.
 *
 * Request message:
 *   offset size field
 *   0      1    op        enum akvp_op
 *   1      1    flags     AKVP_F_EXT when the extension follows the header, other bits zero
 *   2      2    klen      key length (1..65535 for keyed operations, 0 otherwise)
 *   4      4    reqid     echoed in the response
 *   8      4    vlen      value length (operation-specific, zero when unused)
 *   12     24   ext       present only with AKVP_F_EXT, see below
 *   ...    klen key
 *   ...    vlen value
 * The message length must equal 12 + (24 if AKVP_F_EXT) + klen + vlen exactly.
 *
 * Extension (AKVP_F_EXT):
 *   0      8    ttl_ms    time to live of stores and TOUCH: > 0 milliseconds, 0 none, < 0 expire now;
 *                         INT64_MIN with GETM: do not change the expiry
 *   8      4    uflags    client flags of stores
 *   12     4    reserved  zero
 *   16     8    cas       expected CAS version of AKVP_CAS
 * Without the extension, ttl_ms = 0, uflags = 0 and cas = 0 (GETM: expiry unchanged).
 *
 * Response message:
 *   0      1    status    enum akvp_status
 *   1      1    flags     zero
 *   2      2    reserved  zero
 *   4      4    reqid     copy of the request identifier
 *   8      4    vlen      length of the payload that follows
 *   12     vlen payload   operation-specific, see enum akvp_op
 *
 * Revision 1 consisted of GET, PUT, DEL and STATS without the extension and with a 12-field STATS
 * payload; revision-1 requests are valid revision-2 requests.
 */
#ifndef AKVPROTO_H
#define AKVPROTO_H

#include <stddef.h>
#include <stdint.h>

#define AKVP_REQ_HDR 12u
#define AKVP_RESP_HDR 12u
#define AKVP_EXT 24u
#define AKVP_F_EXT 0x01u
/* GETM response metadata: uflags (4), reserved (4), cas (8), ttl_ms (8; -1 = no expiry). */
#define AKVP_META 24u

/* Operations. Payload of the response in brackets. */
enum akvp_op {
    AKVP_GET = 1,     /* key [value] */
    AKVP_PUT = 2,     /* key, value: store unconditionally (memcached set) */
    AKVP_DEL = 3,     /* key */
    AKVP_STATS = 4,   /* [AKVP_STATS_FIELDS 64-bit counters] */
    AKVP_ADD = 5,     /* key, value: store if absent (AKVP_EXISTS otherwise) */
    AKVP_REPLACE = 6, /* key, value: store if present (AKVP_NOTFOUND otherwise) */
    AKVP_APPEND = 7,  /* key, value: append to the existing value */
    AKVP_PREPEND = 8, /* key, value: prepend to the existing value */
    AKVP_CAS = 9,     /* key, value, ext.cas: store if the CAS matches (AKVP_EXISTS on mismatch) */
    AKVP_INCR = 10,   /* key, value = 8-byte signed delta [8-byte new value]; unsigned memcached semantics */
    AKVP_INCRS = 11,  /* as INCR with signed semantics: absent counts as 0, overflow is AKVP_NOTNUM */
    AKVP_TOUCH = 12,  /* key, ext.ttl_ms */
    AKVP_FLUSH = 13,  /* remove all entries */
    AKVP_HAS = 14,    /* key: AKVP_OK when present, AKVP_NOTFOUND otherwise */
    AKVP_GETM = 15,   /* key, ext.ttl_ms (INT64_MIN keeps the expiry) [AKVP_META metadata, value] */
    /*
     * value = sequence of keys, each as 2-byte length and bytes [for each key: 1-byte status, 3 reserved
     * bytes, 4-byte length, value]. The response must fit the message limit (AKVP_TOOBIG otherwise).
     */
    AKVP_MGET = 16,
    AKVP_AUTH = 17, /* value = "user:password" or "password" */
    AKVP_NOOP = 18, /* nothing; answered AKVP_OK (pipeline barrier, keep-alive) */
    /*
     * value = 40-byte replication id, 8-byte offset, 2-byte listening port: starts a replication link
     * (src/repl.h); the connection then carries the replication stream instead of responses.
     */
    AKVP_REPLSYNC = 19,
    AKVP_ROLE = 20, /* [1 byte: 0 primary, 1 replica] */
};

enum akvp_status {
    AKVP_OK = 0,
    AKVP_NOTFOUND = 1,
    AKVP_BADREQ = 2,
    AKVP_TOOBIG = 3,
    AKVP_ERROR = 4,
    AKVP_EXISTS = 5,    /* ADD of a present key, CAS mismatch */
    AKVP_NOMEM = 6,     /* no space without eviction (server started with eviction disabled) */
    AKVP_NOAUTH = 7,    /* authentication required or failed */
    AKVP_NOTNUM = 8,    /* INCR of a non-numeric value, signed overflow */
    AKVP_NOPERM = 9,    /* the user may not run the operation or access the key (access control list) */
    AKVP_READONLY = 10, /* a write sent to a read-only replica */
};

/*
 * STATS payload: the counters of struct akv_stats in order (entries, arena_bytes, log_bytes, gets, hits,
 * puts, overwrites, dels, evictions, reinsertions, index_evictions, rejected, expired, index_bytes,
 * index_grows), then server counters: current connections, total connections, commands processed,
 * uptime in seconds.
 */
#define AKVP_STATS_FIELDS 19u
#define AKVP_STATS_BYTES (AKVP_STATS_FIELDS * 8u)

struct akvp_req {
    uint8_t op;
    uint8_t flags;
    uint16_t klen;
    uint32_t reqid;
    uint32_t vlen;
};

struct akvp_ext {
    int64_t ttl_ms;
    uint32_t uflags;
    uint64_t cas;
};

struct akvp_resp {
    uint8_t status;
    uint8_t flags;
    uint32_t reqid;
    uint32_t vlen;
};

static inline void akvp_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t akvp_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void akvp_put64(uint8_t *p, uint64_t v) {
    akvp_put32(p, (uint32_t)v);
    akvp_put32(p + 4, (uint32_t)(v >> 32));
}

static inline uint64_t akvp_get64(const uint8_t *p) {
    return (uint64_t)akvp_get32(p) | ((uint64_t)akvp_get32(p + 4) << 32);
}

/* Writes a request header (AKVP_REQ_HDR bytes). */
static inline void akvp_req_encode(uint8_t *out, const struct akvp_req *r) {
    out[0] = r->op;
    out[1] = r->flags;
    out[2] = (uint8_t)r->klen;
    out[3] = (uint8_t)(r->klen >> 8);
    akvp_put32(out + 4, r->reqid);
    akvp_put32(out + 8, r->vlen);
}

/* Parses a request header; the caller checks the total message length. Returns 0, or -1 when len < AKVP_REQ_HDR. */
static inline int akvp_req_decode(const uint8_t *in, size_t len, struct akvp_req *r) {
    if (len < AKVP_REQ_HDR) {
        return -1;
    }
    r->op = in[0];
    r->flags = in[1];
    r->klen = (uint16_t)(in[2] | (in[3] << 8));
    r->reqid = akvp_get32(in + 4);
    r->vlen = akvp_get32(in + 8);
    return 0;
}

/* Writes an extension (AKVP_EXT bytes). */
static inline void akvp_ext_encode(uint8_t *out, const struct akvp_ext *x) {
    akvp_put64(out, (uint64_t)x->ttl_ms);
    akvp_put32(out + 8, x->uflags);
    akvp_put32(out + 12, 0);
    akvp_put64(out + 16, x->cas);
}

static inline void akvp_ext_decode(const uint8_t *in, struct akvp_ext *x) {
    x->ttl_ms = (int64_t)akvp_get64(in);
    x->uflags = akvp_get32(in + 8);
    x->cas = akvp_get64(in + 16);
}

/* Writes a response header (AKVP_RESP_HDR bytes). */
static inline void akvp_resp_encode(uint8_t *out, const struct akvp_resp *r) {
    out[0] = r->status;
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    akvp_put32(out + 4, r->reqid);
    akvp_put32(out + 8, r->vlen);
}

/* Parses a response header. Returns 0, or -1 when len < AKVP_RESP_HDR or the payload length disagrees. */
static inline int akvp_resp_decode(const uint8_t *in, size_t len, struct akvp_resp *r) {
    if (len < AKVP_RESP_HDR) {
        return -1;
    }
    r->status = in[0];
    r->flags = in[1];
    r->reqid = akvp_get32(in + 4);
    r->vlen = akvp_get32(in + 8);
    return (size_t)r->vlen + AKVP_RESP_HDR == len ? 0 : -1;
}

#endif
