/* workload.c - operation generator, see workload.h. */
#include "workload.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const op_names[PACE_OP_COUNT] = {"get", "set", "del", "incr", "mget", "rmw", "insert"};

const char *pace_op_name(pace_op_type t) {
    return t < PACE_OP_COUNT ? op_names[t] : "unknown";
}

void pace_wl_default(pace_workload *w) {
    memset(w, 0, sizeof(*w));
    w->weight[PACE_OP_GET] = 0.9;
    w->weight[PACE_OP_SET] = 0.1;
    w->dist = PACE_DIST_UNIFORM;
    w->theta = 0.99;
    w->nkeys = 100000;
    w->vmode = PACE_VSIZE_FIXED;
    w->vmin = w->vmax = 64;
    w->mget_keys = 1;
    strcpy(w->prefix, "k");
    w->prefix_len = 1;
}

int pace_wl_init(pace_workload *w, char *err, size_t errlen) {
    double sum = 0.0;
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        if (w->weight[i] < 0.0) {
            snprintf(err, errlen, "negative operation weight");
            return -1;
        }
        sum += w->weight[i];
    }
    if (sum <= 0.0) {
        snprintf(err, errlen, "the operation mix is empty");
        return -1;
    }
    double c = 0.0;
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        c += w->weight[i] / sum;
        w->cum[i] = c;
    }
    w->cum[PACE_OP_COUNT - 1] = 1.0;
    if (w->nkeys == 0 || w->nkeys >= 1000000000000000ull) {
        snprintf(err, errlen, "key count out of range");
        return -1;
    }
    if (w->mget_keys == 0 || w->mget_keys > PACE_MGET_MAX) {
        snprintf(err, errlen, "keys per MGET must be in [1, %u]", PACE_MGET_MAX);
        return -1;
    }
    if (w->vmode == PACE_VSIZE_FIXED && w->vmin == 0) {
        snprintf(err, errlen, "value size must be positive");
        return -1;
    }
    w->has_zipf = 0;
    if (w->dist == PACE_DIST_ZIPF || w->dist == PACE_DIST_LATEST) {
        if (pace_zipf_init(&w->zipf, w->nkeys, w->theta) != 0) {
            snprintf(err, errlen, "invalid zipf parameters (keys %llu, theta %g)",
                     (unsigned long long)w->nkeys, w->theta);
            return -1;
        }
        w->has_zipf = 1;
    }
    if (w->gauss_sd <= 0.0) {
        w->gauss_sd = 1.0 / 6.0;
    }
    atomic_store(&w->inserted, w->nkeys);
    return 0;
}

static int set_weight(pace_workload *w, const char *name, double v) {
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        if (strcmp(name, op_names[i]) == 0) {
            w->weight[i] = v;
            return 0;
        }
    }
    return -1;
}

int pace_wl_parse_ratio(pace_workload *w, const char *s) {
    char *end;
    double set = strtod(s, &end);
    if (end == s || *end != ':') {
        return -1;
    }
    const char *g = end + 1;
    double get = strtod(g, &end);
    if (end == g || *end != '\0' || set < 0 || get < 0 || set + get <= 0) {
        return -1;
    }
    memset(w->weight, 0, sizeof(w->weight));
    w->weight[PACE_OP_SET] = set;
    w->weight[PACE_OP_GET] = get;
    return 0;
}

int pace_wl_parse_mix(pace_workload *w, const char *s) {
    double weights[PACE_OP_COUNT] = {0};
    char buf[256];
    if (strlen(s) >= sizeof(buf)) {
        return -1;
    }
    strcpy(buf, s);
    pace_workload tmp = *w;
    memset(tmp.weight, 0, sizeof(tmp.weight));
    for (char *tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            return -1;
        }
        *eq = '\0';
        char *end;
        double v = strtod(eq + 1, &end);
        if (end == eq + 1 || *end != '\0' || v < 0 || set_weight(&tmp, tok, v) != 0) {
            return -1;
        }
    }
    (void)weights;
    memcpy(w->weight, tmp.weight, sizeof(w->weight));
    return 0;
}

int pace_wl_parse_vsize(pace_workload *w, const char *s) {
    char *end;
    if (strchr(s, ':') != NULL) {
        char buf[256];
        if (strlen(s) >= sizeof(buf)) {
            return -1;
        }
        strcpy(buf, s);
        unsigned n = 0;
        double total = 0.0;
        double weights[PACE_VSIZE_LIST_MAX];
        for (char *tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
            char *colon = strchr(tok, ':');
            if (colon == NULL || n == PACE_VSIZE_LIST_MAX) {
                return -1;
            }
            *colon = '\0';
            unsigned long size = strtoul(tok, &end, 10);
            if (end == tok || *end != '\0' || size == 0 || size > (1u << 30)) {
                return -1;
            }
            double wt = strtod(colon + 1, &end);
            if (end == colon + 1 || *end != '\0' || wt <= 0) {
                return -1;
            }
            w->vsizes[n] = (unsigned)size;
            weights[n] = wt;
            total += wt;
            n++;
        }
        if (n == 0) {
            return -1;
        }
        double c = 0.0;
        for (unsigned i = 0; i < n; i++) {
            c += weights[i] / total;
            w->vcum[i] = c;
        }
        w->vcum[n - 1] = 1.0;
        w->nvsizes = n;
        w->vmode = PACE_VSIZE_LIST;
        return 0;
    }
    unsigned long a = strtoul(s, &end, 10);
    if (end == s || a == 0 || a > (1u << 30)) {
        return -1;
    }
    if (*end == '-') {
        const char *b = end + 1;
        unsigned long hi = strtoul(b, &end, 10);
        if (end == b || *end != '\0' || hi < a || hi > (1u << 30)) {
            return -1;
        }
        w->vmode = PACE_VSIZE_RANGE;
        w->vmin = (unsigned)a;
        w->vmax = (unsigned)hi;
        return 0;
    }
    if (*end != '\0') {
        return -1;
    }
    w->vmode = PACE_VSIZE_FIXED;
    w->vmin = w->vmax = (unsigned)a;
    return 0;
}

int pace_wl_preset(pace_workload *w, const char *name, char *err, size_t errlen) {
    memset(w->weight, 0, sizeof(w->weight));
    w->dist = PACE_DIST_ZIPF;
    w->theta = 0.99;
    if (strcmp(name, "ycsb-a") == 0) {
        w->weight[PACE_OP_GET] = 50;
        w->weight[PACE_OP_SET] = 50;
    } else if (strcmp(name, "ycsb-b") == 0) {
        w->weight[PACE_OP_GET] = 95;
        w->weight[PACE_OP_SET] = 5;
    } else if (strcmp(name, "ycsb-c") == 0) {
        w->weight[PACE_OP_GET] = 100;
    } else if (strcmp(name, "ycsb-d") == 0) {
        w->weight[PACE_OP_GET] = 95;
        w->weight[PACE_OP_INSERT] = 5;
        w->dist = PACE_DIST_LATEST;
    } else if (strcmp(name, "ycsb-f") == 0) {
        w->weight[PACE_OP_GET] = 50;
        w->weight[PACE_OP_RMW] = 50;
    } else if (strcmp(name, "ycsb-e") == 0) {
        snprintf(err, errlen, "ycsb-e needs range scans, which key-value protocols do not offer");
        return -1;
    } else {
        snprintf(err, errlen, "unknown workload %s (ycsb-a, ycsb-b, ycsb-c, ycsb-d, ycsb-f)", name);
        return -1;
    }
    return 0;
}

void pace_wl_thread_init(const pace_workload *w, pace_wl_thread *t, unsigned id, unsigned nthreads) {
    uint64_t share = w->nkeys / (nthreads ? nthreads : 1);
    t->start = share * id;
    t->span = id + 1 == nthreads ? w->nkeys - t->start : share;
    if (t->span == 0) {
        t->start = 0;
        t->span = w->nkeys;
    }
    t->cursor = 0;
}

/* Standard normal deviate (Box-Muller). */
static double gauss(pace_rng *r) {
    double u1 = pace_rng_double(r);
    double u2 = pace_rng_double(r);
    if (u1 < 1e-300) {
        u1 = 1e-300;
    }
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

uint64_t pace_wl_key(pace_workload *w, pace_wl_thread *t, pace_rng *r) {
    switch (w->dist) {
    case PACE_DIST_ZIPF:
        return pace_zipf_scramble(pace_zipf_next(&w->zipf, r), w->nkeys);
    case PACE_DIST_SEQUENTIAL: {
        uint64_t k = t->start + t->cursor;
        t->cursor = (t->cursor + 1) % t->span;
        return k;
    }
    case PACE_DIST_GAUSSIAN: {
        double v = (double)w->nkeys / 2.0 + gauss(r) * w->gauss_sd * (double)w->nkeys;
        if (v < 0.0) {
            v = 0.0;
        }
        uint64_t k = (uint64_t)v;
        return k >= w->nkeys ? w->nkeys - 1 : k;
    }
    case PACE_DIST_LATEST: {
        uint64_t n = atomic_load_explicit(&w->inserted, memory_order_relaxed);
        uint64_t rank = pace_zipf_next(&w->zipf, r);
        return rank < n ? n - 1 - rank : 0;
    }
    case PACE_DIST_UNIFORM:
    default: {
        uint64_t n = atomic_load_explicit(&w->inserted, memory_order_relaxed);
        return pace_rng_below(r, n);
    }
    }
}

unsigned pace_wl_vsize(const pace_workload *w, pace_rng *r) {
    switch (w->vmode) {
    case PACE_VSIZE_RANGE:
        return w->vmin + (unsigned)pace_rng_below(r, (uint64_t)(w->vmax - w->vmin) + 1);
    case PACE_VSIZE_LIST: {
        double u = pace_rng_double(r);
        for (unsigned i = 0; i < w->nvsizes; i++) {
            if (u < w->vcum[i]) {
                return w->vsizes[i];
            }
        }
        return w->vsizes[w->nvsizes - 1];
    }
    case PACE_VSIZE_FIXED:
    default:
        return w->vmin;
    }
}

unsigned pace_wl_vsize_max(const pace_workload *w) {
    if (w->vmode == PACE_VSIZE_LIST) {
        unsigned m = 0;
        for (unsigned i = 0; i < w->nvsizes; i++) {
            m = w->vsizes[i] > m ? w->vsizes[i] : m;
        }
        return m;
    }
    return w->vmax;
}

void pace_wl_next(pace_workload *w, pace_wl_thread *t, pace_rng *r, pace_op *op) {
    double u = pace_rng_double(r);
    pace_op_type type = PACE_OP_GET;
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        if (u < w->cum[i]) {
            type = (pace_op_type)i;
            break;
        }
    }
    op->type = type;
    op->nkeys = 1;
    op->value_size = 0;
    if (type == PACE_OP_INSERT) {
        op->key = atomic_fetch_add_explicit(&w->inserted, 1, memory_order_relaxed);
    } else {
        op->key = pace_wl_key(w, t, r);
    }
    op->keys[0] = op->key;
    if (type == PACE_OP_MGET) {
        op->nkeys = w->mget_keys;
        for (unsigned i = 1; i < op->nkeys; i++) {
            op->keys[i] = pace_wl_key(w, t, r);
        }
    }
    if (type == PACE_OP_SET || type == PACE_OP_RMW || type == PACE_OP_INSERT) {
        op->value_size = pace_wl_vsize(w, r);
    }
}

size_t pace_wl_key_text(const pace_workload *w, uint64_t idx, char *out) {
    memcpy(out, w->prefix, w->prefix_len);
    for (int i = (int)PACE_KEY_DIGITS - 1; i >= 0; i--) {
        out[w->prefix_len + (size_t)i] = (char)('0' + idx % 10);
        idx /= 10;
    }
    return w->prefix_len + PACE_KEY_DIGITS;
}
