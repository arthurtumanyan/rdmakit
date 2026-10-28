/* respc.c - blocking RESP client with deadlines, see respc.h. */
#include "respc.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct respc {
    int fd;
    uint64_t deadline; /* CLOCK_MONOTONIC milliseconds */
    char buf[16384];
    size_t len;
    size_t off;
};

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void respv_free(respv *v) {
    if (v == NULL) {
        return;
    }
    for (size_t i = 0; i < v->na; i++) {
        respv_free(v->a[i]);
    }
    free(v->a);
    free(v->s);
    free(v);
}

/* Waits until fd is ready for `events` before the deadline. Returns 0 or -1. */
static int wait_fd(respc *c, short events) {
    for (;;) {
        uint64_t now = mono_ms();
        if (now >= c->deadline) {
            return -1;
        }
        struct pollfd pf = {.fd = c->fd, .events = events};
        int n = poll(&pf, 1, (int)(c->deadline - now));
        if (n > 0) {
            return 0;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
}

respc *respc_open(const char *host, int port, unsigned timeout_ms) {
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, ps, &hints, &ai) != 0) {
        return NULL;
    }
    respc *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        freeaddrinfo(ai);
        return NULL;
    }
    c->fd = -1;
    c->deadline = mono_ms() + timeout_ms;
    for (struct addrinfo *p = ai; p != NULL && c->fd < 0; p = p->ai_next) {
        c->fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, p->ai_protocol);
        if (c->fd < 0) {
            continue;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        if ((connect(c->fd, p->ai_addr, p->ai_addrlen) != 0 && errno != EINPROGRESS) ||
            wait_fd(c, POLLOUT) != 0 || getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
            close(c->fd);
            c->fd = -1;
        }
    }
    freeaddrinfo(ai);
    if (c->fd < 0) {
        free(c);
        return NULL;
    }
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return c;
}

void respc_deadline(respc *c, unsigned timeout_ms) {
    c->deadline = mono_ms() + timeout_ms;
}

void respc_close(respc *c) {
    if (c != NULL) {
        close(c->fd);
        free(c);
    }
}

/* Writes n bytes before the deadline. Returns 0 or -1. */
static int write_all(respc *c, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(c->fd, p + off, n - off, MSG_NOSIGNAL);
        if (w > 0) {
            off += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && errno == EAGAIN) {
            if (wait_fd(c, POLLOUT) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

int respc_send(respc *c, int argc, const char *const *argv, const size_t *lens) {
    char head[32];
    int hl = snprintf(head, sizeof(head), "*%d\r\n", argc);
    if (write_all(c, head, (size_t)hl) != 0) {
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        size_t n = lens != NULL ? lens[i] : strlen(argv[i]);
        hl = snprintf(head, sizeof(head), "$%zu\r\n", n);
        if (write_all(c, head, (size_t)hl) != 0 || write_all(c, argv[i], n) != 0 ||
            write_all(c, "\r\n", 2) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Reads more input into the buffer before the deadline. Returns 0 or -1. */
static int fill(respc *c) {
    if (c->off > 0) {
        memmove(c->buf, c->buf + c->off, c->len - c->off);
        c->len -= c->off;
        c->off = 0;
    }
    if (c->len == sizeof(c->buf)) {
        return -1; /* a line longer than the buffer */
    }
    for (;;) {
        ssize_t r = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
        if (r > 0) {
            c->len += (size_t)r;
            return 0;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 && errno == EAGAIN) {
            if (wait_fd(c, POLLIN) != 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}

/* Reads one line (without CR LF) into a new string. */
static char *read_line(respc *c) {
    for (;;) {
        char *nl = memchr(c->buf + c->off, '\n', c->len - c->off);
        if (nl != NULL && nl > c->buf + c->off && nl[-1] == '\r') {
            size_t n = (size_t)(nl - 1 - (c->buf + c->off));
            char *s = malloc(n + 1);
            if (s == NULL) {
                return NULL;
            }
            memcpy(s, c->buf + c->off, n);
            s[n] = '\0';
            c->off += n + 2;
            return s;
        }
        if (fill(c) != 0) {
            return NULL;
        }
    }
}

/* Reads exactly n bytes into dst: first from the buffer, then from the socket. Returns 0 or -1. */
static int read_exact(respc *c, char *dst, size_t n) {
    size_t have = c->len - c->off < n ? c->len - c->off : n;
    memcpy(dst, c->buf + c->off, have);
    c->off += have;
    while (have < n) {
        ssize_t r = recv(c->fd, dst + have, n - have, 0);
        if (r > 0) {
            have += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && errno == EAGAIN) {
            if (wait_fd(c, POLLIN) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

static respv *read_value(respc *c, int depth) {
    if (depth > 8) {
        return NULL;
    }
    char *line = read_line(c);
    if (line == NULL) {
        return NULL;
    }
    respv *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        free(line);
        return NULL;
    }
    v->t = line[0];
    switch (line[0]) {
    case '+':
    case '-':
        v->s = strdup(line + 1);
        v->n = strlen(line + 1);
        if (v->s == NULL) {
            goto bad;
        }
        break;
    case ':':
        v->i = strtoll(line + 1, NULL, 10);
        break;
    case '$': {
        long long n = strtoll(line + 1, NULL, 10);
        if (n < 0) {
            break;
        }
        if ((unsigned long long)n > RESPC_MAX_BULK) {
            goto bad;
        }
        v->s = malloc((size_t)n + 1);
        char crlf[2];
        if (v->s == NULL || read_exact(c, v->s, (size_t)n) != 0 || read_exact(c, crlf, 2) != 0) {
            goto bad;
        }
        v->s[n] = '\0';
        v->n = (size_t)n;
        break;
    }
    case '*': {
        long long n = strtoll(line + 1, NULL, 10);
        if (n < 0) {
            break;
        }
        if (n > 1 << 20) {
            goto bad;
        }
        v->a = calloc(n > 0 ? (size_t)n : 1, sizeof(*v->a));
        if (v->a == NULL) {
            goto bad;
        }
        for (long long i = 0; i < n; i++) {
            v->a[i] = read_value(c, depth + 1);
            if (v->a[i] == NULL) {
                goto bad;
            }
            v->na++;
        }
        break;
    }
    default:
        goto bad;
    }
    free(line);
    return v;
bad:
    free(line);
    respv_free(v);
    return NULL;
}

respv *respc_read(respc *c) {
    return read_value(c, 0);
}

int respc_auth(respc *c, const char *user, const char *pass) {
    const char *argv[3] = {"AUTH", user, pass};
    int argc = 3;
    if (user == NULL || user[0] == '\0') {
        argv[1] = pass;
        argc = 2;
    }
    if (respc_send(c, argc, argv, NULL) != 0) {
        return -1;
    }
    respv *v = respc_read(c);
    int ok = v != NULL && v->t == '+';
    respv_free(v);
    return ok ? 0 : -1;
}

respv *respc_call(const char *host, int port, const char *user, const char *pass, unsigned timeout_ms,
                  int argc, const char *const *argv) {
    respc *c = respc_open(host, port, timeout_ms);
    if (c == NULL) {
        return NULL;
    }
    respv *v = NULL;
    if ((pass == NULL || pass[0] == '\0' || respc_auth(c, user, pass) == 0) &&
        respc_send(c, argc, argv, NULL) == 0) {
        v = respc_read(c);
    }
    respc_close(c);
    return v;
}
