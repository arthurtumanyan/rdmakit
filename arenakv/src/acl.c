/*
 * acl.c - users, passwords and permissions, see acl.h.
 *
 * A user's command permissions are a bitmap over the command table, built by applying the rules in order:
 * a category rule sets or clears the bits of every command in the category, a command rule the bits of
 * the RESP command of that name. This gives the order-dependent meaning of Valkey ACL rules ("+@all
 * -@dangerous" differs from "-@dangerous +@all").
 */
#include "acl.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

#define R ACL_CAT_READ
#define W ACL_CAT_WRITE
#define KS ACL_CAT_KEYSPACE
#define AD ACL_CAT_ADMIN
#define DG ACL_CAT_DANGEROUS
#define PS ACL_CAT_PUBSUB
#define TX ACL_CAT_TRANSACTION
#define CN ACL_CAT_CONNECTION
#define RP ACL_CAT_REPLICATION
#define KR ACL_KEY_READ
#define KW ACL_KEY_WRITE

/*
 * Categories follow the Valkey command table (valkey.io/commands, "ACL categories") where a command exists
 * there; INFO and memcached stats are dangerous as Valkey's INFO is.
 */
static const acl_cmd CMDS[] = {
    /* RESP */
    {ACL_RESP, "APPEND", W, 1, 1, 1, KW},
    {ACL_RESP, "ASKING", CN, 0, 0, 0, 0},
    {ACL_RESP, "AUTH", CN, 0, 0, 0, 0},
    {ACL_RESP, "BGREWRITEAOF", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "BGSAVE", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "CLIENT", CN, 0, 0, 0, 0},
    {ACL_RESP, "CLUSTER", AD, 0, 0, 0, 0},
    {ACL_RESP, "COMMAND", CN, 0, 0, 0, 0},
    {ACL_RESP, "CONFIG", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "DBSIZE", KS | R, 0, 0, 0, 0},
    {ACL_RESP, "DECR", W, 1, 1, 1, KR | KW},
    {ACL_RESP, "DECRBY", W, 1, 1, 1, KR | KW},
    {ACL_RESP, "DEL", W | KS, 1, -1, 1, KW},
    {ACL_RESP, "DISCARD", TX, 0, 0, 0, 0},
    {ACL_RESP, "DUMP", R | KS, 1, 1, 1, KR},
    {ACL_RESP, "ECHO", CN, 0, 0, 0, 0},
    {ACL_RESP, "EXEC", TX, 0, 0, 0, 0},
    {ACL_RESP, "EXISTS", R | KS, 1, -1, 1, KR},
    {ACL_RESP, "EXPIRE", W | KS, 1, 1, 1, KW},
    {ACL_RESP, "EXPIREAT", W | KS, 1, 1, 1, KW},
    {ACL_RESP, "FLUSHALL", W | KS | DG, 0, 0, 0, 0},
    {ACL_RESP, "FLUSHDB", W | KS | DG, 0, 0, 0, 0},
    {ACL_RESP, "GET", R, 1, 1, 1, KR},
    {ACL_RESP, "GETDEL", W, 1, 1, 1, KR | KW},
    {ACL_RESP, "GETEX", W, 1, 1, 1, KR | KW},
    {ACL_RESP, "GETSET", W, 1, 1, 1, KR | KW},
    {ACL_RESP, "HELLO", CN, 0, 0, 0, 0},
    {ACL_RESP, "INCR", W, 1, 1, 1, KR | KW},
    {ACL_RESP, "INCRBY", W, 1, 1, 1, KR | KW},
    {ACL_RESP, "INFO", DG, 0, 0, 0, 0},
    {ACL_RESP, "KEYS", KS | R | DG, 0, 0, 0, 0},
    {ACL_RESP, "LASTSAVE", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "MGET", R, 1, -1, 1, KR},
    {ACL_RESP, "MIGRATE", W | KS | DG, 0, 0, 0, 0},
    {ACL_RESP, "MSET", W, 1, -1, 2, KW},
    {ACL_RESP, "MULTI", TX, 0, 0, 0, 0},
    {ACL_RESP, "PERSIST", W | KS, 1, 1, 1, KW},
    {ACL_RESP, "PEXPIRE", W | KS, 1, 1, 1, KW},
    {ACL_RESP, "PEXPIREAT", W | KS, 1, 1, 1, KW},
    {ACL_RESP, "PING", CN, 0, 0, 0, 0},
    {ACL_RESP, "PSETEX", W, 1, 1, 1, KW},
    {ACL_RESP, "PSUBSCRIBE", PS, 0, 0, 0, 0},
    {ACL_RESP, "PTTL", R | KS, 1, 1, 1, KR},
    {ACL_RESP, "PUBLISH", PS, 0, 0, 0, 0},
    {ACL_RESP, "PUBSUB", PS, 0, 0, 0, 0},
    {ACL_RESP, "PUNSUBSCRIBE", PS, 0, 0, 0, 0},
    {ACL_RESP, "QUIT", CN, 0, 0, 0, 0},
    {ACL_RESP, "READONLY", CN, 0, 0, 0, 0},
    {ACL_RESP, "READWRITE", CN, 0, 0, 0, 0},
    {ACL_RESP, "REPLICAOF", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "RESTORE", W | KS | DG, 1, 1, 1, KW},
    {ACL_RESP, "RESTORE-ASKING", W | KS | DG, 1, 1, 1, KW},
    {ACL_RESP, "ROLE", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "SAVE", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "SCAN", KS | R, 0, 0, 0, 0},
    {ACL_RESP, "SELECT", CN, 0, 0, 0, 0},
    {ACL_RESP, "SET", W, 1, 1, 1, KW},
    {ACL_RESP, "SETEX", W, 1, 1, 1, KW},
    {ACL_RESP, "SETNX", W, 1, 1, 1, KW},
    {ACL_RESP, "SLAVEOF", AD | DG, 0, 0, 0, 0},
    {ACL_RESP, "STRLEN", R, 1, 1, 1, KR},
    {ACL_RESP, "SUBSCRIBE", PS, 0, 0, 0, 0},
    {ACL_RESP, "TIME", CN, 0, 0, 0, 0},
    {ACL_RESP, "TTL", R | KS, 1, 1, 1, KR},
    {ACL_RESP, "TYPE", KS | R, 1, 1, 1, KR},
    {ACL_RESP, "UNLINK", W | KS, 1, -1, 1, KW},
    {ACL_RESP, "UNSUBSCRIBE", PS, 0, 0, 0, 0},
    {ACL_RESP, "UNWATCH", TX, 0, 0, 0, 0},
    {ACL_RESP, "WAIT", CN, 0, 0, 0, 0},
    {ACL_RESP, "WATCH", TX, 1, -1, 1, KR},
    {ACL_RESP, "ACL", AD | DG, 0, 0, 0, 0},
    /* memcached text and meta (keys: argument 1, or 1..last for retrievals) */
    {ACL_MC, "get", R, 1, -1, 1, KR},
    {ACL_MC, "gets", R, 1, -1, 1, KR},
    {ACL_MC, "gat", R | W, 2, -1, 1, KR | KW},
    {ACL_MC, "gats", R | W, 2, -1, 1, KR | KW},
    {ACL_MC, "mg", R, 1, 1, 1, KR},
    {ACL_MC, "ms", W, 1, 1, 1, KW},
    {ACL_MC, "md", W, 1, 1, 1, KW},
    {ACL_MC, "ma", W, 1, 1, 1, KR | KW},
    {ACL_MC, "mn", CN, 0, 0, 0, 0},
    {ACL_MC, "set", W, 1, 1, 1, KW},
    {ACL_MC, "add", W, 1, 1, 1, KW},
    {ACL_MC, "replace", W, 1, 1, 1, KW},
    {ACL_MC, "append", W, 1, 1, 1, KW},
    {ACL_MC, "prepend", W, 1, 1, 1, KW},
    {ACL_MC, "cas", W, 1, 1, 1, KW},
    {ACL_MC, "delete", W, 1, 1, 1, KW},
    {ACL_MC, "incr", W, 1, 1, 1, KR | KW},
    {ACL_MC, "decr", W, 1, 1, 1, KR | KW},
    {ACL_MC, "touch", W, 1, 1, 1, KW},
    {ACL_MC, "flush_all", W | KS | DG, 0, 0, 0, 0},
    {ACL_MC, "stats", DG, 0, 0, 0, 0},
    {ACL_MC, "version", CN, 0, 0, 0, 0},
    {ACL_MC, "verbosity", CN, 0, 0, 0, 0},
    {ACL_MC, "quit", CN, 0, 0, 0, 0},
    /* native operations (akvproto.h), names after the operation constants */
    {ACL_NATIVE, "GET", R, 1, 1, 1, KR},
    {ACL_NATIVE, "PUT", W, 1, 1, 1, KW},
    {ACL_NATIVE, "DEL", W, 1, 1, 1, KW},
    {ACL_NATIVE, "STATS", DG, 0, 0, 0, 0},
    {ACL_NATIVE, "ADD", W, 1, 1, 1, KW},
    {ACL_NATIVE, "REPLACE", W, 1, 1, 1, KW},
    {ACL_NATIVE, "APPEND", W, 1, 1, 1, KW},
    {ACL_NATIVE, "PREPEND", W, 1, 1, 1, KW},
    {ACL_NATIVE, "CAS", W, 1, 1, 1, KW},
    {ACL_NATIVE, "INCR", W, 1, 1, 1, KR | KW},
    {ACL_NATIVE, "INCRS", W, 1, 1, 1, KR | KW},
    {ACL_NATIVE, "TOUCH", W, 1, 1, 1, KW},
    {ACL_NATIVE, "FLUSH", W | KS | DG, 0, 0, 0, 0},
    {ACL_NATIVE, "HAS", R, 1, 1, 1, KR},
    {ACL_NATIVE, "GETM", R, 1, 1, 1, KR},
    {ACL_NATIVE, "MGET", R, 1, 1, 1, KR},
    {ACL_NATIVE, "AUTH", CN, 0, 0, 0, 0},
    {ACL_NATIVE, "NOOP", CN, 0, 0, 0, 0},
    {ACL_NATIVE, "REPLSYNC", RP, 0, 0, 0, 0},
    {ACL_NATIVE, "ROLE", CN, 0, 0, 0, 0},
};

#define NCMDS (sizeof(CMDS) / sizeof(CMDS[0]))
#define BITMAP_WORDS ((NCMDS + 63) / 64)

const char *const acl_categories[] = {"read",   "write",       "keyspace",   "admin",       "dangerous",
                                      "pubsub", "transaction", "connection", "replication", NULL};

struct pattern {
    char *p;
    size_t n;
    unsigned access; /* keys: ACL_KEY_* bits */
};

struct acl_user {
    char *name;
    int on;
    int nopass;
    uint8_t (*hash)[AKV_SHA256_LEN];
    size_t nhash;
    uint64_t allowed[BITMAP_WORDS];
    struct pattern *keys;
    size_t nkeys;
    struct pattern *chans;
    size_t nchans;
    char *cmdrules; /* the command rules as given, for acl_describe() */
    int unrestricted;
};

struct acl {
    acl_user *users;
    size_t n;
};

unsigned acl_ncmds(void) {
    return (unsigned)NCMDS;
}

const acl_cmd *acl_cmd_at(unsigned id) {
    return id < NCMDS ? &CMDS[id] : NULL;
}

int acl_cmd_find(enum acl_proto proto, const char *name, size_t n) {
    for (unsigned i = 0; i < NCMDS; i++) {
        const acl_cmd *c = &CMDS[i];
        if (c->proto != proto || strlen(c->name) != n) {
            continue;
        }
        if (proto == ACL_RESP ? strncasecmp(c->name, name, n) == 0 : memcmp(c->name, name, n) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- glob ------------------------------------------------------------------------------------------- */

/*
 * Matches one pattern element (a literal, an escaped character, '?' or a class [...] with ranges and '^'
 * negation) against character c. Stores the element's length in *adv. Returns 1 on a match.
 */
static int glob_one(const char *pat, size_t plen, char c, size_t *adv) {
    if (pat[0] == '?') {
        *adv = 1;
        return 1;
    }
    if (pat[0] == '\\' && plen >= 2) {
        *adv = 2;
        return pat[1] == c;
    }
    if (pat[0] != '[') {
        *adv = 1;
        return pat[0] == c;
    }
    size_t i = 1;
    int neg = i < plen && pat[i] == '^';
    i += (size_t)neg;
    int match = 0;
    while (i < plen && pat[i] != ']') {
        if (pat[i] == '\\' && i + 1 < plen) {
            match |= pat[i + 1] == c;
            i += 2;
        } else if (i + 2 < plen && pat[i + 1] == '-' && pat[i + 2] != ']') {
            char lo = pat[i] < pat[i + 2] ? pat[i] : pat[i + 2];
            char hi = pat[i] < pat[i + 2] ? pat[i + 2] : pat[i];
            match |= c >= lo && c <= hi;
            i += 3;
        } else {
            match |= pat[i] == c;
            i++;
        }
    }
    *adv = i < plen ? i + 1 : i; /* the closing bracket, when present */
    return match != neg;
}

/*
 * Iterative matching with one backtracking point: on a mismatch the subject resumes one character after
 * the position where the most recent '*' started to match. Every other element matches exactly one
 * character, which makes this exact, and the cost is O(plen * n) instead of exponential in the number of
 * stars (the subject is a client's key of up to 64 KiB).
 */
int acl_glob(const char *pat, size_t plen, const char *s, size_t n) {
    size_t pi = 0;
    size_t si = 0;
    size_t star = SIZE_MAX;
    size_t mark = 0;
    while (si < n) {
        if (pi < plen && pat[pi] == '*') {
            while (pi < plen && pat[pi] == '*') {
                pi++;
            }
            if (pi == plen) {
                return 1;
            }
            star = pi;
            mark = si;
            continue;
        }
        size_t adv;
        if (pi < plen && glob_one(pat + pi, plen - pi, s[si], &adv)) {
            pi += adv;
            si++;
            continue;
        }
        if (star == SIZE_MAX) {
            return 0;
        }
        pi = star;
        si = ++mark;
    }
    while (pi < plen && pat[pi] == '*') {
        pi++;
    }
    return pi == plen;
}

/* ---- parsing ---------------------------------------------------------------------------------------- */

static void set_err(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...) {
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int parse_hash(const char *s, size_t n, uint8_t out[AKV_SHA256_LEN]) {
    if (n != 2 * AKV_SHA256_LEN) {
        return -1;
    }
    for (size_t i = 0; i < AKV_SHA256_LEN; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static int add_hash(acl_user *u, const uint8_t h[AKV_SHA256_LEN]) {
    for (size_t i = 0; i < u->nhash; i++) {
        if (memcmp(u->hash[i], h, AKV_SHA256_LEN) == 0) {
            return 0;
        }
    }
    void *p = realloc(u->hash, (u->nhash + 1) * sizeof(*u->hash));
    if (p == NULL) {
        return -1;
    }
    u->hash = p;
    memcpy(u->hash[u->nhash++], h, AKV_SHA256_LEN);
    u->nopass = 0;
    return 0;
}

static void del_hash(acl_user *u, const uint8_t h[AKV_SHA256_LEN]) {
    for (size_t i = 0; i < u->nhash; i++) {
        if (memcmp(u->hash[i], h, AKV_SHA256_LEN) == 0) {
            memmove(u->hash[i], u->hash[i + 1], (u->nhash - i - 1) * sizeof(*u->hash));
            u->nhash--;
            return;
        }
    }
}

static int add_pattern(struct pattern **list, size_t *n, const char *p, size_t len, unsigned access) {
    struct pattern *l = realloc(*list, (*n + 1) * sizeof(**list));
    if (l == NULL) {
        return -1;
    }
    *list = l;
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, p, len);
    copy[len] = '\0';
    l[(*n)++] = (struct pattern){.p = copy, .n = len, .access = access};
    return 0;
}

static void clear_patterns(struct pattern **list, size_t *n) {
    for (size_t i = 0; i < *n; i++) {
        free((*list)[i].p);
    }
    free(*list);
    *list = NULL;
    *n = 0;
}

static void set_bit(acl_user *u, unsigned id, int on) {
    if (on) {
        u->allowed[id / 64] |= 1ull << (id % 64);
    } else {
        u->allowed[id / 64] &= ~(1ull << (id % 64));
    }
}

static int append_rule(acl_user *u, const char *tok, size_t n) {
    size_t old = u->cmdrules != NULL ? strlen(u->cmdrules) : 0;
    char *p = realloc(u->cmdrules, old + n + 2);
    if (p == NULL) {
        return -1;
    }
    u->cmdrules = p;
    if (old > 0) {
        p[old++] = ' ';
    }
    for (size_t i = 0; i < n; i++) {
        p[old + i] = (char)tolower((unsigned char)tok[i]);
    }
    p[old + n] = '\0';
    return 0;
}

/* Applies a category rule: on/off for every command of the category (cat = all bits for @all). */
static void apply_category(acl_user *u, uint32_t cat, int on) {
    for (unsigned i = 0; i < NCMDS; i++) {
        if (cat == UINT32_MAX || (CMDS[i].cats & cat) != 0) {
            set_bit(u, i, on);
        }
    }
}

static void free_user(acl_user *u) {
    free(u->name);
    free(u->hash);
    clear_patterns(&u->keys, &u->nkeys);
    clear_patterns(&u->chans, &u->nchans);
    free(u->cmdrules);
}

/* Applies one rule to u. Returns 0, or -1 with err set. */
static int apply_rule(acl_user *u, const char *tok, size_t n, char *err, size_t errlen) {
#define IS(s) (n == strlen(s) && strncasecmp(tok, s, n) == 0)
    uint8_t h[AKV_SHA256_LEN];
    if (IS("on")) {
        u->on = 1;
    } else if (IS("off")) {
        u->on = 0;
    } else if (IS("nopass")) {
        free(u->hash);
        u->hash = NULL;
        u->nhash = 0;
        u->nopass = 1;
    } else if (IS("resetpass")) {
        free(u->hash);
        u->hash = NULL;
        u->nhash = 0;
        u->nopass = 0;
    } else if (tok[0] == '>' || tok[0] == '<') {
        akv_sha256(tok + 1, n - 1, h);
        if (tok[0] == '>') {
            if (add_hash(u, h) != 0) {
                set_err(err, errlen, "out of memory");
                return -1;
            }
        } else {
            del_hash(u, h);
        }
    } else if (tok[0] == '#' || tok[0] == '!') {
        if (parse_hash(tok + 1, n - 1, h) != 0) {
            set_err(err, errlen, "'%.*s': a password hash has 64 hexadecimal digits", (int)n, tok);
            return -1;
        }
        if (tok[0] == '#') {
            if (add_hash(u, h) != 0) {
                set_err(err, errlen, "out of memory");
                return -1;
            }
        } else {
            del_hash(u, h);
        }
    } else if (tok[0] == '~' || (n > 3 && tok[0] == '%')) {
        unsigned access = ACL_KEY_READ | ACL_KEY_WRITE;
        const char *pat = tok + 1;
        if (tok[0] == '%') {
            const char *tilde = memchr(tok, '~', n);
            if (tilde == NULL) {
                set_err(err, errlen, "'%.*s': expected %%R~, %%W~ or %%RW~", (int)n, tok);
                return -1;
            }
            access = 0;
            for (const char *c = tok + 1; c < tilde; c++) {
                if (*c == 'R' || *c == 'r') {
                    access |= ACL_KEY_READ;
                } else if (*c == 'W' || *c == 'w') {
                    access |= ACL_KEY_WRITE;
                } else {
                    set_err(err, errlen, "'%.*s': expected %%R~, %%W~ or %%RW~", (int)n, tok);
                    return -1;
                }
            }
            pat = tilde + 1;
        }
        if (add_pattern(&u->keys, &u->nkeys, pat, n - (size_t)(pat - tok), access) != 0) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
    } else if (IS("allkeys")) {
        if (add_pattern(&u->keys, &u->nkeys, "*", 1, ACL_KEY_READ | ACL_KEY_WRITE) != 0) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
    } else if (IS("resetkeys")) {
        clear_patterns(&u->keys, &u->nkeys);
    } else if (tok[0] == '&') {
        if (add_pattern(&u->chans, &u->nchans, tok + 1, n - 1, 0) != 0) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
    } else if (IS("allchannels")) {
        if (add_pattern(&u->chans, &u->nchans, "*", 1, 0) != 0) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
    } else if (IS("resetchannels")) {
        clear_patterns(&u->chans, &u->nchans);
    } else if (IS("allcommands") || IS("nocommands")) {
        apply_category(u, UINT32_MAX, IS("allcommands"));
        append_rule(u, IS("allcommands") ? "+@all" : "-@all", 5);
    } else if ((tok[0] == '+' || tok[0] == '-') && n > 2 && tok[1] == '@') {
        const char *name = tok + 2;
        size_t nl = n - 2;
        uint32_t cat = 0;
        if (nl == 3 && strncasecmp(name, "all", 3) == 0) {
            cat = UINT32_MAX;
        } else {
            for (unsigned i = 0; acl_categories[i] != NULL; i++) {
                if (strlen(acl_categories[i]) == nl && strncasecmp(acl_categories[i], name, nl) == 0) {
                    cat = 1u << i;
                }
            }
        }
        if (cat == 0) {
            set_err(err, errlen, "'%.*s': unknown category", (int)n, tok);
            return -1;
        }
        apply_category(u, cat, tok[0] == '+');
        append_rule(u, tok, n);
    } else if ((tok[0] == '+' || tok[0] == '-') && n > 1) {
        if (memchr(tok, '|', n) != NULL) {
            set_err(err, errlen, "'%.*s': rules for subcommands are not supported", (int)n, tok);
            return -1;
        }
        int id = acl_cmd_find(ACL_RESP, tok + 1, n - 1);
        if (id < 0) {
            set_err(err, errlen, "'%.*s': unknown command", (int)n, tok);
            return -1;
        }
        set_bit(u, (unsigned)id, tok[0] == '+');
        append_rule(u, tok, n);
    } else if (IS("reset")) {
        u->on = 0;
        free(u->hash);
        u->hash = NULL;
        u->nhash = 0;
        u->nopass = 0;
        clear_patterns(&u->keys, &u->nkeys);
        clear_patterns(&u->chans, &u->nchans);
        apply_category(u, UINT32_MAX, 0);
        free(u->cmdrules);
        u->cmdrules = NULL;
    } else {
        set_err(err, errlen, "'%.*s': unknown rule", (int)n, tok);
        return -1;
    }
    return 0;
#undef IS
}

/* Commands of the connection category are always allowed; computes the unrestricted flag. */
static void finish_user(acl_user *u) {
    for (unsigned i = 0; i < NCMDS; i++) {
        if (CMDS[i].cats == ACL_CAT_CONNECTION) {
            set_bit(u, i, 1);
        }
    }
    int all = 1;
    for (unsigned i = 0; i < NCMDS && all; i++) {
        all = (u->allowed[i / 64] >> (i % 64)) & 1;
    }
    int allkeys = 0;
    for (size_t i = 0; i < u->nkeys; i++) {
        allkeys |= u->keys[i].n == 1 && u->keys[i].p[0] == '*' &&
                   u->keys[i].access == (ACL_KEY_READ | ACL_KEY_WRITE);
    }
    int allch = 0;
    for (size_t i = 0; i < u->nchans; i++) {
        allch |= u->chans[i].n == 1 && u->chans[i].p[0] == '*';
    }
    u->unrestricted = all && allkeys && allch;
}

static acl_user *new_user(acl *a, const char *name, size_t n) {
    acl_user *u = realloc(a->users, (a->n + 1) * sizeof(*u));
    if (u == NULL) {
        return NULL;
    }
    a->users = u;
    u = &a->users[a->n];
    memset(u, 0, sizeof(*u));
    u->name = malloc(n + 1);
    if (u->name == NULL) {
        return NULL;
    }
    memcpy(u->name, name, n);
    u->name[n] = '\0';
    a->n++;
    return u;
}

/* Parses a line of a credentials file of earlier versions. */
static int parse_compat_line(acl *a, const char *line, size_t n, char *err, size_t errlen) {
    const char *colon = memchr(line, ':', n);
    const char *user = colon != NULL ? line : "default";
    size_t ulen = colon != NULL ? (size_t)(colon - line) : 7;
    const char *pass = colon != NULL ? colon + 1 : line;
    size_t plen = n - (size_t)(pass - line);
    if (ulen == 0) {
        set_err(err, errlen, "empty user name");
        return -1;
    }
    uint8_t h[AKV_SHA256_LEN];
    akv_sha256(pass, plen, h);
    /* A repeated user adds a password, as several lines for one user did in earlier versions. */
    acl_user *u = (acl_user *)acl_find(a, user, ulen);
    if (u != NULL) {
        return add_hash(u, h);
    }
    u = new_user(a, user, ulen);
    if (u == NULL || add_hash(u, h) != 0) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    u->on = 1;
    apply_category(u, UINT32_MAX, 1);
    append_rule(u, "+@all", 5);
    add_pattern(&u->keys, &u->nkeys, "*", 1, ACL_KEY_READ | ACL_KEY_WRITE);
    add_pattern(&u->chans, &u->nchans, "*", 1, 0);
    return 0;
}

acl *acl_parse(const char *text, int compat, char *err, size_t errlen) {
    acl *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    int lineno = 0;
    const char *p = text;
    while (*p != '\0') {
        lineno++;
        size_t len = strcspn(p, "\n");
        const char *line = p;
        p += len + (p[len] == '\n');
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
            len--;
        }
        while (len > 0 && (line[0] == ' ' || line[0] == '\t')) {
            line++;
            len--;
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        char msg[200] = "";
        int rc;
        if (compat) {
            rc = parse_compat_line(a, line, len, msg, sizeof(msg));
        } else {
            /* user <name> <rules...> */
            const char *end = line + len;
            const char *t = line;
            size_t tn = strcspn(t, " \t");
            if (tn > (size_t)(end - t)) {
                tn = (size_t)(end - t);
            }
            rc = -1;
            if (tn != 4 || strncasecmp(t, "user", 4) != 0) {
                snprintf(msg, sizeof(msg), "a line starts with 'user'");
            } else {
                t += tn;
                while (t < end && (*t == ' ' || *t == '\t')) {
                    t++;
                }
                size_t nn = 0;
                while (t + nn < end && t[nn] != ' ' && t[nn] != '\t') {
                    nn++;
                }
                if (nn == 0) {
                    snprintf(msg, sizeof(msg), "missing user name");
                } else if (acl_find(a, t, nn) != NULL) {
                    snprintf(msg, sizeof(msg), "user '%.*s' is defined twice", (int)nn, t);
                } else {
                    acl_user *u = new_user(a, t, nn);
                    rc = u != NULL ? 0 : -1;
                    t += nn;
                    while (rc == 0 && t < end) {
                        while (t < end && (*t == ' ' || *t == '\t')) {
                            t++;
                        }
                        size_t rn = 0;
                        while (t + rn < end && t[rn] != ' ' && t[rn] != '\t') {
                            rn++;
                        }
                        if (rn > 0) {
                            rc = apply_rule(u, t, rn, msg, sizeof(msg));
                        }
                        t += rn;
                    }
                }
            }
        }
        if (rc != 0) {
            set_err(err, errlen, "line %d: %s", lineno, msg[0] ? msg : "invalid entry");
            acl_free(a);
            return NULL;
        }
    }
    if (a->n == 0) {
        set_err(err, errlen, "no users defined");
        acl_free(a);
        return NULL;
    }
    for (size_t i = 0; i < a->n; i++) {
        finish_user(&a->users[i]);
    }
    return a;
}

acl *acl_load(const char *path, int compat, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        set_err(err, errlen, "%s: %s", path, strerror(errno));
        return NULL;
    }
    char *text = NULL;
    size_t len = 0;
    size_t cap = 0;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (len + r + 1 > cap) {
            cap = (len + r + 1) * 2;
            char *t = realloc(text, cap);
            if (t == NULL) {
                free(text);
                fclose(f);
                set_err(err, errlen, "out of memory");
                return NULL;
            }
            text = t;
        }
        memcpy(text + len, buf, r);
        len += r;
    }
    fclose(f);
    if (text == NULL) {
        set_err(err, errlen, "%s: no users defined", path);
        return NULL;
    }
    text[len] = '\0';
    char msg[256];
    acl *a = acl_parse(text, compat, msg, sizeof(msg));
    free(text);
    if (a == NULL) {
        set_err(err, errlen, "%s: %s", path, msg);
    }
    return a;
}

void acl_free(acl *a) {
    if (a == NULL) {
        return;
    }
    for (size_t i = 0; i < a->n; i++) {
        free_user(&a->users[i]);
    }
    free(a->users);
    free(a);
}

/* ---- queries ---------------------------------------------------------------------------------------- */

size_t acl_nusers(const acl *a) {
    return a->n;
}

const acl_user *acl_user_at(const acl *a, size_t i) {
    return i < a->n ? &a->users[i] : NULL;
}

const acl_user *acl_find(const acl *a, const char *name, size_t n) {
    for (size_t i = 0; i < a->n; i++) {
        if (strlen(a->users[i].name) == n && memcmp(a->users[i].name, name, n) == 0) {
            return &a->users[i];
        }
    }
    return NULL;
}

const acl_user *acl_default_login(const acl *a) {
    const acl_user *u = acl_find(a, "default", 7);
    return u != NULL && u->on && u->nopass ? u : NULL;
}

const acl_user *acl_login(const acl *a, const char *name, size_t nlen, const char *pass, size_t plen) {
    if (name == NULL || nlen == 0) {
        name = "default";
        nlen = 7;
    }
    const acl_user *u = acl_find(a, name, nlen);
    if (u == NULL || !u->on) {
        return NULL;
    }
    if (u->nopass) {
        return u;
    }
    uint8_t h[AKV_SHA256_LEN];
    akv_sha256(pass, plen, h);
    unsigned match = 0;
    for (size_t i = 0; i < u->nhash; i++) {
        unsigned diff = 0;
        for (unsigned k = 0; k < AKV_SHA256_LEN; k++) {
            diff |= (unsigned)(h[k] ^ u->hash[i][k]);
        }
        match |= diff == 0;
    }
    return match ? u : NULL;
}

const char *acl_user_name(const acl_user *u) {
    return u->name;
}

int acl_user_enabled(const acl_user *u) {
    return u->on;
}

int acl_unrestricted(const acl_user *u) {
    return u->unrestricted;
}

int acl_cmd_allowed(const acl_user *u, unsigned id) {
    return id < NCMDS && ((u->allowed[id / 64] >> (id % 64)) & 1) != 0;
}

int acl_key_allowed(const acl_user *u, const void *key, size_t n, unsigned access) {
    unsigned granted = 0;
    for (size_t i = 0; i < u->nkeys && (granted & access) != access; i++) {
        if ((u->keys[i].access & access & ~granted) != 0 && acl_glob(u->keys[i].p, u->keys[i].n, key, n)) {
            granted |= u->keys[i].access;
        }
    }
    return (granted & access) == access;
}

int acl_channel_allowed(const acl_user *u, const void *ch, size_t n) {
    for (size_t i = 0; i < u->nchans; i++) {
        if (acl_glob(u->chans[i].p, u->chans[i].n, ch, n)) {
            return 1;
        }
    }
    return 0;
}

int acl_channel_pattern_allowed(const acl_user *u, const void *pat, size_t n) {
    for (size_t i = 0; i < u->nchans; i++) {
        if ((u->chans[i].n == 1 && u->chans[i].p[0] == '*') ||
            (u->chans[i].n == n && memcmp(u->chans[i].p, pat, n) == 0)) {
            return 1;
        }
    }
    return 0;
}

size_t acl_describe(const acl_user *u, char *out, size_t cap) {
    size_t len = 0;
#define PUT(...)                                                                                             \
    do {                                                                                                     \
        int w_ = snprintf(out + len, len < cap ? cap - len : 0, __VA_ARGS__);                                \
        len += w_ > 0 ? (size_t)w_ : 0;                                                                      \
    } while (0)
    PUT("user %s %s", u->name, u->on ? "on" : "off");
    if (u->nopass) {
        PUT(" nopass");
    }
    for (size_t i = 0; i < u->nhash; i++) {
        PUT(" #");
        for (unsigned k = 0; k < AKV_SHA256_LEN; k++) {
            PUT("%02x", u->hash[i][k]);
        }
    }
    for (size_t i = 0; i < u->nkeys; i++) {
        const char *pre = u->keys[i].access == ACL_KEY_READ    ? "%R~"
                          : u->keys[i].access == ACL_KEY_WRITE ? "%W~"
                                                               : "~";
        PUT(" %s%s", pre, u->keys[i].p);
    }
    if (u->nchans == 0) {
        PUT(" resetchannels");
    }
    for (size_t i = 0; i < u->nchans; i++) {
        PUT(" &%s", u->chans[i].p);
    }
    PUT(" %s", u->cmdrules != NULL ? u->cmdrules : "-@all");
#undef PUT
    if (cap > 0 && len >= cap) {
        out[cap - 1] = '\0';
    }
    return len;
}
