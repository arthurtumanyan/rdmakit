/*
 * akvproto.h - wire protocol between arenakvd and its clients.
 *
 * The protocol runs on top of rdmaduct messages: one request message, one response message. All integers
 * are little-endian. Requests carry an identifier that is echoed in the response, so a client may keep
 * several requests in flight.
 *
 * Request message:
 *   offset size field
 *   0      1    op        AKVP_GET, AKVP_PUT, AKVP_DEL, AKVP_STATS
 *   1      1    flags     reserved, zero
 *   2      2    klen      key length (1..65535 for GET/PUT/DEL, 0 for STATS)
 *   4      4    reqid     echoed in the response
 *   8      4    vlen      value length (PUT only; zero otherwise)
 *   12     klen key
 *   12+klen vlen value
 * The message length must equal 12 + klen + vlen exactly.
 *
 * Response message:
 *   0      1    status    AKVP_OK, AKVP_NOTFOUND, AKVP_BADREQ, AKVP_TOOBIG, AKVP_ERROR
 *   1      1    flags     reserved, zero
 *   2      2    reserved  zero
 *   4      4    reqid     copy of the request identifier
 *   8      4    vlen      length of the payload that follows (GET: value; STATS: AKVP_STATS_BYTES)
 *   12     vlen payload
 *
 * STATS payload: AKVP_STATS_FIELDS little-endian 64-bit counters in the order of struct akv_stats
 * (entries, arena_bytes, log_bytes, gets, hits, puts, overwrites, dels, evictions, reinsertions,
 * index_evictions, rejected).
 */
#ifndef AKVPROTO_H
#define AKVPROTO_H

#include <stddef.h>
#include <stdint.h>

#define AKVP_REQ_HDR 12u
#define AKVP_RESP_HDR 12u

enum akvp_op {
    AKVP_GET = 1,
    AKVP_PUT = 2,
    AKVP_DEL = 3,
    AKVP_STATS = 4,
};

enum akvp_status {
    AKVP_OK = 0,
    AKVP_NOTFOUND = 1,
    AKVP_BADREQ = 2,
    AKVP_TOOBIG = 3,
    AKVP_ERROR = 4,
};

#define AKVP_STATS_FIELDS 12u
#define AKVP_STATS_BYTES (AKVP_STATS_FIELDS * 8u)

struct akvp_req {
    uint8_t op;
    uint8_t flags;
    uint16_t klen;
    uint32_t reqid;
    uint32_t vlen;
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
    out[1] = 0;
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
