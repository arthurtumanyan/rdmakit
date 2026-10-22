/*
 * proto_mc.c - memcached text protocol and meta commands, executed on the engine.
 *
 * Reference: memcached protocol.txt (text protocol and meta commands, memcached 1.6). Implemented:
 *   get, gets, gat, gats, set, add, replace, append, prepend, cas, delete, incr, decr, touch, flush_all,
 *   stats, version, verbosity, quit, and the meta commands mg, ms, md, ma, mn.
 * Not implemented (answered with an error): delayed flush_all, meta flags b (base64 keys), I
 * (invalidation), N on mg (vivify on miss), E (explicit CAS), me (debug).
 *
 * Framing: commands end with "\r\n" ("\n" alone is accepted). A storage command is followed by a data
 * block of the announced length and "\r\n". A command line longer than MC_MAX_LINE, or a data block whose
 * terminator is not "\r\n", loses framing and closes the connection after the error reply. A data block
 * larger than the engine's maximum entry is skipped (session.swallow) and answered with SERVER_ERROR,
 * as memcached does.
 *
 * Authentication (memcached -Y): when credentials are configured, the first command must be
 * "set <any key> <flags> <exptime> <bytes>" whose data is "<user> <password>"; it is answered STORED on
 * success and "CLIENT_ERROR authentication failure" otherwise. Other commands before that answer
 * "CLIENT_ERROR unauthenticated".
 *
 * Keys: 1 to 250 bytes without spaces or control characters.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "srv.h"

#define MC_MAX_LINE 2048u
#define MC_MAX_KEY 250u
#define MC_MAX_TOKENS 24u
/* memcached: exptime values above 30 days are absolute Unix times. */
#define MC_RELATIVE_LIMIT 2592000

struct tok {
    const char *p;
    size_t n;
};

int64_t mc_exptime_to_ttl(int64_t exptime) {
    if (exptime == 0) {
        return 0;
    }
    if (exptime < 0) {
        return -1;
    }
    if (exptime > MC_RELATIVE_LIMIT) {
        int64_t left = exptime - (int64_t)time(NULL);
        if (left > INT64_MAX / 1000) {
            left = INT64_MAX / 1000; /* the engine bounds times to live further (AKV_TTL_MAX) */
        }
        return left > 0 ? left * 1000 : -1;
    }
    return exptime * 1000;
}

static int tok_eq(const struct tok *t, const char *s) {
    size_t n = strlen(s);
    return t->n == n && memcmp(t->p, s, n) == 0;
}

/* Parses a decimal unsigned 64-bit number. Returns 0 or -1. */
static int tok_u64(const struct tok *t, uint64_t *v) {
    if (t->n == 0 || t->n > 20) {
        return -1;
    }
    uint64_t x = 0;
    for (size_t i = 0; i < t->n; i++) {
        unsigned c = (unsigned char)t->p[i] - '0';
        if (c > 9 || x > (UINT64_MAX - c) / 10) {
            return -1;
        }
        x = x * 10 + c;
    }
    *v = x;
    return 0;
}

/* Parses a decimal signed 64-bit number. Returns 0 or -1. */
static int tok_i64(const struct tok *t, int64_t *v) {
    struct tok u = *t;
    int neg = 0;
    if (u.n > 0 && u.p[0] == '-') {
        neg = 1;
        u.p++;
        u.n--;
    }
    uint64_t x;
    if (tok_u64(&u, &x) != 0 || x > (uint64_t)INT64_MAX) {
        return -1;
    }
    *v = neg ? -(int64_t)x : (int64_t)x;
    return 0;
}

static int key_ok(const struct tok *t) {
    if (t->n == 0 || t->n > MC_MAX_KEY) {
        return 0;
    }
    for (size_t i = 0; i < t->n; i++) {
        if ((unsigned char)t->p[i] <= ' ' || t->p[i] == 0x7f) {
            return 0;
        }
    }
    return 1;
}

/* Splits a line into tokens separated by spaces. Returns the count, or -1 above MC_MAX_TOKENS. */
static int tokenize(const char *line, size_t n, struct tok *t) {
    int k = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && line[i] == ' ') {
            i++;
        }
        if (i == n) {
            break;
        }
        size_t start = i;
        while (i < n && line[i] != ' ') {
            i++;
        }
        if (k == (int)MC_MAX_TOKENS) {
            return -1;
        }
        t[k].p = line + start;
        t[k].n = i - start;
        k++;
    }
    return k;
}

static void reply(struct obuf *out, int quiet, const char *s) {
    if (!quiet) {
        ob_add(out, s, strlen(s));
    }
}

/* ---- classic commands ---------------------------------------------------------------------------- */

static void cmd_get(akvd *d, const struct tok *t, int nt, int with_cas, int touch, struct obuf *out) {
    int first = 1;
    int64_t ttl = AKV_TTL_KEEP;
    if (touch) {
        int64_t exptime;
        if (nt < 3 || tok_i64(&t[1], &exptime) != 0) {
            reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
            return;
        }
        ttl = mc_exptime_to_ttl(exptime);
        first = 2;
        SRV_COUNT(d, cmd_touch, 1);
    } else if (nt < 2) {
        reply(out, 0, "ERROR\r\n");
        return;
    }
    for (int i = first; i < nt; i++) {
        if (!key_ok(&t[i])) {
            reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
            return;
        }
    }
    for (int i = first; i < nt; i++) {
        akv_meta m;
        const uint8_t *val;
        size_t vlen;
        SRV_COUNT(d, cmd_get, 1);
        int rc = srv_fetch(d, t[i].p, t[i].n, &m, ttl, &val, &vlen);
        if (rc != 0) {
            SRV_COUNT(d, get_misses, 1);
            continue;
        }
        SRV_COUNT(d, get_hits, 1);
        if (with_cas) {
            ob_printf(out, "VALUE %.*s %u %zu %llu\r\n", (int)t[i].n, t[i].p, m.flags, vlen,
                      (unsigned long long)m.cas);
        } else {
            ob_printf(out, "VALUE %.*s %u %zu\r\n", (int)t[i].n, t[i].p, m.flags, vlen);
        }
        ob_add(out, val, vlen);
        ob_add(out, "\r\n", 2);
    }
    reply(out, 0, "END\r\n");
}

static void store_reply(int rc, enum akv_mode mode, int quiet, struct obuf *out) {
    switch (rc) {
    case 0:
        reply(out, quiet, "STORED\r\n");
        break;
    case -EEXIST:
        reply(out, quiet, mode == AKV_CAS ? "EXISTS\r\n" : "NOT_STORED\r\n");
        break;
    case -ENOENT:
        reply(out, quiet, mode == AKV_CAS ? "NOT_FOUND\r\n" : "NOT_STORED\r\n");
        break;
    case -E2BIG:
        reply(out, quiet, "SERVER_ERROR object too large for cache\r\n");
        break;
    case -ENOMEM:
        reply(out, quiet, "SERVER_ERROR out of memory storing object\r\n");
        break;
    default:
        reply(out, quiet, "SERVER_ERROR storage failure\r\n");
        break;
    }
}

/*
 * Parses a storage command line. Returns the data length, or -1 after appending an error reply (framing
 * kept: the data block, if any, cannot be located, so the caller closes the connection on -2).
 */
static int64_t parse_storage(const struct tok *t, int nt, enum akv_mode mode, uint32_t *flags,
                             int64_t *exptime, uint64_t *cas, int *noreply) {
    int need = mode == AKV_CAS ? 6 : 5;
    if (nt < need || nt > need + 1) {
        return -2;
    }
    uint64_t f;
    uint64_t bytes;
    if (tok_u64(&t[2], &f) != 0 || f > UINT32_MAX || tok_i64(&t[3], exptime) != 0 ||
        tok_u64(&t[4], &bytes) != 0 || bytes > INT32_MAX) {
        return -2;
    }
    if (mode == AKV_CAS && tok_u64(&t[5], cas) != 0) {
        return -2;
    }
    *noreply = nt == need + 1 && tok_eq(&t[need], "noreply");
    if (nt == need + 1 && !*noreply) {
        return -2;
    }
    *flags = (uint32_t)f;
    return (int64_t)bytes;
}

static void cmd_delete(akvd *d, const struct tok *t, int nt, struct obuf *out) {
    int noreply = nt >= 3 && tok_eq(&t[nt - 1], "noreply");
    int args = nt - noreply;
    /* "delete <key> 0" is accepted for compatibility with old clients. */
    if (args < 2 || args > 3 || (args == 3 && !tok_eq(&t[2], "0")) || !key_ok(&t[1])) {
        reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
        return;
    }
    int rc = akv_del(akvd_store(d), t[1].p, t[1].n);
    reply(out, noreply, rc == 0 ? "DELETED\r\n" : "NOT_FOUND\r\n");
}

static void cmd_incr(akvd *d, const struct tok *t, int nt, int decr, struct obuf *out) {
    int noreply = nt == 4 && tok_eq(&t[3], "noreply");
    uint64_t delta;
    if ((nt != 3 && !noreply) || !key_ok(&t[1])) {
        reply(out, 0, "ERROR\r\n");
        return;
    }
    if (tok_u64(&t[2], &delta) != 0 || delta > (uint64_t)INT64_MAX) {
        reply(out, 0, "CLIENT_ERROR invalid numeric delta argument\r\n");
        return;
    }
    int64_t result;
    int rc = akv_incr(akvd_store(d), t[1].p, t[1].n, decr ? -(int64_t)delta : (int64_t)delta,
                      AKV_INCR_UNSIGNED, &result, NULL);
    SRV_COUNT(d, cmd_set, 1);
    if (rc == 0) {
        if (!noreply) {
            ob_printf(out, "%llu\r\n", (unsigned long long)(uint64_t)result);
        }
    } else if (rc == -ENOENT) {
        reply(out, noreply, "NOT_FOUND\r\n");
    } else if (rc == -EINVAL) {
        reply(out, noreply, "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n");
    } else {
        store_reply(rc, AKV_SET, noreply, out);
    }
}

static void cmd_touch(akvd *d, const struct tok *t, int nt, struct obuf *out) {
    int noreply = nt == 4 && tok_eq(&t[3], "noreply");
    int64_t exptime;
    if ((nt != 3 && !noreply) || !key_ok(&t[1]) || tok_i64(&t[2], &exptime) != 0) {
        reply(out, 0, "ERROR\r\n");
        return;
    }
    SRV_COUNT(d, cmd_touch, 1);
    int rc = akv_touch(akvd_store(d), t[1].p, t[1].n, mc_exptime_to_ttl(exptime));
    reply(out, noreply, rc == 0 ? "TOUCHED\r\n" : "NOT_FOUND\r\n");
}

static void cmd_stats(akvd *d, const struct tok *t, int nt, struct obuf *out) {
    if (nt > 1 && !tok_eq(&t[1], "reset")) {
        /* Sub-statistics (items, slabs, settings, ...) have no equivalent: an empty list. */
        reply(out, 0, "END\r\n");
        return;
    }
    if (nt > 1) {
        reply(out, 0, "RESET\r\n");
        return;
    }
    akv_stats st;
    akv_stats_get(akvd_store(d), &st);
    struct srv_counters *c = akvd_counters(d);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
#define LD(f) ((unsigned long long)__atomic_load_n(&c->f, __ATOMIC_RELAXED))
    ob_printf(
        out,
        "STAT pid %d\r\nSTAT uptime %llu\r\nSTAT time %lld\r\nSTAT version %s\r\nSTAT pointer_size %d\r\n"
        "STAT curr_connections %u\r\nSTAT total_connections %llu\r\nSTAT rejected_connections %llu\r\n"
        "STAT cmd_get %llu\r\nSTAT cmd_set %llu\r\nSTAT cmd_flush %llu\r\nSTAT cmd_touch %llu\r\n"
        "STAT get_hits %llu\r\nSTAT get_misses %llu\r\nSTAT auth_errors %llu\r\n"
        "STAT curr_items %llu\r\nSTAT total_items %llu\r\nSTAT evictions %llu\r\nSTAT reclaimed %llu\r\n"
        "STAT expired %llu\r\nSTAT bytes %llu\r\nSTAT limit_maxbytes %llu\r\nSTAT index_bytes %llu\r\n"
        "STAT threads %u\r\nEND\r\n",
        (int)getpid(), (unsigned long long)akvd_uptime(d), (long long)now.tv_sec, AKVD_VERSION,
        (int)(8 * sizeof(void *)), __atomic_load_n(&c->curr_connections, __ATOMIC_RELAXED),
        LD(total_connections), LD(rejected_connections), LD(cmd_get), LD(cmd_set), LD(cmd_flush),
        LD(cmd_touch), LD(get_hits), LD(get_misses), LD(auth_failures), (unsigned long long)st.entries,
        (unsigned long long)st.puts, (unsigned long long)(st.evictions + st.index_evictions),
        (unsigned long long)st.expired, (unsigned long long)st.expired, (unsigned long long)st.log_bytes,
        (unsigned long long)st.arena_bytes, (unsigned long long)st.index_bytes, akvd_threads(d));
#undef LD
}

/* ---- meta commands ------------------------------------------------------------------------------- */

/* Return flags requested by a meta command, echoed in request order. */
struct meta_ret {
    char flags[MC_MAX_TOKENS];
    int n;
    struct tok opaque;
    int quiet;
    int want_value;
};

/* Appends the requested return flags of a hit. */
static void meta_flags(struct obuf *out, const struct meta_ret *r, const struct tok *key, const akv_meta *m,
                       size_t vlen) {
    for (int i = 0; i < r->n; i++) {
        switch (r->flags[i]) {
        case 'f':
            ob_printf(out, " f%u", m->flags);
            break;
        case 'c':
            ob_printf(out, " c%llu", (unsigned long long)m->cas);
            break;
        case 't':
            ob_printf(out, " t%lld", m->ttl_ms < 0 ? -1LL : (long long)((m->ttl_ms + 999) / 1000));
            break;
        case 's':
            ob_printf(out, " s%zu", vlen);
            break;
        case 'k':
            ob_printf(out, " k%.*s", (int)key->n, key->p);
            break;
        case 'O':
            ob_printf(out, " O%.*s", (int)r->opaque.n, r->opaque.p);
            break;
        default:
            break;
        }
    }
}

/* Appends the opaque and key flags of a miss or status reply. */
static void meta_echo(struct obuf *out, const struct meta_ret *r, const struct tok *key) {
    for (int i = 0; i < r->n; i++) {
        if (r->flags[i] == 'O') {
            ob_printf(out, " O%.*s", (int)r->opaque.n, r->opaque.p);
        } else if (r->flags[i] == 'k') {
            ob_printf(out, " k%.*s", (int)key->n, key->p);
        }
    }
}

static void meta_status(struct obuf *out, const char *code, const struct meta_ret *r, const struct tok *key) {
    ob_add(out, code, 2);
    meta_echo(out, r, key);
    ob_add(out, "\r\n", 2);
}

/*
 * Parses meta flags t[first..nt). `allowed` lists the accepted flag letters; letters in `retflags` are
 * return flags. Values of T, F, C, M, N, J, D are stored in vals[letter]. Returns 0, or -1 for an invalid
 * or unsupported flag.
 */
static int meta_parse(const struct tok *t, int first, int nt, const char *allowed, struct meta_ret *r,
                      struct tok vals[128], int has[128]) {
    memset(r, 0, sizeof(*r));
    memset(has, 0, 128 * sizeof(int));
    for (int i = first; i < nt; i++) {
        unsigned char c = (unsigned char)t[i].p[0];
        if (c >= 128 || strchr(allowed, c) == NULL) {
            return -1;
        }
        struct tok v = {t[i].p + 1, t[i].n - 1};
        has[c] = 1;
        vals[c] = v;
        switch (c) {
        case 'q':
            r->quiet = 1;
            break;
        case 'v':
            r->want_value = 1;
            break;
        case 'O':
            r->opaque = v;
            r->flags[r->n++] = 'O';
            break;
        case 'f':
        case 'c':
        case 't':
        case 's':
        case 'k':
            if (v.n != 0) {
                return -1;
            }
            r->flags[r->n++] = (char)c;
            break;
        default:
            break;
        }
    }
    return 0;
}

static void cmd_mg(akvd *d, const struct tok *t, int nt, struct obuf *out) {
    struct meta_ret r;
    struct tok vals[128];
    int has[128];
    if (nt < 2 || !key_ok(&t[1]) || meta_parse(t, 2, nt, "vfctskqOT", &r, vals, has) != 0) {
        reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
        return;
    }
    int64_t touch = AKV_TTL_KEEP;
    if (has['T']) {
        int64_t e;
        if (tok_i64(&vals['T'], &e) != 0) {
            reply(out, 0, "CLIENT_ERROR bad token in command line format\r\n");
            return;
        }
        touch = mc_exptime_to_ttl(e);
    }
    akv_meta m;
    const uint8_t *val;
    size_t vlen;
    SRV_COUNT(d, cmd_get, 1);
    int rc = srv_fetch(d, t[1].p, t[1].n, &m, touch, &val, &vlen);
    if (rc != 0) {
        SRV_COUNT(d, get_misses, 1);
        if (!r.quiet) {
            meta_status(out, "EN", &r, &t[1]);
        }
        return;
    }
    SRV_COUNT(d, get_hits, 1);
    if (r.want_value) {
        ob_printf(out, "VA %zu", vlen);
        meta_flags(out, &r, &t[1], &m, vlen);
        ob_add(out, "\r\n", 2);
        ob_add(out, val, vlen);
        ob_add(out, "\r\n", 2);
    } else {
        ob_add(out, "HD", 2);
        meta_flags(out, &r, &t[1], &m, vlen);
        ob_add(out, "\r\n", 2);
    }
}

/* Executes ms with its data block. */
static void cmd_ms(akvd *d, const struct tok *t, int nt, const uint8_t *data, size_t dlen, struct obuf *out) {
    struct meta_ret r;
    struct tok vals[128];
    int has[128];
    if (meta_parse(t, 3, nt, "TFCMqOkcI", &r, vals, has) != 0 || has['I']) {
        reply(out, 0, "CLIENT_ERROR invalid flag\r\n");
        return;
    }
    akv_store_opts o = {.mode = AKV_SET};
    uint64_t v;
    if (has['F']) {
        if (tok_u64(&vals['F'], &v) != 0 || v > UINT32_MAX) {
            reply(out, 0, "CLIENT_ERROR bad token in command line format\r\n");
            return;
        }
        o.flags = (uint32_t)v;
    }
    if (has['T']) {
        int64_t e;
        if (tok_i64(&vals['T'], &e) != 0) {
            reply(out, 0, "CLIENT_ERROR bad token in command line format\r\n");
            return;
        }
        o.ttl_ms = mc_exptime_to_ttl(e);
    }
    if (has['M']) {
        char mode = vals['M'].n == 1 ? vals['M'].p[0] : '?';
        switch (mode) {
        case 'E':
        case 'e':
            o.mode = AKV_ADD;
            break;
        case 'A':
        case 'a':
            o.mode = AKV_APPEND;
            break;
        case 'P':
        case 'p':
            o.mode = AKV_PREPEND;
            break;
        case 'R':
        case 'r':
            o.mode = AKV_REPLACE;
            break;
        case 'S':
        case 's':
            o.mode = AKV_SET;
            break;
        default:
            reply(out, 0, "CLIENT_ERROR invalid mode for ms\r\n");
            return;
        }
    }
    if (has['C']) {
        if (tok_u64(&vals['C'], &o.cas) != 0) {
            reply(out, 0, "CLIENT_ERROR bad token in command line format\r\n");
            return;
        }
        if (o.mode != AKV_SET) {
            reply(out, 0, "CLIENT_ERROR CAS with a mode other than set is not supported\r\n");
            return;
        }
        o.mode = AKV_CAS;
    }
    uint64_t cas = 0;
    SRV_COUNT(d, cmd_set, 1);
    int rc = akv_store(akvd_store(d), t[1].p, t[1].n, data, dlen, &o, &cas);
    const char *code;
    switch (rc) {
    case 0:
        if (r.quiet) {
            return;
        }
        ob_add(out, "HD", 2);
        for (int i = 0; i < r.n; i++) {
            if (r.flags[i] == 'c') {
                ob_printf(out, " c%llu", (unsigned long long)cas);
            }
        }
        meta_echo(out, &r, &t[1]);
        ob_add(out, "\r\n", 2);
        return;
    case -EEXIST:
        code = o.mode == AKV_CAS ? "EX" : "NS";
        break;
    case -ENOENT:
        code = o.mode == AKV_CAS ? "NF" : "NS";
        break;
    case -E2BIG:
        reply(out, 0, "SERVER_ERROR object too large for cache\r\n");
        return;
    case -ENOMEM:
        reply(out, 0, "SERVER_ERROR out of memory storing object\r\n");
        return;
    default:
        reply(out, 0, "SERVER_ERROR storage failure\r\n");
        return;
    }
    meta_status(out, code, &r, &t[1]);
}

static void cmd_md(akvd *d, const struct tok *t, int nt, struct obuf *out) {
    struct meta_ret r;
    struct tok vals[128];
    int has[128];
    if (nt < 2 || !key_ok(&t[1]) || meta_parse(t, 2, nt, "qOkC", &r, vals, has) != 0) {
        reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
        return;
    }
    akv *s = akvd_store(d);
    if (has['C']) {
        uint64_t want;
        akv_meta m;
        size_t vlen;
        uint8_t dummy;
        if (tok_u64(&vals['C'], &want) != 0) {
            reply(out, 0, "CLIENT_ERROR bad token in command line format\r\n");
            return;
        }
        int rc = akv_get_meta(s, t[1].p, t[1].n, &dummy, 0, &vlen, &m, AKV_TTL_KEEP);
        if (rc == -ENOENT) {
            meta_status(out, "NF", &r, &t[1]);
            return;
        }
        if (m.cas != want) {
            meta_status(out, "EX", &r, &t[1]);
            return;
        }
    }
    /* Quiet mode suppresses HD and NF of md (memcached protocol.txt, meta delete). */
    int rc = akv_del(s, t[1].p, t[1].n);
    if (!r.quiet) {
        meta_status(out, rc == 0 ? "HD" : "NF", &r, &t[1]);
    }
}

static void cmd_ma(akvd *d, const struct tok *t, int nt, struct obuf *out) {
    struct meta_ret r;
    struct tok vals[128];
    int has[128];
    if (nt < 2 || !key_ok(&t[1]) || meta_parse(t, 2, nt, "NJDTMqOktcv", &r, vals, has) != 0) {
        reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
        return;
    }
    uint64_t delta = 1;
    uint64_t initial = 0;
    int64_t viv_ttl = 0;
    int decr = 0;
    if ((has['D'] && tok_u64(&vals['D'], &delta) != 0) || (has['J'] && tok_u64(&vals['J'], &initial) != 0) ||
        (has['N'] && tok_i64(&vals['N'], &viv_ttl) != 0) || delta > (uint64_t)INT64_MAX) {
        reply(out, 0, "CLIENT_ERROR bad token in command line format\r\n");
        return;
    }
    if (has['M']) {
        char mode = vals['M'].n == 1 ? vals['M'].p[0] : '?';
        if (mode == 'D' || mode == 'd' || mode == '-') {
            decr = 1;
        } else if (!(mode == 'I' || mode == 'i' || mode == '+')) {
            reply(out, 0, "CLIENT_ERROR invalid mode for ma\r\n");
            return;
        }
    }
    akv *s = akvd_store(d);
    int64_t result = 0;
    SRV_COUNT(d, cmd_set, 1);
    int rc = akv_incr(s, t[1].p, t[1].n, decr ? -(int64_t)delta : (int64_t)delta, AKV_INCR_UNSIGNED, &result,
                      NULL);
    if (rc == -ENOENT && has['N']) {
        char text[24];
        int n = snprintf(text, sizeof(text), "%llu", (unsigned long long)initial);
        akv_store_opts o = {.mode = AKV_ADD, .ttl_ms = mc_exptime_to_ttl(viv_ttl)};
        rc = akv_store(s, t[1].p, t[1].n, text, (size_t)n, &o, NULL);
        result = (int64_t)initial;
        if (rc == -EEXIST) { /* created concurrently: apply the delta to it */
            rc = akv_incr(s, t[1].p, t[1].n, decr ? -(int64_t)delta : (int64_t)delta, AKV_INCR_UNSIGNED,
                          &result, NULL);
        }
    }
    if (rc == -ENOENT) {
        meta_status(out, "NF", &r, &t[1]);
        return;
    }
    if (rc == -EINVAL) {
        reply(out, 0, "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n");
        return;
    }
    if (rc != 0) {
        store_reply(rc, AKV_SET, 0, out);
        return;
    }
    if (has['T']) {
        int64_t e;
        if (tok_i64(&vals['T'], &e) == 0) {
            akv_touch(s, t[1].p, t[1].n, mc_exptime_to_ttl(e));
        }
    }
    akv_meta m = {0};
    uint8_t dummy;
    size_t vlen = 0;
    akv_get_meta(s, t[1].p, t[1].n, &dummy, 0, &vlen, &m, AKV_TTL_KEEP);
    char text[24];
    int n = snprintf(text, sizeof(text), "%llu", (unsigned long long)(uint64_t)result);
    if (r.want_value) {
        ob_printf(out, "VA %d", n);
        meta_flags(out, &r, &t[1], &m, (size_t)n);
        ob_printf(out, "\r\n%s\r\n", text);
    } else if (!r.quiet) {
        ob_add(out, "HD", 2);
        meta_flags(out, &r, &t[1], &m, (size_t)n);
        ob_add(out, "\r\n", 2);
    }
}

/* ---- dispatcher ----------------------------------------------------------------------------------- */

/* Finds the end of the first line. Returns the length of the line without its terminator and sets *adv
 * to the bytes up to and including it, or returns -1 when no complete line is present. */
static ssize_t line_end(const uint8_t *in, size_t len, size_t *adv) {
    const uint8_t *nl = memchr(in, '\n', len);
    if (nl == NULL) {
        return -1;
    }
    size_t n = (size_t)(nl - in);
    *adv = n + 1;
    if (n > 0 && in[n - 1] == '\r') {
        n--;
    }
    return (ssize_t)n;
}

/* Handles the authentication "set" of memcached -Y. */
static void do_auth(akvd *d, struct session *ss, const uint8_t *data, size_t dlen, struct obuf *out) {
    const uint8_t *sp = memchr(data, ' ', dlen);
    int ok = 0;
    if (sp != NULL) {
        size_t ulen = (size_t)(sp - data);
        ok = akvd_login(d, ss, (const char *)data, ulen, (const char *)sp + 1, dlen - ulen - 1);
    } else {
        SRV_COUNT(d, auth_failures, 1);
    }
    if (ok) {
        reply(out, 0, "STORED\r\n");
    } else {
        reply(out, 0, "CLIENT_ERROR authentication failure\r\n");
    }
}

/*
 * Permission check of a memcached command (tokens t[0, nt)) for the session's user. Retrievals name
 * their keys from the first (gat, gats: second) argument to the last; other commands name one key in
 * argument 1. Returns 1 when the command may run, else replies with CLIENT_ERROR and returns 0.
 */
static int mc_permitted(const struct session *ss, const struct tok *t, int nt, struct obuf *out) {
    if (ss->user == NULL || acl_unrestricted(ss->user)) {
        return 1;
    }
    int id = acl_cmd_find(ACL_MC, t[0].p, t[0].n);
    if (id < 0) {
        return 1; /* unknown: answered ERROR */
    }
    int ok = acl_cmd_allowed(ss->user, (unsigned)id);
    const acl_cmd *k = acl_cmd_at((unsigned)id);
    if (ok && k->first > 0) {
        int last = k->last < 0 ? nt + k->last : k->last;
        if (t[nt - 1].n == 7 && memcmp(t[nt - 1].p, "noreply", 7) == 0 && k->last < 0) {
            last--;
        }
        for (int i = k->first; ok && i <= last && i < nt; i++) {
            ok = acl_key_allowed(ss->user, t[i].p, t[i].n, k->access);
        }
    }
    if (!ok) {
        reply(out, 0, "CLIENT_ERROR permission denied\r\n");
    }
    return ok;
}

/*
 * Prefetches the index buckets (akv_prefetch), then the entries (akv_prefetch_entry), of the keys of up to
 * MC_PREFETCH consecutive retrieval lines ("get", "gets", "mg" with one key) starting at in, so that the
 * lookups of pipelined commands overlap their cache misses. Stops at the first other command, because a
 * storage command is followed by a data block that is not a command line. Returns the offset after the
 * last line examined.
 */
#define MC_PREFETCH 8
static size_t mc_prefetch(akvd *d, const uint8_t *in, size_t len) {
    const uint8_t *keys[MC_PREFETCH];
    size_t klens[MC_PREFETCH];
    int nk = 0;
    size_t off = 0;
    akv *s = akvd_store(d);
    while (nk < MC_PREFETCH && off < len) {
        size_t adv;
        ssize_t ln = line_end(in + off, len - off, &adv);
        if (ln < 0) {
            break;
        }
        const uint8_t *p = in + off;
        size_t skip = 0;
        if (ln > 4 && memcmp(p, "get ", 4) == 0) {
            skip = 4;
        } else if (ln > 5 && memcmp(p, "gets ", 5) == 0) {
            skip = 5;
        } else if (ln > 3 && memcmp(p, "mg ", 3) == 0) {
            skip = 3;
        } else {
            break;
        }
        const uint8_t *k = p + skip;
        const uint8_t *end = p + ln;
        const uint8_t *sp = memchr(k, ' ', (size_t)(end - k));
        size_t klen = (size_t)((sp != NULL ? sp : end) - k);
        if (klen > 0 && klen <= MC_MAX_KEY) {
            akv_prefetch(s, k, klen);
            keys[nk] = k;
            klens[nk] = klen;
            nk++;
        }
        off += adv;
    }
    for (int i = 0; nk > 1 && i < nk; i++) {
        akv_prefetch_entry(s, keys[i], klens[i]);
    }
    return off;
}

size_t mc_execute(akvd *d, struct session *ss, const uint8_t *in, size_t len, struct obuf *out) {
    size_t used = 0;
    size_t prefetched = 0; /* input before this offset had its keys prefetched */
    if (ss->swallow > 0) {
        size_t take = ss->swallow < len ? ss->swallow : len;
        ss->swallow -= take;
        used = take;
    }
    while (used < len && ss->swallow == 0 && !ss->close && ob_pending(out) <= OB_SOFT_LIMIT) {
        const uint8_t *p = in + used;
        size_t left = len - used;
        if (used >= prefetched) {
            prefetched = used + mc_prefetch(d, p, left);
        }
        size_t adv;
        ssize_t ln = line_end(p, left, &adv);
        if (ln < 0) {
            if (left > MC_MAX_LINE) {
                reply(out, 0, "CLIENT_ERROR line too long\r\n");
                ss->close = 1;
                used = len;
            }
            break;
        }
        if ((size_t)ln > MC_MAX_LINE) {
            reply(out, 0, "CLIENT_ERROR line too long\r\n");
            ss->close = 1;
            used = len;
            break;
        }
        struct tok t[MC_MAX_TOKENS];
        int nt = tokenize((const char *)p, (size_t)ln, t);
        if (nt <= 0) {
            used += adv;
            if (nt < 0) {
                reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
            } else {
                reply(out, 0, "ERROR\r\n");
            }
            continue;
        }
        SRV_COUNT(d, commands, 1);
        const struct tok *c = &t[0];

        /* storage commands carry a data block */
        enum akv_mode mode = AKV_SET;
        int storage = 1;
        if (tok_eq(c, "set")) {
            mode = AKV_SET;
        } else if (tok_eq(c, "add")) {
            mode = AKV_ADD;
        } else if (tok_eq(c, "replace")) {
            mode = AKV_REPLACE;
        } else if (tok_eq(c, "append")) {
            mode = AKV_APPEND;
        } else if (tok_eq(c, "prepend")) {
            mode = AKV_PREPEND;
        } else if (tok_eq(c, "cas")) {
            mode = AKV_CAS;
        } else {
            storage = 0;
        }
        int is_ms = tok_eq(c, "ms");
        if (storage || is_ms) {
            int64_t bytes;
            uint32_t flags = 0;
            int64_t exptime = 0;
            uint64_t cas = 0;
            int noreply = 0;
            if (is_ms) {
                uint64_t b;
                bytes =
                    nt >= 3 && key_ok(&t[1]) && tok_u64(&t[2], &b) == 0 && b <= INT32_MAX ? (int64_t)b : -2;
            } else {
                bytes = parse_storage(t, nt, mode, &flags, &exptime, &cas, &noreply);
                if (bytes >= 0 && !key_ok(&t[1])) {
                    bytes = -2;
                }
            }
            if (bytes < 0) {
                /* The data block cannot be located: framing is lost. */
                reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
                ss->close = 1;
                used = len;
                break;
            }
            if ((uint64_t)bytes > akvd_max_entry(d)) {
                reply(out, 0, "SERVER_ERROR object too large for cache\r\n");
                used += adv;
                ss->swallow = (size_t)bytes + 2;
                size_t take = ss->swallow < len - used ? ss->swallow : len - used;
                ss->swallow -= take;
                used += take;
                if (ss->swallow > 0) {
                    break;
                }
                continue;
            }
            if (left < adv + (size_t)bytes + 2) {
                break; /* data block incomplete */
            }
            const uint8_t *data = p + adv;
            if (data[bytes] != '\r' || data[bytes + 1] != '\n') {
                reply(out, 0, "CLIENT_ERROR bad data chunk\r\n");
                ss->close = 1;
                used = len;
                break;
            }
            used += adv + (size_t)bytes + 2;
            if (!ss->authed) {
                if (storage && mode == AKV_SET) {
                    do_auth(d, ss, data, (size_t)bytes, out);
                } else {
                    reply(out, 0, "CLIENT_ERROR unauthenticated\r\n");
                }
                continue;
            }
            if (akvd_session_refresh(d, ss) != 0 || !mc_permitted(ss, t, nt, out)) {
                continue;
            }
            if (is_ms) {
                cmd_ms(d, t, nt, data, (size_t)bytes, out);
                continue;
            }
            akv_store_opts o = {
                .mode = mode, .flags = flags, .ttl_ms = mc_exptime_to_ttl(exptime), .cas = cas};
            SRV_COUNT(d, cmd_set, 1);
            int rc = akv_store(akvd_store(d), t[1].p, t[1].n, data, (size_t)bytes, &o, NULL);
            store_reply(rc, mode, noreply, out);
            continue;
        }
        used += adv;
        if (tok_eq(c, "quit")) {
            ss->close = 1;
            break;
        }
        if (!ss->authed) {
            reply(out, 0, "CLIENT_ERROR unauthenticated\r\n");
            continue;
        }
        if (akvd_session_refresh(d, ss) != 0 || !mc_permitted(ss, t, nt, out)) {
            continue;
        }
        if (tok_eq(c, "get")) {
            cmd_get(d, t, nt, 0, 0, out);
        } else if (tok_eq(c, "gets")) {
            cmd_get(d, t, nt, 1, 0, out);
        } else if (tok_eq(c, "gat")) {
            cmd_get(d, t, nt, 0, 1, out);
        } else if (tok_eq(c, "gats")) {
            cmd_get(d, t, nt, 1, 1, out);
        } else if (tok_eq(c, "mg")) {
            cmd_mg(d, t, nt, out);
        } else if (tok_eq(c, "md")) {
            cmd_md(d, t, nt, out);
        } else if (tok_eq(c, "ma")) {
            cmd_ma(d, t, nt, out);
        } else if (tok_eq(c, "mn")) {
            reply(out, 0, "MN\r\n");
        } else if (tok_eq(c, "delete")) {
            cmd_delete(d, t, nt, out);
        } else if (tok_eq(c, "incr") || tok_eq(c, "decr")) {
            cmd_incr(d, t, nt, tok_eq(c, "decr"), out);
        } else if (tok_eq(c, "touch")) {
            cmd_touch(d, t, nt, out);
        } else if (tok_eq(c, "flush_all")) {
            int noreply = nt >= 2 && tok_eq(&t[nt - 1], "noreply");
            int64_t delay = 0;
            if (nt - noreply == 2 && tok_i64(&t[1], &delay) != 0) {
                reply(out, 0, "CLIENT_ERROR bad command line format\r\n");
            } else if (delay != 0) {
                reply(out, 0, "CLIENT_ERROR delayed flush_all is not supported\r\n");
            } else {
                akv_flush(akvd_store(d));
                SRV_COUNT(d, cmd_flush, 1);
                reply(out, noreply, "OK\r\n");
            }
        } else if (tok_eq(c, "stats")) {
            cmd_stats(d, t, nt, out);
        } else if (tok_eq(c, "version")) {
            /* Protocol level of memcached 1.6 (meta commands), followed by the server's own version. */
            ob_printf(out, "VERSION 1.6.0-arenakv-%s\r\n", AKVD_VERSION);
        } else if (tok_eq(c, "verbosity")) {
            reply(out, nt >= 2 && tok_eq(&t[nt - 1], "noreply"), "OK\r\n");
        } else {
            reply(out, 0, "ERROR\r\n");
        }
    }
    return used;
}
