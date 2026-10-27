/* trace.c - KV-cache request traces, see trace.h. */
#include "trace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Appends v to the growing array *a (count *n, capacity *cap). Returns 0 or -1. */
static int push64(uint64_t **a, uint64_t *n, uint64_t *cap, uint64_t v) {
    if (*n == *cap) {
        uint64_t c = *cap ? *cap * 2 : 1024;
        uint64_t *p = realloc(*a, c * sizeof(*p));
        if (p == NULL) {
            return -1;
        }
        *a = p;
        *cap = c;
    }
    (*a)[(*n)++] = v;
    return 0;
}

/* Position after the value separator of field `name` ("name" followed by ':'), or NULL. */
static const char *field(const char *line, const char *name) {
    size_t n = strlen(name);
    for (const char *p = strstr(line, name); p != NULL; p = strstr(p + 1, name)) {
        if (p > line && p[-1] == '"' && p[n] == '"') {
            const char *q = p + n + 1;
            while (*q == ' ' || *q == '\t') {
                q++;
            }
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t') {
                    q++;
                }
                return q;
            }
        }
    }
    return NULL;
}

/* Parses a non-negative decimal integer at *p and advances *p. Returns 0 or -1. */
static int parse_u64(const char **p, uint64_t *v) {
    const char *s = *p;
    if (*s < '0' || *s > '9') {
        return -1;
    }
    uint64_t x = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        uint64_t d = (uint64_t)(*s - '0');
        if (x > (UINT64_MAX - d) / 10) {
            return -1;
        }
        x = x * 10 + d;
    }
    *p = s;
    *v = x;
    return 0;
}

int pace_trace_load(const char *path, pace_trace *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    uint64_t cap_req = 0;
    uint64_t cap_first = 0;
    uint64_t cap_ids = 0;
    uint64_t nfirst = 0;
    uint32_t *nblocks = NULL;
    uint64_t cap_nb = 0;
    char *line = NULL;
    size_t lcap = 0;
    unsigned long lineno = 0;
    int rc = 0;
    while (getline(&line, &lcap, f) >= 0) {
        lineno++;
        const char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            s++;
        }
        if (*s == '\0') {
            continue;
        }
        const char *ts = field(line, "timestamp");
        const char *ids = field(line, "hash_ids");
        uint64_t t_ms;
        if (ts == NULL || parse_u64(&ts, &t_ms) != 0 || t_ms > UINT64_MAX / 1000) {
            snprintf(err, errlen, "%s:%lu: missing or invalid \"timestamp\"", path, lineno);
            rc = -1;
            break;
        }
        if (out->n > 0 && t_ms * 1000 < out->t_us[out->n - 1]) {
            snprintf(err, errlen, "%s:%lu: timestamp decreases", path, lineno);
            rc = -1;
            break;
        }
        if (ids == NULL || *ids != '[') {
            snprintf(err, errlen, "%s:%lu: missing \"hash_ids\" array", path, lineno);
            rc = -1;
            break;
        }
        ids++;
        uint64_t start = out->nids;
        for (;;) {
            while (*ids == ' ' || *ids == '\t') {
                ids++;
            }
            if (*ids == ']') {
                break;
            }
            uint64_t v;
            if (parse_u64(&ids, &v) != 0) {
                rc = -1;
                break;
            }
            if (push64(&out->ids, &out->nids, &cap_ids, v) != 0) {
                rc = -2;
                break;
            }
            while (*ids == ' ' || *ids == '\t') {
                ids++;
            }
            if (*ids == ',') {
                ids++;
            } else if (*ids != ']') {
                rc = -1;
                break;
            }
        }
        if (rc == 0 && out->nids == start) {
            rc = -1; /* a request without blocks */
        }
        if (rc == 0 && out->nids - start > UINT32_MAX) {
            rc = -1;
        }
        if (rc != 0) {
            snprintf(err, errlen, rc == -2 ? "%s:%lu: out of memory" : "%s:%lu: invalid \"hash_ids\"", path,
                     lineno);
            rc = -1;
            break;
        }
        uint64_t nreq = out->n;
        if (push64(&out->t_us, &out->n, &cap_req, t_ms * 1000) != 0 ||
            push64(&out->first, &nfirst, &cap_first, start) != 0) {
            snprintf(err, errlen, "%s:%lu: out of memory", path, lineno);
            rc = -1;
            break;
        }
        if (nblocks == NULL || nreq >= cap_nb) {
            uint64_t c = cap_nb ? cap_nb * 2 : 1024;
            uint32_t *p = realloc(nblocks, c * sizeof(*p));
            if (p == NULL) {
                snprintf(err, errlen, "%s:%lu: out of memory", path, lineno);
                rc = -1;
                break;
            }
            nblocks = p;
            cap_nb = c;
        }
        nblocks[nreq] = (uint32_t)(out->nids - start);
    }
    free(line);
    fclose(f);
    out->nblocks = nblocks;
    if (rc == 0 && out->n == 0) {
        snprintf(err, errlen, "%s: no requests", path);
        rc = -1;
    }
    if (rc != 0) {
        pace_trace_free(out);
    }
    return rc;
}

void pace_trace_free(pace_trace *t) {
    free(t->t_us);
    free(t->first);
    free(t->nblocks);
    free(t->ids);
    memset(t, 0, sizeof(*t));
}
