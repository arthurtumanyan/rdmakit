/*
 * sentinel.c - automatic failover monitor, see sentinel.h.
 *
 * Threads: the monitor thread polls the nodes and the other monitors and runs elections and failovers;
 * the listener thread answers RESP requests of clients and of the other monitors. Shared state (epochs,
 * vote, configured primary, the latest observations) is guarded by one mutex; network calls are made
 * without holding it. Every call to a node or monitor uses a new TCP connection and a deadline (respc.h), so
 * that a dead or stalled peer costs at most the call timeout per tick.
 */
#include "sentinel.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "respc.h"

#define TICK_MS 100        /* monitor loop period; the primary is pinged every tick */
#define REFRESH_MS 1000    /* ROLE of every node, configuration exchange, s_down questions */
#define PEER_FRESH_MS 3000 /* a monitor answered within this time counts as reachable */
#define MAX_CLIENTS 64
#define MAX_REQUEST (64u << 10)

/* ---- small helpers -------------------------------------------------------------------------------------- */

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Logs an event line: UTC time, group name, event (as Redis Sentinel names them: +sdown, +odown, ...). */
static void slog(const char *name, const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s sentinel %s: %s\n", when, name, msg);
    fflush(stderr);
}

/* Growing output buffer for replies. */
struct sbuf {
    char *p;
    size_t len;
    size_t cap;
};

static void sb_add(struct sbuf *b, const void *data, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) {
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (p == NULL) {
            return; /* the reply is then truncated and the client sees a protocol error */
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void sb_printf(struct sbuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_add(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

static void sb_bulk(struct sbuf *b, const char *s) {
    sb_printf(b, "$%zu\r\n", strlen(s));
    sb_add(b, s, strlen(s));
    sb_add(b, "\r\n", 2);
}

/* A "key" "value" pair of a flat state array (SENTINEL MASTERS and friends). */
static void sb_kv(struct sbuf *b, const char *k, const char *v) {
    sb_bulk(b, k);
    sb_bulk(b, v);
}

static void sb_kvi(struct sbuf *b, const char *k, long long v) {
    char t[32];
    snprintf(t, sizeof(t), "%lld", v);
    sb_kv(b, k, t);
}

/* ---- decision rules ------------------------------------------------------------------------------------- */

int sentinel_pick(const sentinel_candidate *c, unsigned n, unsigned primary) {
    int best = -1;
    for (unsigned i = 0; i < n; i++) {
        if (i == primary || !c[i].reachable || !c[i].is_replica) {
            continue;
        }
        if (best < 0 || c[i].offset > c[best].offset) {
            best = (int)i;
        }
    }
    return best;
}

unsigned sentinel_votes_needed(unsigned monitors, unsigned quorum) {
    unsigned majority = monitors / 2 + 1;
    return quorum > majority ? quorum : majority;
}

int sentinel_vote_request(sentinel_vote *v, uint64_t epoch, const char *id) {
    int changed = 0;
    if (epoch > v->current_epoch) {
        v->current_epoch = epoch;
        changed = 1;
    }
    if (v->voted_epoch < epoch && epoch >= v->current_epoch) {
        v->voted_epoch = epoch;
        snprintf(v->voted_for, sizeof(v->voted_for), "%s", id);
        changed = 1;
    }
    return changed;
}

/* ---- monitor state -------------------------------------------------------------------------------------- */

struct node_rt {
    int reachable;       /* the last call succeeded within the timeout */
    uint64_t last_ok_ms; /* last valid reply */
    int is_replica;      /* ROLE says replica */
    int is_primary;      /* ROLE says primary */
    char master_host[256];
    int master_port; /* native port of the node it follows */
    int link_up;     /* replica link state "connected" */
    uint64_t offset;
};

struct peer_rt {
    uint64_t last_seen_ms; /* last answer to a configuration exchange */
    char id[SENTINEL_ID_LEN + 1];
    int says_down; /* answered that the primary is down */
};

struct sentinel {
    sentinel_cfg cfg;
    char id[SENTINEL_ID_LEN + 1];
    pthread_mutex_t m;
    /* persistent state, stored in cfg.state_path before it is acted on or answered */
    sentinel_vote vote;
    uint64_t config_epoch;
    unsigned primary;
    /* observations */
    struct node_rt nodes[SENTINEL_MAX_NODES];
    struct peer_rt peers[SENTINEL_MAX_PEERS];
    int sdown;
    int odown;
    uint64_t sdown_since;
    uint64_t next_attempt_ms; /* earliest start of the next election */
    int forced;               /* SENTINEL FAILOVER was requested */
    /* threads */
    int lfd;
    int port;
    atomic_int stop;
    pthread_t monitor;
    pthread_t listener;
};

/* Number of monitors including this one. */
static unsigned monitors(const struct sentinel *s) {
    return s->cfg.npeers + 1;
}

static unsigned quorum(const struct sentinel *s) {
    return s->cfg.quorum ? s->cfg.quorum : monitors(s) / 2 + 1;
}

/* Call timeout for nodes and monitors: half the down detection time, between 100 ms and 1 s. */
static unsigned call_ms(const struct sentinel *s) {
    unsigned t = s->cfg.down_after_ms / 2;
    return t < 100 ? 100 : t > 1000 ? 1000 : t;
}

/* Writes the persistent state (write, fsync, rename). Returns 0 or -1. Called with s->m held. */
static int store_state(struct sentinel *s) {
    if (s->cfg.state_path[0] == '\0') {
        return 0;
    }
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->cfg.state_path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "myid %s\ncurrent_epoch %llu\nvoted_epoch %llu\nvoted_for %s\nconfig_epoch %llu\nprimary %u\n",
            s->id, (unsigned long long)s->vote.current_epoch, (unsigned long long)s->vote.voted_epoch,
            s->vote.voted_for[0] ? s->vote.voted_for : "-", (unsigned long long)s->config_epoch, s->primary);
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, s->cfg.state_path) != 0) {
        slog(s->cfg.name, "-state-write %s: %s", s->cfg.state_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* Loads the state file, if any. Returns 0, or -1 when it exists but is unreadable or refers to no node. */
static int load_state(struct sentinel *s) {
    if (s->cfg.state_path[0] == '\0') {
        return 0;
    }
    FILE *f = fopen(s->cfg.state_path, "r");
    if (f == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    char key[32];
    char val[64];
    int rc = 0;
    while (fscanf(f, "%31s %63s", key, val) == 2) {
        if (strcmp(key, "myid") == 0 && strlen(val) == SENTINEL_ID_LEN) {
            snprintf(s->id, sizeof(s->id), "%s", val);
        } else if (strcmp(key, "current_epoch") == 0) {
            s->vote.current_epoch = strtoull(val, NULL, 10);
        } else if (strcmp(key, "voted_epoch") == 0) {
            s->vote.voted_epoch = strtoull(val, NULL, 10);
        } else if (strcmp(key, "voted_for") == 0) {
            snprintf(s->vote.voted_for, sizeof(s->vote.voted_for), "%s", strcmp(val, "-") == 0 ? "" : val);
        } else if (strcmp(key, "config_epoch") == 0) {
            s->config_epoch = strtoull(val, NULL, 10);
        } else if (strcmp(key, "primary") == 0) {
            s->primary = (unsigned)strtoul(val, NULL, 10);
            if (s->primary >= s->cfg.nnodes) {
                rc = -1;
            }
        }
    }
    fclose(f);
    return rc;
}

/* Adopts a configuration seen elsewhere when its epoch is newer. Called with s->m held. */
static void adopt_config(struct sentinel *s, uint64_t epoch, unsigned primary, const char *from) {
    if (epoch > s->config_epoch && primary < s->cfg.nnodes) {
        unsigned old = s->primary;
        s->config_epoch = epoch;
        s->primary = primary;
        if (epoch > s->vote.current_epoch) {
            s->vote.current_epoch = epoch;
        }
        s->sdown = 0;
        s->odown = 0;
        s->forced = 0;
        store_state(s);
        if (old != primary) {
            const sentinel_node_cfg *o = &s->cfg.nodes[old];
            const sentinel_node_cfg *n = &s->cfg.nodes[primary];
            slog(s->cfg.name, "+switch-master %s %d %s %d (configuration epoch %llu from %s)", o->host,
                 o->resp_port, n->host, n->resp_port, (unsigned long long)epoch, from);
        }
    }
}

/* ---- listener: requests of clients and of the other monitors -------------------------------------------- */

/* Parses one RESP array command (or an inline command) at p. Returns bytes used, 0 incomplete, -1 error. */
static long parse_request(const char *p, size_t n, char **argv, int *argc, int maxargs) {
    size_t off = 0;
    if (n == 0) {
        return 0;
    }
    if (p[0] != '*') {
        const char *nl = memchr(p, '\n', n);
        if (nl == NULL) {
            return n > 4096 ? -1 : 0;
        }
        size_t len = (size_t)(nl - p);
        char *line = strndup(p, len > 0 && p[len - 1] == '\r' ? len - 1 : len);
        if (line == NULL) {
            return -1;
        }
        *argc = 0;
        char *save = NULL;
        int failed = 0;
        for (char *t = strtok_r(line, " \t", &save); t != NULL && *argc < maxargs;
             t = strtok_r(NULL, " \t", &save)) {
            argv[*argc] = strdup(t);
            failed |= argv[*argc] == NULL;
            (*argc)++;
        }
        free(line);
        return failed ? -1 : (long)len + 1;
    }
    const char *nl = memchr(p, '\n', n);
    if (nl == NULL) {
        return n > 64 ? -1 : 0;
    }
    long count = strtol(p + 1, NULL, 10);
    if (count < 1 || count > maxargs) {
        return -1;
    }
    off = (size_t)(nl - p) + 1;
    for (long i = 0; i < count; i++) {
        if (off >= n) {
            goto incomplete;
        }
        if (p[off] != '$') {
            goto error;
        }
        const char *e = memchr(p + off, '\n', n - off);
        if (e == NULL) {
            goto incomplete;
        }
        long len = strtol(p + off + 1, NULL, 10);
        if (len < 0 || len > (long)MAX_REQUEST) {
            goto error;
        }
        off = (size_t)(e - p) + 1;
        if (off + (size_t)len + 2 > n) {
            goto incomplete;
        }
        argv[i] = strndup(p + off, (size_t)len);
        off += (size_t)len + 2;
    }
    *argc = (int)count;
    return (long)off;
incomplete:
    for (long i = 0; i < count; i++) {
        free(argv[i]);
        argv[i] = NULL;
    }
    return 0;
error:
    for (long i = 0; i < count; i++) {
        free(argv[i]);
        argv[i] = NULL;
    }
    return -1;
}

/* State array of the primary (SENTINEL MASTERS, SENTINEL MASTER). Called with s->m held. */
static void master_entry(struct sentinel *s, struct sbuf *o) {
    const sentinel_node_cfg *p = &s->cfg.nodes[s->primary];
    char flags[64];
    snprintf(flags, sizeof(flags), "master%s%s", s->sdown ? ",s_down" : "", s->odown ? ",o_down" : "");
    sb_add(o, "*22\r\n", 5);
    sb_kv(o, "name", s->cfg.name);
    sb_kv(o, "ip", p->host);
    sb_kvi(o, "port", p->resp_port);
    sb_kv(o, "runid", "");
    sb_kv(o, "flags", flags);
    sb_kvi(o, "num-slaves", (long long)s->cfg.nnodes - 1);
    sb_kvi(o, "num-other-sentinels", s->cfg.npeers);
    sb_kvi(o, "quorum", quorum(s));
    sb_kvi(o, "config-epoch", (long long)s->config_epoch);
    sb_kvi(o, "down-after-milliseconds", s->cfg.down_after_ms);
    sb_kvi(o, "failover-timeout", s->cfg.failover_timeout_ms);
}

static void reply_sentinel(struct sentinel *s, char **argv, int argc, struct sbuf *o) {
    const char *sub = argv[1];
    int named = argc >= 3 && strcmp(argv[2], s->cfg.name) == 0;
    pthread_mutex_lock(&s->m);
    if (strcasecmp(sub, "get-master-addr-by-name") == 0 && argc == 3) {
        if (named) {
            const sentinel_node_cfg *p = &s->cfg.nodes[s->primary];
            char port[16];
            snprintf(port, sizeof(port), "%d", p->resp_port);
            sb_add(o, "*2\r\n", 4);
            sb_bulk(o, p->host);
            sb_bulk(o, port);
        } else {
            sb_add(o, "*-1\r\n", 5);
        }
    } else if (strcasecmp(sub, "masters") == 0 && argc == 2) {
        sb_add(o, "*1\r\n", 4);
        master_entry(s, o);
    } else if (strcasecmp(sub, "master") == 0 && named) {
        master_entry(s, o);
    } else if ((strcasecmp(sub, "replicas") == 0 || strcasecmp(sub, "slaves") == 0) && named) {
        sb_printf(o, "*%u\r\n", s->cfg.nnodes - 1);
        for (unsigned i = 0; i < s->cfg.nnodes; i++) {
            if (i == s->primary) {
                continue;
            }
            const sentinel_node_cfg *n = &s->cfg.nodes[i];
            const struct node_rt *rt = &s->nodes[i];
            char nm[300];
            snprintf(nm, sizeof(nm), "%s:%d", n->host, n->resp_port);
            sb_add(o, "*16\r\n", 5);
            sb_kv(o, "name", nm);
            sb_kv(o, "ip", n->host);
            sb_kvi(o, "port", n->resp_port);
            sb_kv(o, "runid", "");
            sb_kv(o, "flags", rt->reachable ? "slave" : "slave,s_down");
            sb_kv(o, "master-host", rt->master_host);
            sb_kv(o, "master-link-status", rt->link_up ? "ok" : "err");
            sb_kvi(o, "slave-repl-offset", (long long)rt->offset);
        }
    } else if (strcasecmp(sub, "sentinels") == 0 && named) {
        uint64_t now = mono_ms();
        sb_printf(o, "*%u\r\n", s->cfg.npeers);
        for (unsigned i = 0; i < s->cfg.npeers; i++) {
            const sentinel_peer_cfg *p = &s->cfg.peers[i];
            char nm[300];
            snprintf(nm, sizeof(nm), "%s:%d", p->host, p->port);
            int fresh = s->peers[i].last_seen_ms != 0 && now - s->peers[i].last_seen_ms < PEER_FRESH_MS;
            sb_add(o, "*10\r\n", 5);
            sb_kv(o, "name", nm);
            sb_kv(o, "ip", p->host);
            sb_kvi(o, "port", p->port);
            sb_kv(o, "runid", s->peers[i].id);
            sb_kv(o, "flags", fresh ? "sentinel" : "sentinel,s_down");
        }
    } else if (strcasecmp(sub, "is-master-down-by-addr") == 0 && argc == 6) {
        /* ip port current-epoch runid: "*" asks only for the down state; an identity asks for a vote */
        const sentinel_node_cfg *p = &s->cfg.nodes[s->primary];
        int down = s->sdown && strcmp(argv[2], p->host) == 0 && atoi(argv[3]) == p->resp_port;
        uint64_t epoch = strtoull(argv[4], NULL, 10);
        const char *who = argv[5];
        if (strcmp(who, "*") != 0 && strlen(who) == SENTINEL_ID_LEN) {
            uint64_t before = s->vote.voted_epoch;
            if (sentinel_vote_request(&s->vote, epoch, who)) {
                store_state(s);
            }
            if (s->vote.voted_epoch != before) {
                slog(s->cfg.name, "+vote-for-leader %s %llu", who, (unsigned long long)epoch);
                /* another monitor leads this attempt: do not start a competing one right away */
                s->next_attempt_ms = mono_ms() + s->cfg.failover_timeout_ms;
            }
        } else if (epoch > s->vote.current_epoch) {
            s->vote.current_epoch = epoch;
            store_state(s);
        }
        sb_printf(o, "*3\r\n:%d\r\n", down);
        sb_bulk(o, s->vote.voted_epoch == epoch && s->vote.voted_for[0] ? s->vote.voted_for : "*");
        sb_printf(o, ":%llu\r\n", (unsigned long long)s->vote.voted_epoch);
    } else if (strcasecmp(sub, "akv-hello") == 0 && argc == 7 && named) {
        /* name id current-epoch config-epoch primary: exchange of configurations between monitors */
        uint64_t cur = strtoull(argv[4], NULL, 10);
        if (cur > s->vote.current_epoch) {
            s->vote.current_epoch = cur;
            store_state(s);
        }
        adopt_config(s, strtoull(argv[5], NULL, 10), (unsigned)strtoul(argv[6], NULL, 10), argv[3]);
        sb_add(o, "*4\r\n", 4);
        sb_bulk(o, s->id);
        sb_printf(o, ":%llu\r\n:%llu\r\n:%u\r\n", (unsigned long long)s->vote.current_epoch,
                  (unsigned long long)s->config_epoch, s->primary);
    } else if (strcasecmp(sub, "failover") == 0 && named) {
        s->forced = 1;
        s->next_attempt_ms = 0;
        sb_add(o, "+OK\r\n", 5);
    } else if (strcasecmp(sub, "ckquorum") == 0 && named) {
        uint64_t now = mono_ms();
        unsigned usable = 1;
        for (unsigned i = 0; i < s->cfg.npeers; i++) {
            usable += s->peers[i].last_seen_ms != 0 && now - s->peers[i].last_seen_ms < PEER_FRESH_MS;
        }
        unsigned need = sentinel_votes_needed(monitors(s), quorum(s));
        if (usable >= need) {
            sb_printf(o, "+OK %u usable Sentinels. Quorum and failover authorization can be reached\r\n",
                      usable);
        } else {
            sb_printf(o, "-NOQUORUM %u usable Sentinels of %u needed\r\n", usable, need);
        }
    } else if (strcasecmp(sub, "myid") == 0 && argc == 2) {
        sb_bulk(o, s->id);
    } else if (argc >= 3 && !named) {
        sb_add(o, "-ERR No such master with that name\r\n", 36);
    } else {
        sb_printf(o, "-ERR unknown or malformed SENTINEL subcommand '%.32s'\r\n", sub);
    }
    pthread_mutex_unlock(&s->m);
}

static void reply(struct sentinel *s, char **argv, int argc, struct sbuf *o) {
    if (strcasecmp(argv[0], "PING") == 0) {
        sb_add(o, "+PONG\r\n", 7);
    } else if (strcasecmp(argv[0], "ROLE") == 0) {
        sb_add(o, "*2\r\n", 4);
        sb_bulk(o, "sentinel");
        sb_add(o, "*1\r\n", 4);
        sb_bulk(o, s->cfg.name);
    } else if (strcasecmp(argv[0], "SENTINEL") == 0 && argc >= 2) {
        reply_sentinel(s, argv, argc, o);
    } else if (strcasecmp(argv[0], "HELLO") == 0 || strcasecmp(argv[0], "CLIENT") == 0) {
        sb_add(o, "-ERR unknown command\r\n", 22); /* clients fall back to RESP2 without names */
    } else {
        sb_printf(o, "-ERR unknown command '%.32s'\r\n", argv[0]);
    }
}

struct client {
    int fd;
    char *in;
    size_t len;
};

static void *listener_main(void *arg) {
    struct sentinel *s = arg;
    struct client cl[MAX_CLIENTS];
    unsigned ncl = 0;
    while (!atomic_load(&s->stop)) {
        struct pollfd pf[MAX_CLIENTS + 1];
        pf[0] = (struct pollfd){.fd = s->lfd, .events = POLLIN};
        for (unsigned i = 0; i < ncl; i++) {
            pf[i + 1] = (struct pollfd){.fd = cl[i].fd, .events = POLLIN};
        }
        if (poll(pf, ncl + 1, 200) <= 0) {
            continue;
        }
        if (pf[0].revents & POLLIN) {
            int fd = accept4(s->lfd, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0 && ncl < MAX_CLIENTS) {
                cl[ncl++] = (struct client){.fd = fd};
            } else if (fd >= 0) {
                close(fd);
            }
        }
        for (unsigned i = 0; i < ncl;) {
            int drop = 0;
            if (pf[i + 1].fd == cl[i].fd && (pf[i + 1].revents & (POLLIN | POLLHUP | POLLERR))) {
                char buf[4096];
                ssize_t r = recv(cl[i].fd, buf, sizeof(buf), 0);
                if (r <= 0 || cl[i].len + (size_t)r > MAX_REQUEST) {
                    drop = 1;
                } else {
                    char *p = realloc(cl[i].in, cl[i].len + (size_t)r);
                    if (p == NULL) {
                        drop = 1;
                    } else {
                        cl[i].in = p;
                        memcpy(p + cl[i].len, buf, (size_t)r);
                        cl[i].len += (size_t)r;
                    }
                }
                while (!drop && cl[i].len > 0) {
                    char *argv[16] = {0};
                    int argc = 0;
                    long used = parse_request(cl[i].in, cl[i].len, argv, &argc, 16);
                    int complete = used > 0 && argc > 0;
                    for (int k = 0; complete && k < argc; k++) {
                        complete = argv[k] != NULL;
                    }
                    struct sbuf o = {0};
                    if (complete) {
                        reply(s, argv, argc, &o);
                    }
                    for (int k = 0; k < 16; k++) {
                        free(argv[k]); /* every slot: the parser may have filled some on failure */
                    }
                    if (used == 0) {
                        free(o.p);
                        break;
                    }
                    if (used < 0) {
                        free(o.p);
                        drop = 1;
                        break;
                    }
                    memmove(cl[i].in, cl[i].in + used, cl[i].len - (size_t)used);
                    cl[i].len -= (size_t)used;
                    /* replies are small: a blocking send with a bound (SO_SNDTIMEO) is sufficient */
                    struct timeval tv = {.tv_sec = 1};
                    setsockopt(cl[i].fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                    size_t off = 0;
                    while (off < o.len) {
                        ssize_t w = send(cl[i].fd, o.p + off, o.len - off, MSG_NOSIGNAL);
                        if (w <= 0) {
                            drop = 1;
                            break;
                        }
                        off += (size_t)w;
                    }
                    free(o.p);
                }
            }
            if (drop) {
                close(cl[i].fd);
                free(cl[i].in);
                cl[i] = cl[--ncl];
                pf[i + 1] = pf[ncl + 1];
            } else {
                i++;
            }
        }
    }
    for (unsigned i = 0; i < ncl; i++) {
        close(cl[i].fd);
        free(cl[i].in);
    }
    return NULL;
}

/* ---- monitor --------------------------------------------------------------------------------------------- */

static respv *node_call(struct sentinel *s, unsigned i, int argc, const char *const *argv) {
    const sentinel_node_cfg *n = &s->cfg.nodes[i];
    return respc_call(n->host, n->resp_port, s->cfg.node_user, s->cfg.node_pass, call_ms(s), argc, argv);
}

/*
 * REPLICAOF on node i. Changing a node's role stops its replication link thread, which waits up to 1 s in a
 * receive (replication.c), so the call gets the failover timeout (at most 10 s) instead of the ping timeout.
 */
static respv *node_replicaof(struct sentinel *s, unsigned i, int argc, const char *const *argv) {
    const sentinel_node_cfg *n = &s->cfg.nodes[i];
    unsigned t = s->cfg.failover_timeout_ms < 10000 ? s->cfg.failover_timeout_ms : 10000;
    return respc_call(n->host, n->resp_port, s->cfg.node_user, s->cfg.node_pass, t, argc, argv);
}

static respv *peer_call(struct sentinel *s, unsigned i, int argc, const char *const *argv) {
    const sentinel_peer_cfg *p = &s->cfg.peers[i];
    return respc_call(p->host, p->port, NULL, NULL, call_ms(s), argc, argv);
}

/* A node answer that shows the node is up: anything but a missing reply or an error other than LOADING. */
static int alive(const respv *v) {
    return v != NULL && (v->t != '-' || (v->s != NULL && strncmp(v->s, "LOADING", 7) == 0));
}

/* Refreshes the observation of node i with ROLE. */
static void refresh_node(struct sentinel *s, unsigned i) {
    static const char *const role[1] = {"ROLE"};
    respv *v = node_call(s, i, 1, role);
    struct node_rt rt = {0};
    if (alive(v)) {
        rt.reachable = 1;
        rt.last_ok_ms = mono_ms();
        if (v->t == '*' && v->na >= 1 && v->a[0]->s != NULL) {
            if (strcmp(v->a[0]->s, "master") == 0) {
                rt.is_primary = 1;
                rt.offset = v->na >= 2 ? (uint64_t)v->a[1]->i : 0;
            } else if (strcmp(v->a[0]->s, "slave") == 0 && v->na >= 5) {
                rt.is_replica = 1;
                snprintf(rt.master_host, sizeof(rt.master_host), "%s", v->a[1]->s ? v->a[1]->s : "");
                rt.master_port = (int)v->a[2]->i;
                rt.link_up = v->a[3]->s != NULL && strcmp(v->a[3]->s, "connected") == 0;
                rt.offset = (uint64_t)v->a[4]->i;
            }
        }
    }
    respv_free(v);
    pthread_mutex_lock(&s->m);
    if (rt.reachable) {
        s->nodes[i] = rt;
    } else {
        s->nodes[i].reachable = 0;
    }
    pthread_mutex_unlock(&s->m);
}

/* Exchanges configurations with every monitor. */
static void hello_peers(struct sentinel *s) {
    char cur[24];
    char cfg_epoch[24];
    char prim[16];
    pthread_mutex_lock(&s->m);
    snprintf(cur, sizeof(cur), "%llu", (unsigned long long)s->vote.current_epoch);
    snprintf(cfg_epoch, sizeof(cfg_epoch), "%llu", (unsigned long long)s->config_epoch);
    snprintf(prim, sizeof(prim), "%u", s->primary);
    pthread_mutex_unlock(&s->m);
    const char *argv[7] = {"SENTINEL", "AKV-HELLO", s->cfg.name, s->id, cur, cfg_epoch, prim};
    for (unsigned i = 0; i < s->cfg.npeers; i++) {
        respv *v = peer_call(s, i, 7, argv);
        if (v != NULL && v->t == '*' && v->na == 4 && v->a[0]->s != NULL) {
            pthread_mutex_lock(&s->m);
            s->peers[i].last_seen_ms = mono_ms();
            snprintf(s->peers[i].id, sizeof(s->peers[i].id), "%s", v->a[0]->s);
            if ((uint64_t)v->a[1]->i > s->vote.current_epoch) {
                s->vote.current_epoch = (uint64_t)v->a[1]->i;
                store_state(s);
            }
            adopt_config(s, (uint64_t)v->a[2]->i, (unsigned)v->a[3]->i, s->peers[i].id);
            pthread_mutex_unlock(&s->m);
        }
        respv_free(v);
    }
}

/*
 * Asks every monitor about the primary at epoch `epoch`; `who` is "*" (down state only) or this monitor's
 * identity (a vote request). Returns the monitors (this one included when it sees the primary down) that
 * answered down, and in *votes those that voted for this monitor in `epoch`.
 */
static unsigned ask_peers(struct sentinel *s, uint64_t epoch, const char *who, unsigned *votes) {
    char port[16];
    char ep[24];
    char host[256];
    pthread_mutex_lock(&s->m);
    snprintf(host, sizeof(host), "%s", s->cfg.nodes[s->primary].host);
    snprintf(port, sizeof(port), "%d", s->cfg.nodes[s->primary].resp_port);
    unsigned down = s->sdown;
    pthread_mutex_unlock(&s->m);
    snprintf(ep, sizeof(ep), "%llu", (unsigned long long)epoch);
    const char *argv[6] = {"SENTINEL", "is-master-down-by-addr", host, port, ep, who};
    *votes = 0;
    for (unsigned i = 0; i < s->cfg.npeers; i++) {
        respv *v = peer_call(s, i, 6, argv);
        if (v != NULL && v->t == '*' && v->na == 3) {
            down += v->a[0]->i == 1;
            if (strcmp(who, "*") != 0 && v->a[1]->s != NULL && strcmp(v->a[1]->s, s->id) == 0 &&
                (uint64_t)v->a[2]->i == epoch) {
                (*votes)++;
            }
        }
        respv_free(v);
    }
    return down;
}

/* Promotes the best replica in epoch `epoch` and reconfigures the group. Returns 0 or -1. */
static int failover(struct sentinel *s, uint64_t epoch) {
    for (unsigned i = 0; i < s->cfg.nnodes; i++) {
        refresh_node(s, i);
    }
    sentinel_candidate c[SENTINEL_MAX_NODES] = {0};
    pthread_mutex_lock(&s->m);
    unsigned old = s->primary;
    for (unsigned i = 0; i < s->cfg.nnodes; i++) {
        c[i] = (sentinel_candidate){.reachable = s->nodes[i].reachable,
                                    .is_replica = s->nodes[i].is_replica,
                                    .offset = s->nodes[i].offset};
    }
    pthread_mutex_unlock(&s->m);
    int pick = sentinel_pick(c, s->cfg.nnodes, old);
    if (pick < 0) {
        slog(s->cfg.name, "-failover-abort-no-good-slave (epoch %llu)", (unsigned long long)epoch);
        return -1;
    }
    const sentinel_node_cfg *n = &s->cfg.nodes[pick];
    slog(s->cfg.name, "+selected-slave %s %d (offset %llu)", n->host, n->resp_port,
         (unsigned long long)c[pick].offset);
    static const char *const noone[3] = {"REPLICAOF", "NO", "ONE"};
    respv *v = node_replicaof(s, (unsigned)pick, 3, noone);
    int ok = v != NULL && v->t == '+';
    respv_free(v);
    if (!ok) {
        slog(s->cfg.name, "-failover-abort-slave-refused %s %d", n->host, n->resp_port);
        return -1;
    }
    uint64_t until = mono_ms() + s->cfg.failover_timeout_ms;
    for (;;) {
        refresh_node(s, (unsigned)pick);
        pthread_mutex_lock(&s->m);
        int promoted = s->nodes[pick].is_primary;
        pthread_mutex_unlock(&s->m);
        if (promoted) {
            break;
        }
        if (mono_ms() >= until || atomic_load(&s->stop)) {
            slog(s->cfg.name, "-failover-abort-timeout %s %d", n->host, n->resp_port);
            return -1;
        }
        usleep(TICK_MS * 1000);
    }
    pthread_mutex_lock(&s->m);
    if (epoch > s->config_epoch) {
        s->config_epoch = epoch;
        s->primary = (unsigned)pick;
        s->sdown = 0;
        s->odown = 0;
        s->forced = 0;
        s->nodes[pick].last_ok_ms = mono_ms();
        store_state(s);
    }
    pthread_mutex_unlock(&s->m);
    const sentinel_node_cfg *o = &s->cfg.nodes[old];
    slog(s->cfg.name, "+switch-master %s %d %s %d (configuration epoch %llu)", o->host, o->resp_port, n->host,
         n->resp_port, (unsigned long long)epoch);
    hello_peers(s); /* the other monitors learn the new configuration now, not at the next exchange */
    char np[16];
    snprintf(np, sizeof(np), "%d", n->native_port);
    const char *repl[3] = {"REPLICAOF", n->host, np};
    for (unsigned i = 0; i < s->cfg.nnodes; i++) {
        if (i == (unsigned)pick) {
            continue;
        }
        respv *r = node_replicaof(s, i, 3, repl);
        if (r != NULL && r->t == '+') {
            slog(s->cfg.name, "+slave-reconf-sent %s %d", s->cfg.nodes[i].host, s->cfg.nodes[i].resp_port);
        }
        respv_free(r);
    }
    return 0;
}

/*
 * Points nodes that do not follow the configured primary at it, when this monitor reaches a majority of
 * all monitors (so that its configuration is the newest a majority knows) and the primary reports the
 * primary role.
 */
static void reconcile(struct sentinel *s) {
    uint64_t now = mono_ms();
    pthread_mutex_lock(&s->m);
    unsigned reach = 1;
    for (unsigned i = 0; i < s->cfg.npeers; i++) {
        reach += s->peers[i].last_seen_ms != 0 && now - s->peers[i].last_seen_ms < PEER_FRESH_MS;
    }
    unsigned p = s->primary;
    int go = reach >= monitors(s) / 2 + 1 && s->nodes[p].reachable && s->nodes[p].is_primary && !s->sdown;
    struct node_rt nodes[SENTINEL_MAX_NODES];
    memcpy(nodes, s->nodes, sizeof(nodes));
    pthread_mutex_unlock(&s->m);
    if (!go) {
        return;
    }
    const sentinel_node_cfg *pn = &s->cfg.nodes[p];
    char np[16];
    snprintf(np, sizeof(np), "%d", pn->native_port);
    const char *repl[3] = {"REPLICAOF", pn->host, np};
    for (unsigned i = 0; i < s->cfg.nnodes; i++) {
        if (i == p || !nodes[i].reachable) {
            continue;
        }
        int follows = nodes[i].is_replica && strcmp(nodes[i].master_host, pn->host) == 0 &&
                      nodes[i].master_port == pn->native_port;
        if (follows) {
            continue;
        }
        respv *r = node_replicaof(s, i, 3, repl);
        if (r != NULL && r->t == '+') {
            slog(s->cfg.name, "%s %s %d", nodes[i].is_primary ? "+convert-to-slave" : "+fix-slave-config",
                 s->cfg.nodes[i].host, s->cfg.nodes[i].resp_port);
        }
        respv_free(r);
    }
}

static uint32_t random_below(uint32_t n) {
    uint32_t r = 0;
    if (getrandom(&r, sizeof(r), 0) != (ssize_t)sizeof(r)) {
        r = (uint32_t)mono_ms();
    }
    return n ? r % n : 0;
}

static void *monitor_main(void *arg) {
    struct sentinel *s = arg;
    uint64_t next_refresh = 0;
    static const char *const ping[1] = {"PING"};
    while (!atomic_load(&s->stop)) {
        pthread_mutex_lock(&s->m);
        unsigned p = s->primary;
        pthread_mutex_unlock(&s->m);
        respv *v = node_call(s, p, 1, ping);
        int ok = alive(v);
        respv_free(v);
        uint64_t now = mono_ms();
        int refresh = now >= next_refresh;
        if (refresh) {
            next_refresh = now + REFRESH_MS;
            for (unsigned i = 0; i < s->cfg.nnodes; i++) {
                refresh_node(s, i);
            }
            hello_peers(s);
        }
        pthread_mutex_lock(&s->m);
        if (p == s->primary) {
            if (ok) {
                s->nodes[p].last_ok_ms = now;
            }
            int was = s->sdown;
            uint64_t last = s->nodes[p].last_ok_ms;
            s->sdown =
                last == 0 ? now - s->sdown_since > s->cfg.down_after_ms : now - last > s->cfg.down_after_ms;
            if (s->sdown && !was) {
                slog(s->cfg.name, "+sdown master %s %d", s->cfg.nodes[p].host, s->cfg.nodes[p].resp_port);
            } else if (!s->sdown && was) {
                slog(s->cfg.name, "-sdown master %s %d", s->cfg.nodes[p].host, s->cfg.nodes[p].resp_port);
                if (s->odown) {
                    slog(s->cfg.name, "-odown master %s %d", s->cfg.nodes[p].host, s->cfg.nodes[p].resp_port);
                }
                s->odown = 0;
            }
        }
        int sdown = s->sdown;
        int forced = s->forced;
        uint64_t cur = s->vote.current_epoch;
        pthread_mutex_unlock(&s->m);
        if (sdown && refresh) {
            unsigned votes;
            unsigned down = ask_peers(s, cur, "*", &votes);
            pthread_mutex_lock(&s->m);
            int was = s->odown;
            s->odown = down >= quorum(s);
            if (s->odown && !was) {
                slog(s->cfg.name, "+odown master %s %d #quorum %u/%u", s->cfg.nodes[p].host,
                     s->cfg.nodes[p].resp_port, down, quorum(s));
                if (s->next_attempt_ms < now) {
                    s->next_attempt_ms = now + random_below(1000); /* spreads the monitors' first attempts */
                }
            }
            pthread_mutex_unlock(&s->m);
        }
        pthread_mutex_lock(&s->m);
        int start = (s->odown || forced) && now >= s->next_attempt_ms && s->primary == p;
        uint64_t epoch = 0;
        if (start) {
            s->vote.current_epoch++;
            epoch = s->vote.current_epoch;
            s->vote.voted_epoch = epoch;
            snprintf(s->vote.voted_for, sizeof(s->vote.voted_for), "%s", s->id);
            s->next_attempt_ms = now + s->cfg.failover_timeout_ms + random_below(1000);
            store_state(s);
        }
        pthread_mutex_unlock(&s->m);
        if (start) {
            slog(s->cfg.name, "+try-failover master %s %d (epoch %llu)", s->cfg.nodes[p].host,
                 s->cfg.nodes[p].resp_port, (unsigned long long)epoch);
            unsigned votes = 0;
            ask_peers(s, epoch, s->id, &votes);
            unsigned need = sentinel_votes_needed(monitors(s), quorum(s));
            if (votes + 1 >= need) {
                slog(s->cfg.name, "+elected-leader (epoch %llu, %u of %u votes)", (unsigned long long)epoch,
                     votes + 1, monitors(s));
                failover(s, epoch);
            } else {
                slog(s->cfg.name, "-failover-abort-not-elected (epoch %llu, %u of %u votes needed)",
                     (unsigned long long)epoch, votes + 1, need);
            }
        } else if (refresh && !sdown) {
            reconcile(s);
        }
        uint64_t spent = mono_ms() - now;
        if (spent < TICK_MS) {
            usleep((useconds_t)(TICK_MS - spent) * 1000);
        }
    }
    return NULL;
}

/* ---- lifecycle ------------------------------------------------------------------------------------------- */

void sentinel_cfg_default(sentinel_cfg *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->name, sizeof(cfg->name), "arenakv");
    snprintf(cfg->listen_port, sizeof(cfg->listen_port), "26379");
    cfg->down_after_ms = 5000;
    cfg->failover_timeout_ms = 60000;
}

sentinel *sentinel_start(const sentinel_cfg *cfg, char *err, size_t errlen) {
    if (cfg->nnodes < 2 || cfg->nnodes > SENTINEL_MAX_NODES || cfg->npeers > SENTINEL_MAX_PEERS) {
        snprintf(err, errlen, "a group needs 2 to %d nodes and at most %d other monitors", SENTINEL_MAX_NODES,
                 SENTINEL_MAX_PEERS);
        return NULL;
    }
    if (cfg->quorum > cfg->npeers + 1) {
        snprintf(err, errlen, "quorum %u is larger than the %u monitors", cfg->quorum, cfg->npeers + 1);
        return NULL;
    }
    struct sentinel *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->cfg = *cfg;
    s->lfd = -1;
    pthread_mutex_init(&s->m, NULL);
    if (load_state(s) != 0) {
        snprintf(err, errlen, "state file %s is unreadable or names a node that is not configured",
                 cfg->state_path);
        sentinel_stop(s);
        return NULL;
    }
    if (s->id[0] == '\0') {
        uint8_t raw[SENTINEL_ID_LEN / 2];
        if (getrandom(raw, sizeof(raw), 0) != (ssize_t)sizeof(raw)) {
            snprintf(err, errlen, "getrandom: %s", strerror(errno));
            sentinel_stop(s);
            return NULL;
        }
        for (size_t i = 0; i < sizeof(raw); i++) {
            snprintf(s->id + 2 * i, 3, "%02x", raw[i]);
        }
    }
    if (store_state(s) != 0) {
        snprintf(err, errlen, "cannot write the state file %s", cfg->state_path);
        sentinel_stop(s);
        return NULL;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
    struct addrinfo *ai = NULL;
    int rc = getaddrinfo(cfg->listen_addr[0] ? cfg->listen_addr : NULL, cfg->listen_port, &hints, &ai);
    if (rc != 0) {
        snprintf(err, errlen, "listen address: %s", gai_strerror(rc));
        sentinel_stop(s);
        return NULL;
    }
    for (struct addrinfo *p = ai; p != NULL && s->lfd < 0; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        int one = 1;
        if (fd >= 0 && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
            bind(fd, p->ai_addr, p->ai_addrlen) == 0 && listen(fd, 64) == 0) {
            s->lfd = fd;
        } else if (fd >= 0) {
            close(fd);
        }
    }
    freeaddrinfo(ai);
    if (s->lfd < 0) {
        snprintf(err, errlen, "listen on port %s: %s", cfg->listen_port, strerror(errno));
        sentinel_stop(s);
        return NULL;
    }
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t sl = sizeof(ss);
    if (getsockname(s->lfd, (struct sockaddr *)&ss, &sl) == 0) {
        s->port = ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&ss)->sin6_port
                                                 : ((struct sockaddr_in *)&ss)->sin_port);
    }
    s->sdown_since = mono_ms();
    if (pthread_create(&s->listener, NULL, listener_main, s) != 0) {
        snprintf(err, errlen, "thread: %s", strerror(errno));
        sentinel_stop(s);
        return NULL;
    }
    if (pthread_create(&s->monitor, NULL, monitor_main, s) != 0) {
        atomic_store(&s->stop, 1);
        pthread_join(s->listener, NULL);
        s->listener = 0;
        snprintf(err, errlen, "thread: %s", strerror(errno));
        sentinel_stop(s);
        return NULL;
    }
    const sentinel_node_cfg *p = &cfg->nodes[s->primary];
    slog(cfg->name,
         "monitoring primary %s %d (configuration epoch %llu), %u nodes, %u other monitors, quorum %u, "
         "id %s, port %d",
         p->host, p->resp_port, (unsigned long long)s->config_epoch, cfg->nnodes, cfg->npeers, quorum(s),
         s->id, s->port);
    return s;
}

int sentinel_port(const sentinel *s) {
    return s->port;
}

void sentinel_stop(sentinel *s) {
    if (s == NULL) {
        return;
    }
    atomic_store(&s->stop, 1);
    if (s->monitor) {
        pthread_join(s->monitor, NULL);
    }
    if (s->listener) {
        pthread_join(s->listener, NULL);
    }
    if (s->lfd >= 0) {
        close(s->lfd);
    }
    pthread_mutex_destroy(&s->m);
    free(s);
}
