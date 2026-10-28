/*
 * akv_reshard.c - moves hash slots, with their keys, from one arenakvd primary to another while clients
 * keep using the cluster (the procedure of the Redis Cluster specification, "Live reconfiguration"):
 *
 *   akv_reshard --from HOST:PORT --to HOST:PORT --slots 0-99,200 [--batch 100] [--timeout 5000]
 *               [--auth [USER:]PASSWORD]
 *
 * HOST:PORT are RESP addresses of the source and the target primary. Steps:
 *   1. every slot is marked IMPORTING on the target and MIGRATING on the source (CLUSTER SETSLOT);
 *   2. the keys of each slot are moved in batches (CLUSTER GETKEYSINSLOT, MIGRATE ... KEYS);
 *   3. every slot is assigned to the target on the target, the source and every other node listed by
 *      CLUSTER NODES (CLUSTER SETSLOT ... NODE), and every node writes its map file (CLUSTER SAVECONFIG).
 * While a slot moves, clients are redirected with ASK for keys already moved. Each step can be repeated, so
 * an interrupted run is completed by running the same command again. Exit status 0 on success, 1 on a
 * failure (the message names the step), 2 on invalid arguments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cluster.h"
#include "respc.h"

#define MAX_NODES 1024
#define CHUNK 1024 /* commands sent before their replies are read */

struct node {
    char host[256];
    int port;
};

static unsigned timeout_ms = 5000;
static unsigned batch = 100;
static char auth_user[256];
static char auth_pass[256];

static int usage(void) {
    fprintf(stderr,
            "usage: akv_reshard --from HOST:PORT --to HOST:PORT --slots RANGES [--batch N] [--timeout MS]\n"
            "                   [--auth [USER:]PASSWORD]\n"
            "  --slots    slots to move: numbers and ranges separated by commas, for example 0-99,200\n"
            "  --batch    keys per MIGRATE (default: 100)\n"
            "  --timeout  milliseconds for each request and each MIGRATE (default: 5000)\n"
            "  --auth     credentials for every node (the user needs the admin and dangerous categories)\n");
    return 2;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int parse_hostport(const char *s, struct node *n) {
    const char *c = strrchr(s, ':');
    char *end;
    if (c == NULL || c == s || (size_t)(c - s) >= sizeof(n->host)) {
        return -1;
    }
    long p = strtol(c + 1, &end, 10);
    if (*end != '\0' || p <= 0 || p > 65535) {
        return -1;
    }
    memcpy(n->host, s, (size_t)(c - s));
    n->host[c - s] = '\0';
    n->port = (int)p;
    return 0;
}

/* Parses "0-99,200" into the slot flags. Returns the number of slots, or -1. */
static long parse_slots(const char *spec, unsigned char *want) {
    long count = 0;
    const char *p = spec;
    while (*p != '\0') {
        char *end;
        long lo = strtol(p, &end, 10);
        long hi = lo;
        if (end == p) {
            return -1;
        }
        if (*end == '-') {
            const char *q = end + 1;
            hi = strtol(q, &end, 10);
            if (end == q) {
                return -1;
            }
        }
        if (lo < 0 || hi < lo || hi >= (long)CLUSTER_SLOTS || (*end != ',' && *end != '\0')) {
            return -1;
        }
        for (long s = lo; s <= hi; s++) {
            count += !want[s];
            want[s] = 1;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return count;
}

/* Connects to a node and authenticates. Returns NULL with a message on standard error. */
static respc *connect_node(const struct node *n) {
    respc *c = respc_open(n->host, n->port, timeout_ms);
    if (c == NULL) {
        fprintf(stderr, "akv_reshard: cannot connect to %s:%d\n", n->host, n->port);
        return NULL;
    }
    if (auth_pass[0] != '\0' && respc_auth(c, auth_user, auth_pass) != 0) {
        fprintf(stderr, "akv_reshard: authentication failed at %s:%d\n", n->host, n->port);
        respc_close(c);
        return NULL;
    }
    return c;
}

/* One command on an open connection; returns the reply or NULL. */
static respv *call(respc *c, int argc, const char *const *argv) {
    respc_deadline(c, timeout_ms);
    if (respc_send(c, argc, argv, NULL) != 0) {
        return NULL;
    }
    return respc_read(c);
}

/* CLUSTER MYID of a node into id. Returns 0 or -1. */
static int node_id(respc *c, const struct node *n, char *id, size_t cap) {
    static const char *const argv[2] = {"CLUSTER", "MYID"};
    respv *v = call(c, 2, argv);
    int ok = v != NULL && v->t == '$' && v->s != NULL && v->n > 0 && v->n < cap;
    if (ok) {
        snprintf(id, cap, "%s", v->s);
    } else {
        fprintf(stderr, "akv_reshard: %s:%d did not report its node id (is it in cluster mode?)\n", n->host,
                n->port);
    }
    respv_free(v);
    return ok ? 0 : -1;
}

/*
 * Sends "CLUSTER SETSLOT <slot> <action> [id]" for every wanted slot, in chunks, and checks the replies.
 * Returns 0, or -1 after printing the first error.
 */
static int setslot_all(respc *c, const struct node *n, const unsigned char *want, const char *action,
                       const char *id) {
    unsigned slots[CHUNK];
    for (unsigned s = 0; s < CLUSTER_SLOTS;) {
        unsigned k = 0;
        respc_deadline(c, timeout_ms);
        for (; s < CLUSTER_SLOTS && k < CHUNK; s++) {
            if (!want[s]) {
                continue;
            }
            char num[16];
            snprintf(num, sizeof(num), "%u", s);
            const char *argv[5] = {"CLUSTER", "SETSLOT", num, action, id};
            if (respc_send(c, id != NULL ? 5 : 4, argv, NULL) != 0) {
                fprintf(stderr, "akv_reshard: %s:%d: connection lost\n", n->host, n->port);
                return -1;
            }
            slots[k++] = s;
        }
        int failed = 0;
        for (unsigned i = 0; i < k; i++) {
            respv *v = respc_read(c);
            if (!failed && (v == NULL || v->t != '+')) {
                fprintf(stderr, "akv_reshard: %s:%d: CLUSTER SETSLOT %u %s: %s\n", n->host, n->port, slots[i],
                        action,
                        v == NULL      ? "no reply"
                        : v->s != NULL ? v->s
                                       : "unexpected reply");
                failed = 1;
            }
            respv_free(v);
        }
        if (failed) {
            return -1;
        }
    }
    return 0;
}

/* Reads every node of CLUSTER NODES (RESP addresses). Returns their number, or -1. */
static int list_nodes(respc *c, struct node *nodes, int max) {
    static const char *const argv[2] = {"CLUSTER", "NODES"};
    respv *v = call(c, 2, argv);
    if (v == NULL || v->t != '$' || v->s == NULL) {
        respv_free(v);
        return -1;
    }
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(v->s, "\n", &save); line != NULL && n < max;
         line = strtok_r(NULL, "\n", &save)) {
        char id[64];
        char addr[300];
        if (sscanf(line, "%63s %299s", id, addr) != 2) {
            continue;
        }
        char *at = strchr(addr, '@');
        if (at != NULL) {
            *at = '\0';
        }
        if (parse_hostport(addr, &nodes[n]) == 0) {
            n++;
        }
    }
    respv_free(v);
    return n;
}

int main(int argc, char **argv) {
    struct node from = {0};
    struct node to = {0};
    static unsigned char want[CLUSTER_SLOTS];
    long nslots = -1;
    for (int i = 1; i < argc; i += 2) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        if (v == NULL) {
            return usage();
        }
        if (strcmp(a, "--from") == 0) {
            if (parse_hostport(v, &from) != 0) {
                fprintf(stderr, "invalid --from %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--to") == 0) {
            if (parse_hostport(v, &to) != 0) {
                fprintf(stderr, "invalid --to %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--slots") == 0) {
            nslots = parse_slots(v, want);
            if (nslots <= 0) {
                fprintf(stderr, "invalid --slots %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--batch") == 0) {
            batch = (unsigned)strtoul(v, NULL, 10);
            if (batch == 0 || batch > 4096) {
                fprintf(stderr, "invalid --batch %s (1 to 4096)\n", v);
                return 2;
            }
        } else if (strcmp(a, "--timeout") == 0) {
            timeout_ms = (unsigned)strtoul(v, NULL, 10);
            if (timeout_ms < 100) {
                fprintf(stderr, "invalid --timeout %s (100 ms or more)\n", v);
                return 2;
            }
        } else if (strcmp(a, "--auth") == 0) {
            const char *c = strchr(v, ':');
            if (c != NULL) {
                snprintf(auth_user, sizeof(auth_user), "%.*s", (int)(c - v), v);
                snprintf(auth_pass, sizeof(auth_pass), "%s", c + 1);
            } else {
                snprintf(auth_pass, sizeof(auth_pass), "%s", v);
            }
        } else {
            return usage();
        }
    }
    if (from.port == 0 || to.port == 0 || nslots <= 0) {
        return usage();
    }
    double t0 = now_s();
    respc *src = connect_node(&from);
    respc *dst = src != NULL ? connect_node(&to) : NULL;
    char src_id[64];
    char dst_id[64];
    if (dst == NULL || node_id(src, &from, src_id, sizeof(src_id)) != 0 ||
        node_id(dst, &to, dst_id, sizeof(dst_id)) != 0) {
        respc_close(src);
        respc_close(dst);
        return 1;
    }
    if (strcmp(src_id, dst_id) == 0) {
        fprintf(stderr, "akv_reshard: source and target are the same node\n");
        return 1;
    }
    static struct node nodes[MAX_NODES];
    int nnodes = list_nodes(src, nodes, MAX_NODES);
    if (nnodes < 2) {
        fprintf(stderr, "akv_reshard: CLUSTER NODES of the source lists no cluster\n");
        return 1;
    }
    /* 1. mark the slots on both nodes */
    if (setslot_all(dst, &to, want, "IMPORTING", src_id) != 0 ||
        setslot_all(src, &from, want, "MIGRATING", dst_id) != 0) {
        fprintf(stderr,
                "akv_reshard: marking the slots failed; run the same command again after fixing the cause\n");
        return 1;
    }
    printf("marked %ld slots: %s IMPORTING from %s, %s MIGRATING to %s\n", nslots, dst_id, src_id, src_id,
           dst_id);
    /* 2. move the keys */
    char port[16];
    char tmo[16];
    char cnt[16];
    snprintf(port, sizeof(port), "%d", to.port);
    snprintf(tmo, sizeof(tmo), "%u", timeout_ms);
    snprintf(cnt, sizeof(cnt), "%u", batch);
    unsigned long long moved = 0;
    for (unsigned s = 0; s < CLUSTER_SLOTS; s++) {
        if (!want[s]) {
            continue;
        }
        char num[16];
        snprintf(num, sizeof(num), "%u", s);
        for (;;) {
            const char *gk[4] = {"CLUSTER", "GETKEYSINSLOT", num, cnt};
            respv *keys = call(src, 4, gk);
            if (keys == NULL || keys->t != '*') {
                fprintf(stderr, "akv_reshard: CLUSTER GETKEYSINSLOT %u at the source: %s\n", s,
                        keys != NULL && keys->s != NULL ? keys->s : "no reply");
                respv_free(keys);
                return 1;
            }
            if (keys->na == 0) {
                respv_free(keys);
                break;
            }
            /* MIGRATE host port "" 0 timeout REPLACE [AUTH2 user pass | AUTH pass] KEYS k... */
            size_t max = 12 + keys->na;
            const char **mv = calloc(max, sizeof(*mv));
            size_t *ml = calloc(max, sizeof(*ml));
            if (mv == NULL || ml == NULL) {
                fprintf(stderr, "akv_reshard: out of memory\n");
                free(mv);
                free(ml);
                respv_free(keys);
                return 1;
            }
            int k = 0;
            mv[k++] = "MIGRATE";
            mv[k++] = to.host;
            mv[k++] = port;
            mv[k++] = "";
            mv[k++] = "0";
            mv[k++] = tmo;
            mv[k++] = "REPLACE";
            if (auth_pass[0] != '\0' && auth_user[0] != '\0') {
                mv[k++] = "AUTH2";
                mv[k++] = auth_user;
                mv[k++] = auth_pass;
            } else if (auth_pass[0] != '\0') {
                mv[k++] = "AUTH";
                mv[k++] = auth_pass;
            }
            mv[k++] = "KEYS";
            for (int i = 0; i < k; i++) {
                ml[i] = strlen(mv[i]);
            }
            for (size_t i = 0; i < keys->na; i++) {
                mv[k] = keys->a[i]->s;
                ml[k++] = keys->a[i]->n;
            }
            respc_deadline(src, timeout_ms * 2);
            respv *r = respc_send(src, k, mv, ml) == 0 ? respc_read(src) : NULL;
            free(mv);
            free(ml);
            int ok = r != NULL && r->t == '+';
            if (!ok) {
                fprintf(stderr, "akv_reshard: MIGRATE of slot %u: %s\n", s,
                        r != NULL && r->s != NULL ? r->s : "no reply");
            }
            moved += keys->na;
            respv_free(r);
            respv_free(keys);
            if (!ok) {
                return 1;
            }
        }
    }
    printf("moved %llu keys\n", moved);
    /* 3. assign the slots: target, source, then every other node */
    if (setslot_all(dst, &to, want, "NODE", dst_id) != 0 ||
        setslot_all(src, &from, want, "NODE", dst_id) != 0) {
        fprintf(stderr, "akv_reshard: assigning the slots failed; run the same command again\n");
        return 1;
    }
    int rc = 0;
    for (int i = 0; i < nnodes; i++) {
        respc *c = connect_node(&nodes[i]);
        if (c == NULL) {
            rc = 1;
            continue;
        }
        char id[64];
        int skip =
            node_id(c, &nodes[i], id, sizeof(id)) != 0 || strcmp(id, src_id) == 0 || strcmp(id, dst_id) == 0;
        if (!skip && setslot_all(c, &nodes[i], want, "NODE", dst_id) != 0) {
            rc = 1;
        }
        static const char *const save[2] = {"CLUSTER", "SAVECONFIG"};
        respv *v = call(c, 2, save);
        if (v == NULL || v->t != '+') {
            fprintf(stderr, "akv_reshard: CLUSTER SAVECONFIG at %s:%d: %s\n", nodes[i].host, nodes[i].port,
                    v != NULL && v->s != NULL ? v->s : "no reply");
            rc = 1;
        }
        respv_free(v);
        respc_close(c);
    }
    respc_close(src);
    respc_close(dst);
    printf("assigned %ld slots to %s on %d nodes in %.2f s%s\n", nslots, dst_id, nnodes, now_s() - t0,
           rc != 0 ? " (some nodes failed, see above; run the same command again)" : "");
    return rc;
}
