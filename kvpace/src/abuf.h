/*
 * abuf.h - non-blocking buffered connection for the asynchronous text-protocol drivers.
 *
 * Holds the socket (TCP or TLS), an input buffer that grows to the largest reply, an output buffer of
 * encoded requests, and a FIFO of the operation types of outstanding requests, which tells the reply
 * parser what to expect. The connection is opened blocking (so that authentication can use the
 * synchronous code) and switched to non-blocking afterwards.
 */
#ifndef PACE_ABUF_H
#define PACE_ABUF_H

#include <stddef.h>
#include <stdint.h>

#include "netbuf.h"
#include "workload.h"

typedef struct pace_abuf {
    pace_sock s;
    uint8_t *r;
    size_t rlen; /* bytes in r */
    size_t roff; /* bytes of r consumed */
    size_t rcap;
    uint8_t *w;
    size_t wlen;
    size_t woff;
    size_t wcap;
    uint8_t *types; /* ring of pace_op_type */
    unsigned thead;
    unsigned tcount;
    unsigned tcap;
} pace_abuf;

/* Takes over an authenticated blocking connection (its socket; n is freed) and makes it non-blocking. */
int pace_abuf_adopt(pace_abuf *b, pace_net *n);

/* Appends bytes or formatted text to the output. Return 0 or -1. */
int pace_abuf_add(pace_abuf *b, const void *data, size_t n);
int pace_abuf_printf(pace_abuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Records the type of a submitted request. Returns 0 or -1. */
int pace_abuf_push(pace_abuf *b, pace_op_type t);
/* Type of the oldest outstanding request; -1 when none. */
int pace_abuf_front(const pace_abuf *b);
void pace_abuf_pop(pace_abuf *b);

/* Writes pending output and reads available input. Returns 0, or -1 on failure or end of stream. */
int pace_abuf_io(pace_abuf *b);

/* Unconsumed input. */
static inline const uint8_t *pace_abuf_in(const pace_abuf *b) {
    return b->r + b->roff;
}
static inline size_t pace_abuf_avail(const pace_abuf *b) {
    return b->rlen - b->roff;
}
void pace_abuf_consume(pace_abuf *b, size_t n);

/* Epoll events to wait for. */
unsigned pace_abuf_events(const pace_abuf *b);

/* 1 when input can be processed without waiting (TLS-buffered bytes). */
int pace_abuf_ready(const pace_abuf *b);

void pace_abuf_close(pace_abuf *b);

#endif
