/* netbuf.c - buffered blocking TCP connection, see netbuf.h. */
#include "netbuf.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

pace_net *pace_net_open(const char *host, const char *port) {
    pace_net *n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return NULL;
    }
    if (pace_sock_connect(&n->s, host, port, 0) != 0) {
        free(n);
        return NULL;
    }
    return n;
}

void pace_net_close(pace_net *n) {
    if (n == NULL) {
        return;
    }
    pace_sock_close(&n->s);
    free(n->wbuf);
    free(n);
}

int pace_net_reserve(pace_net *n, size_t cap) {
    if (n->wcap >= cap) {
        return 0;
    }
    size_t c = n->wcap ? n->wcap : 1024;
    while (c < cap) {
        c *= 2;
    }
    unsigned char *p = realloc(n->wbuf, c);
    if (p == NULL) {
        return -1;
    }
    n->wbuf = p;
    n->wcap = c;
    return 0;
}

int pace_net_send(pace_net *n, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t w = pace_sock_write(&n->s, p, len);
        if (w < 0) {
            return -1;
        }
        p += w; /* 0: a blocking TLS socket needed another round; retry */
        len -= (size_t)w;
    }
    return 0;
}

/* Refills the read buffer; returns 0 or -1 (error or end of stream). */
static int fill(pace_net *n) {
    for (;;) {
        ssize_t r = pace_sock_read(&n->s, n->rbuf, PACE_NET_RBUF);
        if (r > 0) {
            n->rpos = 0;
            n->rlen = (size_t)r;
            return 0;
        }
        if (r < 0) {
            return -1;
        }
    }
}

int pace_net_read_line(pace_net *n, char *out, size_t cap) {
    size_t len = 0;
    for (;;) {
        if (n->rpos == n->rlen && fill(n) != 0) {
            return -1;
        }
        unsigned char ch = n->rbuf[n->rpos++];
        if (ch == '\n') {
            if (len > 0 && out[len - 1] == '\r') {
                len--;
            }
            out[len] = '\0';
            return 0;
        }
        if (len + 1 >= cap) {
            return -1;
        }
        out[len++] = (char)ch;
    }
}

int pace_net_read_exact(pace_net *n, void *dst, size_t len) {
    unsigned char *d = dst;
    while (len > 0) {
        if (n->rpos == n->rlen && fill(n) != 0) {
            return -1;
        }
        size_t avail = n->rlen - n->rpos;
        size_t take = avail < len ? avail : len;
        if (d != NULL) {
            memcpy(d, n->rbuf + n->rpos, take);
            d += take;
        }
        n->rpos += take;
        len -= take;
    }
    return 0;
}
