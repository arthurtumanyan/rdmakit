/*
 * repl.h - replication backlog and stream format (internal interface of the server).
 *
 * A primary keeps the change records of its engine (the encoding of the append-only log, aof.h) in a ring
 * buffer, the backlog, addressed by a byte offset that grows from 1 for the life of the replication id. A
 * replica asks for the stream from (replication id, offset): when the id matches and the offset is still in
 * the backlog the primary continues from there (partial resynchronization), otherwise it sends a full copy
 * of its content followed by the stream from the offset at which the copy started (full
 * resynchronization). Records describe resulting states, so records that the copy already contains are
 * applied again without effect. This follows the PSYNC design of Redis and Valkey (replication ids, a
 * second id after a promotion, a backlog of fixed size).
 *
 * Messages of the replication link (the first byte is the type; integers little-endian):
 *   primary -> replica
 *     REPL_FULL     40-byte replication id, 8-byte offset of the stream that follows the copy
 *     REPL_CONTINUE 40-byte replication id (the current one; it differs after a promotion)
 *     REPL_BASE     bytes of the copy: records of the AOF encoding, possibly split across messages
 *     REPL_BASE_END the copy is complete
 *     REPL_STREAM   8-byte offset of the first byte, then bytes of the stream (records, possibly split)
 *     REPL_PING     8-byte current offset of the primary (keep-alive, about once per second)
 *   replica -> primary
 *     REPL_ACK      8-byte offset up to which the replica has applied the stream
 * The request that starts a link is the native operation AKVP_REPLSYNC with the value: 40-byte
 * replication id (zeros: none), 8-byte offset (the next byte the replica needs).
 */
#ifndef AKV_REPL_H
#define AKV_REPL_H

#include <stddef.h>
#include <stdint.h>

#include "akv.h"

#define REPL_ID_LEN 40u

enum repl_msg {
    REPL_FULL = 1,
    REPL_CONTINUE = 2,
    REPL_BASE = 3,
    REPL_BASE_END = 4,
    REPL_STREAM = 5,
    REPL_PING = 6,
    REPL_ACK = 7,
};

typedef struct repl_backlog repl_backlog;

/* Creates a backlog of size bytes with a new random replication id. */
repl_backlog *repl_backlog_create(size_t size);
void repl_backlog_destroy(repl_backlog *b);

/* Appends a change record (the encoding of aof.h). Returns the offset after it. */
uint64_t repl_backlog_append(repl_backlog *b, const akv_change *c);

/* Appends raw stream bytes received from a primary (a replica keeps the primary's offsets). */
void repl_backlog_append_raw(repl_backlog *b, const uint8_t *p, size_t n);

/* Offset of the next byte to be appended (the end of the stream). */
uint64_t repl_backlog_end(repl_backlog *b);

/* Offset after the last record appended by the calling thread (0 = none): the offset WAIT waits for. */
uint64_t repl_thread_offset(void);

/*
 * Copies up to cap stream bytes from offset off into out, waiting up to timeout_ms for data. Returns the
 * number of bytes, 0 on timeout, or -ERANGE when off is no longer (or not yet) in the backlog.
 */
long repl_backlog_read(repl_backlog *b, uint64_t off, uint8_t *out, size_t cap, int timeout_ms);

/*
 * Decides how a replica asking for (id, off) continues. Returns 1 for a partial resynchronization (the
 * stream from off), 0 for a full one; *start receives the offset the stream starts at in both cases.
 */
int repl_backlog_plan(repl_backlog *b, const char id[REPL_ID_LEN], uint64_t off, uint64_t *start);

/* The current replication id (REPL_ID_LEN characters, not NUL-terminated), and the previous one. */
void repl_backlog_ids(repl_backlog *b, char id[REPL_ID_LEN], char id2[REPL_ID_LEN], uint64_t *second_off);

/*
 * Adopts the replication id and the offset of a primary (a replica after REPL_FULL): the backlog empties
 * and continues at off.
 */
void repl_backlog_reset(repl_backlog *b, const char id[REPL_ID_LEN], uint64_t off);

/* Adopts a new replication id of the primary at the current offset (its promotion): the previous id
 * becomes the second id, valid up to the current offset. */
void repl_backlog_switch_id(repl_backlog *b, const char id[REPL_ID_LEN]);

/* After a promotion: the current id becomes the second id (valid up to the current offset) and a new
 * random id is chosen, so that replicas of the old primary can continue partially. */
void repl_backlog_promote(repl_backlog *b);

/* First offset still held and the number of bytes held (INFO). */
void repl_backlog_range(repl_backlog *b, uint64_t *first, uint64_t *held);

/* Incremental parser of a record stream: feeds bytes, calls fn for every complete record. */
typedef struct repl_parser {
    uint8_t *buf;
    size_t len;
    size_t cap;
} repl_parser;

/* Returns 0, -EBADMSG for an invalid record, -ENOMEM; fn returning non-zero stops with that value. */
int repl_parse(repl_parser *p, const uint8_t *data, size_t n, int (*fn)(void *arg, const akv_change *c),
               void *arg);
void repl_parser_free(repl_parser *p);

#endif
