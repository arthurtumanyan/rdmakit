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
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return NULL;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return NULL;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    pace_net *n = calloc(1, sizeof(*n));
    if (n == NULL) {
        close(fd);
        return NULL;
    }
    n->fd = fd;
    return n;
}

void pace_net_close(pace_net *n) {
    if (n == NULL) {
        return;
    }
    close(n->fd);
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
        ssize_t w = send(n->fd, p, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Refills the read buffer; returns 0 or -1 (error or end of stream). */
static int fill(pace_net *n) {
    for (;;) {
        ssize_t r = recv(n->fd, n->rbuf, PACE_NET_RBUF, 0);
        if (r > 0) {
            n->rpos = 0;
            n->rlen = (size_t)r;
            return 0;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return -1;
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
