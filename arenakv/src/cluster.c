/*
 * cluster.c - slot computation and configuration file of the cluster protocol, see cluster.h.
 */
#include "cluster.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t cluster_crc16(const void *data, size_t n) {
    const uint8_t *p = data;
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

unsigned cluster_keyslot(const void *key, size_t n) {
    const char *k = key;
    const char *open = memchr(k, '{', n);
    if (open != NULL) {
        size_t rest = n - (size_t)(open - k) - 1;
        const char *close = memchr(open + 1, '}', rest);
        if (close != NULL && close > open + 1) {
            return cluster_crc16(open + 1, (size_t)(close - open - 1)) & (CLUSTER_SLOTS - 1);
        }
    }
    return cluster_crc16(k, n) & (CLUSTER_SLOTS - 1);
}

static void set_err(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int valid_id(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 40) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-') {
            return 0;
        }
    }
    return 1;
}

static int parse_port(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    return *end == '\0' && v > 0 && v <= 65535 ? (int)v : -1;
}

void cluster_free(cluster *c) {
    if (c != NULL) {
        free(c->nodes);
        free(c);
    }
}

/* Replica lines name their primary by id; resolved after all lines are read. */
struct pending {
    unsigned node;
    char primary[41];
    int line;
};

cluster *cluster_load(const char *path, const char *self_id, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        set_err(err, errlen, "%s: %s", path, strerror(errno));
        return NULL;
    }
    cluster *c = calloc(1, sizeof(*c));
    struct pending *pend = NULL;
    size_t npend = 0;
    if (c == NULL) {
        fclose(f);
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    for (unsigned s = 0; s < CLUSTER_SLOTS; s++) {
        c->owner[s] = -1;
    }
    c->self = -1;
    char line[8192];
    int lineno = 0;
    int ok = 1;
    while (ok && fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        char *tok[1100];
        int nt = 0;
        for (char *save = NULL, *t = strtok_r(line, " \t\r\n", &save); t != NULL && nt < 1100;
             t = strtok_r(NULL, " \t\r\n", &save)) {
            tok[nt++] = t;
        }
        if (nt == 0) {
            continue;
        }
        if (strcmp(tok[0], "node") != 0 || nt < 7) {
            set_err(
                err, errlen,
                "%s: line %d: expected 'node <id> <host> <resp-port> <native-port> primary|replica-of ...'",
                path, lineno);
            ok = 0;
            break;
        }
        if (!valid_id(tok[1]) || strlen(tok[2]) >= sizeof(c->nodes[0].host)) {
            set_err(err, errlen, "%s: line %d: invalid node id or host", path, lineno);
            ok = 0;
            break;
        }
        for (unsigned i = 0; i < c->n; i++) {
            if (strcmp(c->nodes[i].id, tok[1]) == 0) {
                set_err(err, errlen, "%s: line %d: node %s is defined twice", path, lineno, tok[1]);
                ok = 0;
            }
        }
        int rp = parse_port(tok[3]);
        int np = parse_port(tok[4]);
        if (ok && (rp < 0 || np < 0)) {
            set_err(err, errlen, "%s: line %d: invalid port", path, lineno);
            ok = 0;
        }
        if (!ok) {
            break;
        }
        struct cluster_node *nn = realloc(c->nodes, (c->n + 1) * sizeof(*nn));
        if (nn == NULL) {
            set_err(err, errlen, "out of memory");
            ok = 0;
            break;
        }
        c->nodes = nn;
        struct cluster_node *node = &c->nodes[c->n];
        memset(node, 0, sizeof(*node));
        snprintf(node->id, sizeof(node->id), "%s", tok[1]);
        snprintf(node->host, sizeof(node->host), "%s", tok[2]);
        node->resp_port = rp;
        node->native_port = np;
        node->primary_of = -1;
        if (strcmp(tok[5], "primary") == 0) {
            node->primary = 1;
            for (int i = 6; i < nt && ok; i++) {
                char *dash = strchr(tok[i], '-');
                char *end;
                long lo = strtol(tok[i], &end, 10);
                long hi = lo;
                if (dash != NULL && end == dash) {
                    hi = strtol(dash + 1, &end, 10);
                }
                if (*end != '\0' || lo < 0 || hi < lo || hi >= (long)CLUSTER_SLOTS) {
                    set_err(err, errlen, "%s: line %d: invalid slot range '%s'", path, lineno, tok[i]);
                    ok = 0;
                    break;
                }
                for (long s = lo; s <= hi; s++) {
                    if (c->owner[s] >= 0) {
                        set_err(err, errlen, "%s: line %d: slot %ld is already served by node %s", path,
                                lineno, s, c->nodes[c->owner[s]].id);
                        ok = 0;
                        break;
                    }
                    c->owner[s] = (int16_t)c->n;
                }
            }
        } else if (strcmp(tok[5], "replica-of") == 0 && nt == 7) {
            struct pending *np2 = realloc(pend, (npend + 1) * sizeof(*np2));
            if (np2 == NULL) {
                set_err(err, errlen, "out of memory");
                ok = 0;
                break;
            }
            pend = np2;
            pend[npend] = (struct pending){.node = c->n, .line = lineno};
            snprintf(pend[npend].primary, sizeof(pend[npend].primary), "%s", tok[6]);
            npend++;
        } else {
            set_err(err, errlen, "%s: line %d: expected 'primary <slots>' or 'replica-of <id>'", path,
                    lineno);
            ok = 0;
            break;
        }
        if (self_id != NULL && strcmp(node->id, self_id) == 0) {
            c->self = (int)c->n;
        }
        c->n++;
    }
    fclose(f);
    for (size_t i = 0; ok && i < npend; i++) {
        int found = -1;
        for (unsigned k = 0; k < c->n; k++) {
            if (strcmp(c->nodes[k].id, pend[i].primary) == 0 && c->nodes[k].primary) {
                found = (int)k;
            }
        }
        if (found < 0) {
            set_err(err, errlen, "%s: line %d: no primary with id %s", path, pend[i].line, pend[i].primary);
            ok = 0;
        } else {
            c->nodes[pend[i].node].primary_of = found;
        }
    }
    free(pend);
    if (ok && c->n == 0) {
        set_err(err, errlen, "%s: no nodes", path);
        ok = 0;
    }
    if (ok && self_id != NULL && c->self < 0) {
        set_err(err, errlen, "%s: this node's id %s is not in the file", path, self_id);
        ok = 0;
    }
    if (!ok) {
        cluster_free(c);
        return NULL;
    }
    return c;
}

int cluster_shard_of_self(const cluster *c) {
    if (c->self < 0) {
        return -1;
    }
    return c->nodes[c->self].primary ? c->self : c->nodes[c->self].primary_of;
}

unsigned cluster_assigned(const cluster *c) {
    unsigned n = 0;
    for (unsigned s = 0; s < CLUSTER_SLOTS; s++) {
        n += c->owner[s] >= 0;
    }
    return n;
}
