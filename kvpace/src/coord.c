/* coord.c - coordinated runs of several load generators, see coord.h. */
#include "coord.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "merge.h"

#define RESULT_MAX (64u << 20) /* largest result accepted from a generator */
#define LINE_MAX_LEN 64        /* longest protocol line */

/* ---- common ------------------------------------------------------------------------------------------- */

/* Writes all n bytes (blocking socket). Returns 0 or -1. */
static int write_all(int fd, const void *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, (const char *)p + off, n - off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Splits "HOST:PORT" (the last colon separates; [IPv6]:PORT is accepted). Returns 0 or -1. */
static int split_hostport(const char *s, char *host, size_t hcap, char *port, size_t pcap) {
    const char *colon = strrchr(s, ':');
    if (colon == NULL || colon[1] == '\0') {
        return -1;
    }
    const char *h = s;
    size_t hl = (size_t)(colon - s);
    if (hl >= 2 && h[0] == '[' && h[hl - 1] == ']') {
        h++;
        hl -= 2;
    }
    if (hl >= hcap || strlen(colon + 1) >= pcap) {
        return -1;
    }
    memcpy(host, h, hl);
    host[hl] = '\0';
    snprintf(port, pcap, "%s", colon + 1);
    return 0;
}

/* ---- coordinator -------------------------------------------------------------------------------------- */

struct gen {
    int fd;
    int ready;
    int done; /* 1: result stored, -1: failed or disconnected */
    char line[LINE_MAX_LEN];
    size_t llen;
    char *res; /* result being received */
    size_t rlen;
    size_t rwant; /* bytes of the result announced by RESULT */
};

struct pace_barrier {
    int lfd;
    int port;
    unsigned count;
};

pace_barrier *pace_barrier_open(const char *addr, const char *port, unsigned count, char *err,
                                size_t errlen) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
    struct addrinfo *ai = NULL;
    int rc = getaddrinfo(addr != NULL && addr[0] != '\0' ? addr : NULL, port, &hints, &ai);
    if (rc != 0) {
        snprintf(err, errlen, "%s:%s: %s", addr ? addr : "*", port, gai_strerror(rc));
        return NULL;
    }
    int fd = -1;
    for (struct addrinfo *p = ai; p != NULL && fd < 0; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, p->ai_addr, p->ai_addrlen) != 0 || listen(fd, 64) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        snprintf(err, errlen, "listen on %s:%s: %s", addr ? addr : "*", port, strerror(errno));
        return NULL;
    }
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) {
        snprintf(err, errlen, "getsockname: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    pace_barrier *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    b->lfd = fd;
    b->count = count;
    b->port = ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&ss)->sin6_port
                                             : ((struct sockaddr_in *)&ss)->sin_port);
    return b;
}

int pace_barrier_port(const pace_barrier *b) {
    return b->port;
}

void pace_barrier_close(pace_barrier *b) {
    if (b != NULL) {
        close(b->lfd);
        free(b);
    }
}

/* Path of input k (1-based) next to out: "x.json" -> "x-k.json", otherwise "out-k.json". */
static void input_path(const char *out, unsigned k, char *buf, size_t cap) {
    size_t n = strlen(out);
    if (n > 5 && strcmp(out + n - 5, ".json") == 0) {
        snprintf(buf, cap, "%.*s-%u.json", (int)(n - 5), out, k);
    } else {
        snprintf(buf, cap, "%s-%u.json", out, k);
    }
}

/*
 * Consumes input of generator g: protocol lines, and the bytes of an announced result. Returns 0, or -1
 * on a protocol error (the generator is then marked failed).
 */
static int gen_input(struct gen *g, const char *p, size_t n, unsigned k, const char *out) {
    while (n > 0) {
        if (g->rwant > 0) {
            size_t take = g->rwant - g->rlen < n ? g->rwant - g->rlen : n;
            memcpy(g->res + g->rlen, p, take);
            g->rlen += take;
            p += take;
            n -= take;
            if (g->rlen == g->rwant) {
                char path[4096];
                input_path(out, k, path, sizeof(path));
                FILE *f = fopen(path, "w");
                int ok = f != NULL && fwrite(g->res, 1, g->rlen, f) == g->rlen;
                if (f != NULL && fclose(f) != 0) {
                    ok = 0;
                }
                if (!ok) {
                    fprintf(stderr, "barrier: cannot write %s\n", path);
                }
                free(g->res);
                g->res = NULL;
                g->rwant = 0;
                g->done = ok ? 1 : -1;
            }
            continue;
        }
        char c = *p++;
        n--;
        if (c != '\n') {
            if (g->llen + 1 >= sizeof(g->line)) {
                return -1;
            }
            g->line[g->llen++] = c;
            continue;
        }
        g->line[g->llen] = '\0';
        g->llen = 0;
        if (strcmp(g->line, "READY") == 0 && !g->ready) {
            g->ready = 1;
        } else if (strcmp(g->line, "FAILED") == 0) {
            g->done = -1;
        } else if (strncmp(g->line, "RESULT ", 7) == 0 && g->ready && g->done == 0) {
            char *end;
            unsigned long long v = strtoull(g->line + 7, &end, 10);
            if (*end != '\0' || v == 0 || v > RESULT_MAX) {
                return -1;
            }
            g->res = malloc((size_t)v);
            if (g->res == NULL) {
                return -1;
            }
            g->rwant = (size_t)v;
            g->rlen = 0;
        } else {
            return -1;
        }
    }
    return 0;
}

int pace_barrier_run(pace_barrier *b, const char *out, double timeout_s) {
    struct gen *gs = calloc(b->count, sizeof(*gs));
    struct pollfd *pf = calloc((size_t)b->count + 1, sizeof(*pf));
    if (gs == NULL || pf == NULL) {
        free(gs);
        free(pf);
        return -1;
    }
    for (unsigned i = 0; i < b->count; i++) {
        gs[i].fd = -1;
    }
    double deadline = timeout_s > 0 ? mono_s() + timeout_s : 0.0;
    unsigned joined = 0;
    int started = 0;
    int rc = 0;
    for (;;) {
        unsigned ready = 0;
        unsigned finished = 0;
        int failed = 0;
        for (unsigned i = 0; i < joined; i++) {
            ready += gs[i].ready;
            finished += gs[i].done != 0;
            failed |= gs[i].done < 0;
        }
        if (failed) {
            fprintf(stderr, "barrier: a generator failed or disconnected\n");
            rc = -1;
            break;
        }
        if (!started && joined == b->count && ready == b->count) {
            for (unsigned i = 0; i < b->count; i++) {
                if (write_all(gs[i].fd, "GO\n", 3) != 0) {
                    gs[i].done = -1;
                }
            }
            started = 1;
            fprintf(stderr, "barrier: %u generators started\n", b->count);
            continue;
        }
        if (started && finished == b->count) {
            break;
        }
        int timeout_ms = -1;
        if (deadline > 0) {
            double left = deadline - mono_s();
            if (left <= 0) {
                fprintf(stderr,
                        "barrier: timed out with %u of %u generators connected, %u ready, %u finished\n",
                        joined, b->count, ready, finished);
                rc = -1;
                break;
            }
            timeout_ms = (int)(left * 1000) + 1;
        }
        nfds_t nf = 0;
        if (joined < b->count) {
            pf[nf++] = (struct pollfd){.fd = b->lfd, .events = POLLIN};
        }
        for (unsigned i = 0; i < joined; i++) {
            pf[nf++] = (struct pollfd){.fd = gs[i].fd, .events = POLLIN};
        }
        int n = poll(pf, nf, timeout_ms);
        if (n < 0 && errno != EINTR) {
            rc = -1;
            break;
        }
        nfds_t at = 0;
        if (joined < b->count) {
            if (pf[0].revents & POLLIN) {
                int fd = accept4(b->lfd, NULL, NULL, SOCK_CLOEXEC);
                if (fd >= 0) {
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    gs[joined++].fd = fd;
                }
            }
            at = 1;
        }
        for (unsigned i = 0; i < joined && at < nf; i++, at++) {
            if (!(pf[at].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            char buf[65536];
            ssize_t r = recv(gs[i].fd, buf, sizeof(buf), 0);
            if (r <= 0) {
                if (gs[i].done == 0) {
                    gs[i].done = -1;
                }
                continue;
            }
            if (gen_input(&gs[i], buf, (size_t)r, i + 1, out) != 0) {
                gs[i].done = -1;
            }
        }
    }
    for (unsigned i = 0; i < joined; i++) {
        close(gs[i].fd);
        free(gs[i].res);
    }
    free(pf);
    if (rc == 0) {
        /* argv of "kvpace merge": merge --out OUT IN1 IN2 ... */
        char **av = calloc((size_t)b->count + 3, sizeof(*av));
        char(*paths)[4096] = calloc(b->count, sizeof(*paths));
        if (av == NULL || paths == NULL) {
            rc = -1;
        } else {
            av[0] = "merge";
            av[1] = "--out";
            av[2] = (char *)out;
            for (unsigned i = 0; i < b->count; i++) {
                input_path(out, i + 1, paths[i], sizeof(paths[i]));
                av[3 + i] = paths[i];
            }
            rc = pace_merge_main((int)b->count + 3, av) == 0 ? 0 : -1;
        }
        free(av);
        free(paths);
    }
    free(gs);
    return rc;
}

int pace_barrier_main(int argc, char **argv) {
    const char *listen_at = NULL;
    const char *out = NULL;
    unsigned long count = 0;
    double timeout_s = 3600.0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end = NULL;
        if (strcmp(a, "--listen") == 0 && v != NULL) {
            listen_at = v;
        } else if (strcmp(a, "--out") == 0 && v != NULL) {
            out = v;
        } else if (strcmp(a, "--count") == 0 && v != NULL) {
            count = strtoul(v, &end, 10);
            if (*end != '\0' || count == 0 || count > 4096) {
                fprintf(stderr, "barrier: --count needs a number from 1 to 4096\n");
                return 2;
            }
        } else if (strcmp(a, "--timeout") == 0 && v != NULL) {
            timeout_s = strtod(v, &end);
            if (*end != '\0' || timeout_s < 0) {
                fprintf(stderr, "barrier: --timeout needs seconds, 0 or more (0: no limit)\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: kvpace barrier --listen [ADDR:]PORT --count N --out MERGED.json "
                            "[--timeout SEC]\n");
            return 2;
        }
        i++;
    }
    if (listen_at == NULL || out == NULL || count == 0) {
        fprintf(stderr,
                "usage: kvpace barrier --listen [ADDR:]PORT --count N --out MERGED.json [--timeout SEC]\n");
        return 2;
    }
    char host[256] = "";
    char port[32];
    if (strchr(listen_at, ':') == NULL) {
        snprintf(port, sizeof(port), "%s", listen_at);
    } else if (split_hostport(listen_at, host, sizeof(host), port, sizeof(port)) != 0) {
        fprintf(stderr, "barrier: bad --listen %s\n", listen_at);
        return 2;
    }
    char err[256];
    pace_barrier *b = pace_barrier_open(host, port, (unsigned)count, err, sizeof(err));
    if (b == NULL) {
        fprintf(stderr, "barrier: %s\n", err);
        return 1;
    }
    fprintf(stderr, "barrier: waiting for %lu generators on port %d\n", count, pace_barrier_port(b));
    int rc = pace_barrier_run(b, out, timeout_s);
    pace_barrier_close(b);
    return rc == 0 ? 0 : 1;
}

/* ---- generator ---------------------------------------------------------------------------------------- */

struct pace_coord {
    int fd;
};

pace_coord *pace_coord_connect(const char *hostport, char *err, size_t errlen) {
    char host[256];
    char port[32];
    if (split_hostport(hostport, host, sizeof(host), port, sizeof(port)) != 0) {
        snprintf(err, errlen, "coordinator address %s is not HOST:PORT", hostport);
        return NULL;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *ai = NULL;
    int rc = getaddrinfo(host, port, &hints, &ai);
    if (rc != 0) {
        snprintf(err, errlen, "coordinator %s: %s", hostport, gai_strerror(rc));
        return NULL;
    }
    int fd = -1;
    for (struct addrinfo *p = ai; p != NULL && fd < 0; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (fd >= 0 && connect(fd, p->ai_addr, p->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        snprintf(err, errlen, "coordinator %s: %s", hostport, strerror(errno));
        return NULL;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    pace_coord *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    c->fd = fd;
    return c;
}

int pace_coord_start(void *arg) {
    pace_coord *c = arg;
    if (write_all(c->fd, "READY\n", 6) != 0) {
        fprintf(stderr, "coordinator: connection lost\n");
        return -1;
    }
    char line[LINE_MAX_LEN];
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t r = recv(c->fd, &ch, 1, 0);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            fprintf(stderr, "coordinator: connection lost before the start\n");
            return -1;
        }
        if (ch == '\n') {
            break;
        }
        if (n + 1 >= sizeof(line)) {
            return -1;
        }
        line[n++] = ch;
    }
    line[n] = '\0';
    if (strcmp(line, "GO") != 0) {
        fprintf(stderr, "coordinator: unexpected message '%s'\n", line);
        return -1;
    }
    return 0;
}

int pace_coord_result(pace_coord *c, const char *json, size_t len) {
    char head[64];
    int hl = snprintf(head, sizeof(head), "RESULT %zu\n", len);
    return write_all(c->fd, head, (size_t)hl) == 0 && write_all(c->fd, json, len) == 0 ? 0 : -1;
}

void pace_coord_failed(pace_coord *c) {
    (void)write_all(c->fd, "FAILED\n", 7);
}

void pace_coord_close(pace_coord *c) {
    if (c != NULL) {
        close(c->fd);
        free(c);
    }
}
