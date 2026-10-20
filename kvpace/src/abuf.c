/* abuf.c - non-blocking buffered connection, see abuf.h. */
#include "abuf.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

/* Largest input buffer: replies above this size (MGET of many large values) fail the connection. */
#define ABUF_MAX_IN (256u << 20)

int pace_abuf_adopt(pace_abuf *b, pace_net *n) {
    memset(b, 0, sizeof(*b));
    b->s = n->s;
    /* Bytes the blocking reader already buffered belong to the stream and stay first. */
    size_t left = n->rlen - n->rpos;
    if (left > 0) {
        b->r = malloc(left);
        if (b->r == NULL) {
            return -1;
        }
        memcpy(b->r, n->rbuf + n->rpos, left);
        b->rlen = left;
        b->rcap = left;
    }
    free(n->wbuf);
    free(n);
    int fl = fcntl(b->s.fd, F_GETFL);
    return fcntl(b->s.fd, F_SETFL, fl | O_NONBLOCK) == 0 ? 0 : -1;
}

static int grow(uint8_t **p, size_t *cap, size_t need) {
    if (need <= *cap) {
        return 0;
    }
    size_t c = *cap ? *cap : 4096;
    while (c < need) {
        c *= 2;
    }
    uint8_t *q = realloc(*p, c);
    if (q == NULL) {
        return -1;
    }
    *p = q;
    *cap = c;
    return 0;
}

int pace_abuf_add(pace_abuf *b, const void *data, size_t n) {
    if (b->woff > 0 && b->woff == b->wlen) {
        b->woff = b->wlen = 0;
    }
    if (grow(&b->w, &b->wcap, b->wlen + n) != 0) {
        return -1;
    }
    memcpy(b->w + b->wlen, data, n);
    b->wlen += n;
    return 0;
}

int pace_abuf_printf(pace_abuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return -1;
    }
    return pace_abuf_add(b, tmp, (size_t)n);
}

int pace_abuf_push(pace_abuf *b, pace_op_type t) {
    if (b->tcount == b->tcap) {
        unsigned cap = b->tcap ? b->tcap * 2 : 64;
        uint8_t *q = malloc(cap);
        if (q == NULL) {
            return -1;
        }
        for (unsigned i = 0; i < b->tcount; i++) {
            q[i] = b->types[(b->thead + i) % b->tcap];
        }
        free(b->types);
        b->types = q;
        b->thead = 0;
        b->tcap = cap;
    }
    b->types[(b->thead + b->tcount) % b->tcap] = (uint8_t)t;
    b->tcount++;
    return 0;
}

int pace_abuf_front(const pace_abuf *b) {
    return b->tcount > 0 ? b->types[b->thead] : -1;
}

void pace_abuf_pop(pace_abuf *b) {
    b->thead = (b->thead + 1) % b->tcap;
    b->tcount--;
}

int pace_abuf_io(pace_abuf *b) {
    b->s.want_write = 0;
    while (b->woff < b->wlen) {
        ssize_t w = pace_sock_write(&b->s, b->w + b->woff, b->wlen - b->woff);
        if (w < 0) {
            return -1;
        }
        if (w == 0) {
            break;
        }
        b->woff += (size_t)w;
    }
    if (b->woff == b->wlen) {
        b->woff = b->wlen = 0;
    }
    for (;;) {
        if (b->roff > 0 && b->roff == b->rlen) {
            b->roff = b->rlen = 0;
        }
        if (b->rcap - b->rlen < 16384) {
            if (b->roff > 0) {
                memmove(b->r, b->r + b->roff, b->rlen - b->roff);
                b->rlen -= b->roff;
                b->roff = 0;
            }
            if (b->rcap - b->rlen < 16384 &&
                (b->rcap >= ABUF_MAX_IN || grow(&b->r, &b->rcap, b->rcap + 65536) != 0)) {
                return -1;
            }
        }
        ssize_t r = pace_sock_read(&b->s, b->r + b->rlen, b->rcap - b->rlen);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        b->rlen += (size_t)r;
    }
}

void pace_abuf_consume(pace_abuf *b, size_t n) {
    b->roff += n;
}

unsigned pace_abuf_events(const pace_abuf *b) {
    return EPOLLIN | (b->woff < b->wlen || b->s.want_write ? EPOLLOUT : 0);
}

int pace_abuf_ready(const pace_abuf *b) {
    return pace_sock_pending(&b->s);
}

void pace_abuf_close(pace_abuf *b) {
    pace_sock_close(&b->s);
    free(b->r);
    free(b->w);
    free(b->types);
    memset(b, 0, sizeof(*b));
    b->s.fd = -1;
}
