/*
 * proto_resp.c - RESP2 and RESP3 (Redis serialization protocol), executed on the engine.
 *
 * Reference: Redis protocol specification (RESP2, RESP3) and the Redis command reference. Requests are
 * arrays of bulk strings or inline commands (space-separated words on one line). A session speaks RESP2
 * until HELLO 3; RESP3 then changes the reply types of nulls (_), maps (HELLO, CONFIG GET, ACL GETUSER)
 * and INFO (verbatim string). Implemented commands:
 *   connection   PING ECHO AUTH HELLO (protocols 2 and 3) QUIT RESET SELECT (database 0) CLIENT (SETNAME
 *                GETNAME ID SETINFO) COMMAND (COUNT DOCS INFO, empty) CONFIG GET
 *   transactions MULTI EXEC DISCARD WATCH UNWATCH
 *   pub/sub      SUBSCRIBE UNSUBSCRIBE PSUBSCRIBE PUNSUBSCRIBE PUBLISH PUBSUB (CHANNELS NUMSUB NUMPAT); a RESP2
 *                session with subscriptions accepts only these, PING, QUIT and RESET (RESP3 accepts all)
 *   strings      GET SET (EX PX EXAT PXAT NX XX KEEPTTL GET) SETNX SETEX PSETEX GETSET GETDEL GETEX
 *                MGET MSET APPEND STRLEN INCR INCRBY DECR DECRBY
 *   keys         DEL UNLINK EXISTS TYPE EXPIRE PEXPIRE EXPIREAT PEXPIREAT TTL PTTL PERSIST KEYS SCAN
 *   server       DBSIZE FLUSHALL FLUSHDB INFO SAVE BGSAVE LASTSAVE TIME
 * Values are strings; the engine has no other data types (commands of other types answer "unknown
 * command"). Composite commands that read and then write (SET ... GET, GETSET, GETDEL, GETEX, APPEND on a missing
 * key, MSET) run under a lock set on their keys, so they are atomic as in Redis. SCAN returns all matching keys in one reply
 * with cursor 0, which satisfies its guarantees; KEYS and SCAN copy each shard while its lock is held.
 *
 * Transactions: commands after MULTI are queued (answered QUEUED). EXEC locks the engine shards of every
 * key of the queued commands and of the watched keys (all shards when a queued command has no key
 * positions and uses the engine), compares the CAS versions recorded by WATCH, runs the queue, and
 * releases the locks: no other client observes or changes those keys in between, so the transaction is
 * atomic and isolated, and its change records are applied together by the append-only log. As in Valkey,
 * an unknown or refused command while queueing aborts the transaction (EXECABORT), while a command that
 * fails during EXEC reports its error in its place and the others still run. WATCH compares CAS
 * versions: a key that is created and removed again between WATCH and EXEC is not seen as changed.
 */
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "srv.h"

#define RESP_MAX_ARGS 1048576u
#define RESP_MAX_INLINE 65536u

struct arg {
    const char *p;
    size_t n;
};

/* Thread-local argument vector. */
static __thread struct arg *tl_args;
static __thread size_t tl_args_cap;

static int args_reserve(size_t n) {
    if (n <= tl_args_cap) {
        return 0;
    }
    size_t cap = tl_args_cap ? tl_args_cap : 16;
    while (cap < n) {
        cap *= 2;
    }
    struct arg *a = realloc(tl_args, cap * sizeof(*a));
    if (a == NULL) {
        return -1;
    }
    tl_args = a;
    tl_args_cap = cap;
    return 0;
}

void resp_thread_exit(void) {
    free(tl_args);
    tl_args = NULL;
    tl_args_cap = 0;
}

/* ---- replies ---------------------------------------------------------------------------------------- */

static void r_raw(struct obuf *o, const char *s) {
    ob_add(o, s, strlen(s));
}

static void r_ok(struct obuf *o) {
    ob_add(o, "+OK\r\n", 5);
}

static void r_err(struct obuf *o, const char *msg) {
    ob_printf(o, "-%s\r\n", msg);
}

static void r_int(struct obuf *o, long long v) {
    ob_printf(o, ":%lld\r\n", v);
}

static void r_bulk(struct obuf *o, const void *p, size_t n) {
    ob_printf(o, "$%zu\r\n", n);
    ob_add(o, p, n);
    ob_add(o, "\r\n", 2);
}

static void r_bulk_str(struct obuf *o, const char *s) {
    r_bulk(o, s, strlen(s));
}

/* RESP protocol of the session whose request is being executed (resp_execute sets it per call). */
static __thread int tl_resp3;

/* Set while this thread holds an engine lock set for a command (EXEC or a composite command). */
static __thread int tl_locked;

static void r_null(struct obuf *o) {
    if (tl_resp3) {
        ob_add(o, "_\r\n", 3);
    } else {
        ob_add(o, "$-1\r\n", 5);
    }
}

/* The null array of RESP2 (an aborted EXEC); RESP3 has one null. */
/* An out-of-band frame: RESP3 push, RESP2 array (pub/sub confirmations). */
static void r_push(struct obuf *o, long long n) {
    ob_printf(o, "%c%lld\r\n", tl_resp3 ? '>' : '*', n);
}

static void r_null_array(struct obuf *o) {
    if (tl_resp3) {
        ob_add(o, "_\r\n", 3);
    } else {
        ob_add(o, "*-1\r\n", 5);
    }
}

/* A map of n pairs: RESP3 map, RESP2 array of 2n elements. */
static void r_map(struct obuf *o, long long n) {
    ob_printf(o, tl_resp3 ? "%%%lld\r\n" : "*%lld\r\n", tl_resp3 ? n : 2 * n);
}

/* Text for humans (INFO): RESP3 verbatim string of format "txt", RESP2 bulk string. */
static void r_verbatim(struct obuf *o, const void *p, size_t n) {
    if (tl_resp3) {
        ob_printf(o, "=%zu\r\ntxt:", n + 4);
        ob_add(o, p, n);
        ob_add(o, "\r\n", 2);
    } else {
        ob_printf(o, "$%zu\r\n", n);
        ob_add(o, p, n);
        ob_add(o, "\r\n", 2);
    }
}

static void r_array(struct obuf *o, long long n) {
    ob_printf(o, "*%lld\r\n", n);
}

static void r_arity(struct obuf *o, const struct arg *cmd) {
    ob_printf(o, "-ERR wrong number of arguments for '%.*s' command\r\n", (int)cmd->n, cmd->p);
}

static void r_store_error(struct obuf *o, int rc) {
    if (rc == -ENOMEM) {
        r_err(o, "OOM command not allowed when used memory > 'maxmemory'.");
    } else if (rc == -E2BIG) {
        r_err(o, "ERR string exceeds maximum allowed size");
    } else if (rc == -EINVAL) {
        r_err(o, "ERR invalid key");
    } else {
        r_err(o, "ERR storage failure");
    }
}

/* ---- argument helpers ------------------------------------------------------------------------------- */

static int is(const struct arg *a, const char *s) {
    size_t n = strlen(s);
    return a->n == n && strncasecmp(a->p, s, n) == 0;
}

/* Parses a signed 64-bit decimal integer (Redis string2ll rules: no spaces, no '+'). Returns 0 or -1. */
static int arg_ll(const struct arg *a, long long *v) {
    if (a->n == 0 || a->n > 20) {
        return -1;
    }
    size_t i = 0;
    int neg = 0;
    if (a->p[0] == '-') {
        neg = 1;
        i = 1;
        if (a->n == 1) {
            return -1;
        }
    }
    unsigned long long x = 0;
    for (; i < a->n; i++) {
        unsigned c = (unsigned char)a->p[i] - '0';
        if (c > 9 || x > (ULLONG_MAX - c) / 10) {
            return -1;
        }
        x = x * 10 + c;
    }
    if ((!neg && x > (unsigned long long)LLONG_MAX) || (neg && x > (unsigned long long)LLONG_MAX + 1)) {
        return -1;
    }
    *v = neg ? (long long)(0 - x) : (long long)x;
    return 0;
}

static long long now_unix_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Parses an expiry option pair (EX/PX/EXAT/PXAT value) at args[i]. Returns the TTL in milliseconds (> 0,
 * or -1 when the absolute time has passed), or 0 on a syntax error after replying.
 */
static int64_t parse_expiry(const struct arg *opt, const struct arg *val, struct obuf *o, int *bad) {
    long long v;
    *bad = 0;
    if (arg_ll(val, &v) != 0) {
        r_err(o, "ERR value is not an integer or out of range");
        *bad = 1;
        return 0;
    }
    if (v <= 0) {
        r_err(o, "ERR invalid expire time in 'set' command");
        *bad = 1;
        return 0;
    }
    int64_t ttl;
    if (is(opt, "EX")) {
        ttl = v > LLONG_MAX / 1000 ? -2 : v * 1000;
    } else if (is(opt, "PX")) {
        ttl = v;
    } else if (is(opt, "EXAT")) {
        ttl = v > LLONG_MAX / 1000 ? -2 : v * 1000 - now_unix_ms();
        if (ttl <= 0 && v > 0) {
            return -1;
        }
    } else {
        ttl = v - now_unix_ms();
        if (ttl <= 0 && v > 0) {
            return -1;
        }
    }
    if (ttl == -2) {
        r_err(o, "ERR invalid expire time in 'set' command");
        *bad = 1;
        return 0;
    }
    return ttl;
}

/* ---- commands --------------------------------------------------------------------------------------- */

static void get_reply(akvd *d, const struct arg *k, struct obuf *o, int64_t touch) {
    akv_meta m;
    const uint8_t *val;
    size_t vlen;
    SRV_COUNT(d, cmd_get, 1);
    if (srv_fetch(d, k->p, k->n, &m, touch, &val, &vlen) == 0) {
        SRV_COUNT(d, get_hits, 1);
        r_bulk(o, val, vlen);
    } else {
        SRV_COUNT(d, get_misses, 1);
        r_null(o);
    }
}

static void cmd_set(akvd *d, struct arg *a, size_t n, struct obuf *o) {
    if (n < 3) {
        r_arity(o, &a[0]);
        return;
    }
    akv_store_opts opt = {.mode = AKV_SET};
    int nx = 0;
    int xx = 0;
    int get = 0;
    int have_ttl = 0;
    for (size_t i = 3; i < n; i++) {
        if (is(&a[i], "NX")) {
            nx = 1;
        } else if (is(&a[i], "XX")) {
            xx = 1;
        } else if (is(&a[i], "GET")) {
            get = 1;
        } else if (is(&a[i], "KEEPTTL")) {
            opt.keep_ttl = 1;
        } else if ((is(&a[i], "EX") || is(&a[i], "PX") || is(&a[i], "EXAT") || is(&a[i], "PXAT")) &&
                   i + 1 < n && !have_ttl) {
            int bad;
            opt.ttl_ms = parse_expiry(&a[i], &a[i + 1], o, &bad);
            if (bad) {
                return;
            }
            have_ttl = 1;
            i++;
        } else {
            r_err(o, "ERR syntax error");
            return;
        }
    }
    if ((nx && xx) || (opt.keep_ttl && have_ttl)) {
        r_err(o, "ERR syntax error");
        return;
    }
    opt.mode = nx ? AKV_ADD : xx ? AKV_REPLACE : AKV_SET;
    if (get) {
        get_reply(d, &a[1], o, AKV_TTL_KEEP);
    }
    SRV_COUNT(d, cmd_set, 1);
    int rc = akv_store(akvd_store(d), a[1].p, a[1].n, a[2].p, a[2].n, &opt, NULL);
    if (get) {
        if (rc != 0 && rc != -EEXIST && rc != -ENOENT) {
            /* The GET reply is already queued; the store error cannot be reported in RESP2 any more. */
        }
        return;
    }
    if (rc == 0) {
        r_ok(o);
    } else if (rc == -EEXIST || rc == -ENOENT) {
        r_null(o);
    } else {
        r_store_error(o, rc);
    }
}

static void cmd_incr(akvd *d, const struct arg *k, long long delta, struct obuf *o) {
    int64_t result;
    SRV_COUNT(d, cmd_set, 1);
    int rc = akv_incr(akvd_store(d), k->p, k->n, delta, AKV_INCR_SIGNED, &result, NULL);
    if (rc == 0) {
        r_int(o, result);
    } else if (rc == -EINVAL) {
        r_err(o, "ERR value is not an integer or out of range");
    } else if (rc == -ERANGE) {
        r_err(o, "ERR increment or decrement would overflow");
    } else {
        r_store_error(o, rc);
    }
}

/* EXPIRE family: unit 1000 (seconds) or 1; absolute times when abs is set. */
static void cmd_expire(akvd *d, struct arg *a, size_t n, long long unit, int abs, struct obuf *o) {
    long long v;
    if (n != 3) {
        r_arity(o, &a[0]);
        return;
    }
    if (arg_ll(&a[2], &v) != 0 || (unit == 1000 && (v > LLONG_MAX / 1000 || v < LLONG_MIN / 1000))) {
        r_err(o, "ERR value is not an integer or out of range");
        return;
    }
    long long ttl = v * unit;
    if (abs && __builtin_sub_overflow(ttl, now_unix_ms(), &ttl)) {
        ttl = -1; /* far in the past: delete */
    }
    /* A non-positive time to live deletes the key (Redis semantics). */
    int rc = akv_touch(akvd_store(d), a[1].p, a[1].n, ttl > 0 ? ttl : -1);
    r_int(o, rc == 0 ? 1 : 0);
}

static void cmd_ttl(akvd *d, const struct arg *k, int ms, struct obuf *o) {
    akv_meta m;
    size_t vlen;
    uint8_t dummy;
    int rc = akv_get_meta(akvd_store(d), k->p, k->n, &dummy, 0, &vlen, &m, AKV_TTL_KEEP);
    if (rc == -ENOENT || rc == -EINVAL) {
        r_int(o, -2);
    } else if (m.ttl_ms < 0) {
        r_int(o, -1);
    } else {
        r_int(o, ms ? m.ttl_ms : (m.ttl_ms + 500) / 1000);
    }
}

struct key_collect {
    struct obuf *out;
    const char *pattern; /* NUL-terminated glob, NULL = all */
    long long count;
    struct obuf keys;
};

static int collect_key(void *arg, const akv_record *r) {
    struct key_collect *c = arg;
    if (c->pattern != NULL) {
        char buf[512];
        if (r->klen >= sizeof(buf)) {
            return 0; /* keys above 511 bytes are not matched by patterns */
        }
        memcpy(buf, r->key, r->klen);
        buf[r->klen] = '\0';
        if (fnmatch(c->pattern, buf, 0) != 0) {
            return 0;
        }
    }
    ob_printf(&c->keys, "$%zu\r\n", r->klen);
    ob_add(&c->keys, r->key, r->klen);
    ob_add(&c->keys, "\r\n", 2);
    c->count++;
    return 0;
}

static void cmd_keys(akvd *d, const struct arg *pattern, int scan, struct obuf *o) {
    struct key_collect c = {.out = o};
    char pat[512];
    if (pattern != NULL) {
        if (pattern->n >= sizeof(pat)) {
            r_err(o, "ERR pattern too long");
            return;
        }
        memcpy(pat, pattern->p, pattern->n);
        pat[pattern->n] = '\0';
        if (strcmp(pat, "*") != 0) {
            c.pattern = pat;
        }
    }
    akv_foreach(akvd_store(d), collect_key, &c);
    if (scan) {
        r_array(o, 2);
        r_bulk_str(o, "0");
    }
    r_array(o, c.count);
    ob_add(o, c.keys.p, c.keys.len);
    ob_free(&c.keys);
}

static void cmd_info(akvd *d, struct obuf *o) {
    akv_stats st;
    akv_stats_get(akvd_store(d), &st);
    struct srv_counters *c = akvd_counters(d);
    struct obuf b = {0};
    aof_info ai = {0};
    int aof_on = akvd_aof_info(d, &ai) == 0;
    akv_warm_info wi;
    akvd_warm_info(d, &wi);
    static const char *const warm_state[] = {"off", "created", "restored", "mismatch", "unclean", "damaged"};
#define LD(f) ((unsigned long long)__atomic_load_n(&c->f, __ATOMIC_RELAXED))
    ob_printf(&b,
              "# Server\r\narenakv_version:%s\r\nredis_mode:standalone\r\nprocess_id:%d\r\n"
              "uptime_in_seconds:%llu\r\nio_threads_active:%u\r\n\r\n"
              "# Clients\r\nconnected_clients:%u\r\n\r\n"
              "# Memory\r\nused_memory:%llu\r\nmaxmemory:%llu\r\nindex_memory:%llu\r\n\r\n"
              "# Persistence\r\nrdb_bgsave_in_progress:%d\r\nrdb_last_save_time:%llu\r\n"
              "rdb_last_bgsave_status:%s\r\naof_enabled:%d\r\naof_rewrite_in_progress:%d\r\n"
              "aof_last_bgrewrite_status:%s\r\naof_last_write_status:%s\r\naof_current_size:%llu\r\n"
              "aof_base_size:%llu\r\naof_buffer_length:%llu\r\naof_rewrites:%llu\r\naof_fsyncs:%llu\r\n"
              "warm_restart:%s\r\nwarm_restart_entries:%llu\r\nwarm_restart_rebuild_us:%llu\r\n\r\n"
              "# Stats\r\ntotal_connections_received:%llu\r\nrejected_connections:%llu\r\n"
              "total_commands_processed:%llu\r\nkeyspace_hits:%llu\r\nkeyspace_misses:%llu\r\n"
              "evicted_keys:%llu\r\nexpired_keys:%llu\r\npubsub_channels:%zu\r\npubsub_patterns:%zu\r\n\r\n"
              "# Keyspace\r\ndb0:keys=%llu\r\n",
              AKVD_VERSION, (int)getpid(), (unsigned long long)akvd_uptime(d), akvd_threads(d),
              __atomic_load_n(&c->curr_connections, __ATOMIC_RELAXED),
              (unsigned long long)(st.log_bytes + st.index_bytes), (unsigned long long)st.arena_bytes,
              (unsigned long long)st.index_bytes, akvd_bgsave_running(d),
              (unsigned long long)akvd_lastsave(d), akvd_last_save_ok(d) ? "ok" : "err", aof_on,
              ai.rewrite_running, !aof_on || ai.last_rewrite_ok ? "ok" : "err",
              !aof_on || ai.last_write_ok ? "ok" : "err", (unsigned long long)ai.current_size,
              (unsigned long long)ai.base_size, (unsigned long long)ai.pending,
              (unsigned long long)ai.rewrites, (unsigned long long)ai.fsyncs, warm_state[wi.state],
              (unsigned long long)wi.entries, (unsigned long long)wi.rebuild_us, LD(total_connections),
              LD(rejected_connections), LD(commands), LD(get_hits), LD(get_misses),
              (unsigned long long)(st.evictions + st.index_evictions), (unsigned long long)st.expired,
              pubsub_channels(akvd_pubsub(d), NULL, 0, NULL, NULL), pubsub_numpat(akvd_pubsub(d)),
              (unsigned long long)st.entries);
#undef LD
    r_verbatim(o, b.p, b.len);
    ob_free(&b);
}

/*
 * CONFIG GET: parameters with a meaning in arenakv, reported under their Redis names. maxmemory-policy
 * is "noeviction" with eviction disabled and "allkeys-lru" otherwise (the closest Redis policy; arenakv
 * evicts in write order with a second chance for entries read since their last write).
 */
static void cmd_config_get(akvd *d, const struct arg *pattern, struct obuf *o) {
    char pat[128];
    if (pattern->n >= sizeof(pat)) {
        r_array(o, 0);
        return;
    }
    memcpy(pat, pattern->p, pattern->n);
    pat[pattern->n] = '\0';
    akv_stats st;
    akv_stats_get(akvd_store(d), &st);
    char maxmem[32];
    char threads[16];
    snprintf(maxmem, sizeof(maxmem), "%llu", (unsigned long long)st.arena_bytes);
    snprintf(threads, sizeof(threads), "%u", akvd_threads(d));
    aof_info ai;
    const char *table[][2] = {
        {"maxmemory", maxmem},
        {"maxmemory-policy", akvd_no_evict(d) ? "noeviction" : "allkeys-lru"},
        {"save", ""},
        {"appendonly", akvd_aof_info(d, &ai) == 0 ? "yes" : "no"},
        {"appendfsync", akvd_appendfsync(d)},
        {"databases", "1"},
        {"io-threads", threads},
    };
    size_t count = 0;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        count += fnmatch(pat, table[i][0], FNM_CASEFOLD) == 0;
    }
    r_map(o, (long long)count);
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (fnmatch(pat, table[i][0], FNM_CASEFOLD) == 0) {
            r_bulk_str(o, table[i][0]);
            r_bulk_str(o, table[i][1]);
        }
    }
}

/* Executes one command. */
/* Lower-case copy of a command name for error messages (Valkey reports names in lower case). */
static void lower_name(const struct arg *c, char *out, size_t cap) {
    size_t n = c->n < cap - 1 ? c->n : cap - 1;
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)tolower((unsigned char)c->p[i]);
    }
    out[n] = '\0';
}

/*
 * Permission check of a RESP command for a user (NULL reason on success). Unknown commands pass (the
 * dispatcher answers them). Keys are checked at the positions of the command table; SET with the GET
 * option also reads its key.
 */
static const char *resp_denied(const acl_user *u, const struct arg *a, size_t n) {
    int id = acl_cmd_find(ACL_RESP, a[0].p, a[0].n);
    if (id < 0) {
        return NULL;
    }
    if (!acl_cmd_allowed(u, (unsigned)id)) {
        return "command";
    }
    const acl_cmd *k = acl_cmd_at((unsigned)id);
    if (k->first == 0) {
        return NULL;
    }
    unsigned access = k->access;
    if (is(&a[0], "SET")) {
        for (size_t i = 3; i < n; i++) {
            access |= is(&a[i], "GET") ? ACL_KEY_READ : 0;
        }
    }
    long last = k->last < 0 ? (long)n + k->last : k->last;
    for (long i = k->first; i <= last && (size_t)i < n; i += k->step) {
        if (!acl_key_allowed(u, a[i].p, a[i].n, access)) {
            return "key";
        }
    }
    return NULL;
}

/* Replies with the NOPERM error of Valkey for a denied command. Returns 1 when the command may run. */
static int resp_permitted(struct session *ss, const struct arg *a, size_t n, struct obuf *o) {
    if (ss->user == NULL || acl_unrestricted(ss->user)) {
        return 1;
    }
    const char *why = resp_denied(ss->user, a, n);
    if (why == NULL) {
        return 1;
    }
    if (why[0] == 'k') {
        r_err(o, "NOPERM No permissions to access a key");
    } else {
        char name[64];
        lower_name(&a[0], name, sizeof(name));
        ob_printf(o, "-NOPERM User %s has no permissions to run the '%s' command\r\n",
                  acl_user_name(ss->user), name);
    }
    return 0;
}

static void cmd_acl(akvd *d, struct session *ss, struct arg *a, size_t n, struct obuf *o);
static void dispatch(akvd *d, struct session *ss, struct arg *a, size_t n, struct obuf *o);

/* ---- transactions ------------------------------------------------------------------------------------ */

/* Largest queue of one transaction; beyond it the transaction is refused. */
#define TXN_MAX_BYTES (64u << 20)

/* Ends a transaction without running it: drops the queue and the watched keys. */
static void txn_reset(struct session *ss) {
    ss->in_multi = 0;
    ss->multi_error = 0;
    ss->nqueued = 0;
    ob_free(&ss->queued);
    ob_free(&ss->watched);
}

void resp_session_free(struct session *ss) {
    txn_reset(ss);
    ob_free(&ss->subs);
}

/* Queues a command of a transaction (a copy of its arguments) and answers QUEUED. */
static void queue_command(struct session *ss, const struct arg *a, size_t n, struct obuf *o) {
    if (acl_cmd_find(ACL_RESP, a[0].p, a[0].n) < 0) {
        ob_printf(o, "-ERR unknown command '%.*s'\r\n", a[0].n > 64 ? 64 : (int)a[0].n, a[0].p);
        ss->multi_error = 1;
        return;
    }
    size_t need = 4;
    for (size_t i = 0; i < n; i++) {
        need += 4 + a[i].n;
    }
    if (ss->queued.len + need > TXN_MAX_BYTES) {
        r_err(o, "ERR transaction too large");
        ss->multi_error = 1;
        return;
    }
    uint8_t *p = ob_reserve(&ss->queued, need);
    if (p == NULL) {
        r_err(o, "ERR out of memory");
        ss->multi_error = 1;
        return;
    }
    uint32_t cnt = (uint32_t)n;
    memcpy(p, &cnt, 4);
    p += 4;
    for (size_t i = 0; i < n; i++) {
        uint32_t len = (uint32_t)a[i].n;
        memcpy(p, &len, 4);
        memcpy(p + 4, a[i].p, a[i].n);
        p += 4 + a[i].n;
    }
    ob_commit(&ss->queued, need);
    ss->nqueued++;
    r_raw(o, "+QUEUED\r\n");
}

/* Commands without key positions that do not use the engine; others need every shard locked in EXEC. */
static int engine_free(const struct arg *c) {
    static const char *const names[] = {
        "PING", "ECHO",    "TIME",   "SELECT",   "CLIENT",   "COMMAND",   "HELLO",  "AUTH",
        "ACL",  "PUBLISH", "PUBSUB", "LASTSAVE", "READONLY", "READWRITE", "BGSAVE", "BGREWRITEAOF"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (is(c, names[i])) {
            return 1;
        }
    }
    return 0;
}

/* Decodes queued command `off` into args (capacity cap). Returns the argument count; *next the next offset. */
static size_t queued_args(const struct obuf *q, size_t off, struct arg *args, size_t cap, size_t *next) {
    uint32_t cnt;
    memcpy(&cnt, q->p + off, 4);
    off += 4;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t len;
        memcpy(&len, q->p + off, 4);
        if (i < cap) {
            args[i] = (struct arg){.p = (const char *)q->p + off + 4, .n = len};
        }
        off += 4 + len;
    }
    *next = off;
    return cnt;
}

static void cmd_exec(akvd *d, struct session *ss, struct obuf *o) {
    if (!ss->in_multi) {
        r_err(o, "ERR EXEC without MULTI");
        return;
    }
    if (ss->multi_error) {
        txn_reset(ss);
        r_err(o, "EXECABORT Transaction discarded because of previous errors.");
        return;
    }
    akv *s = akvd_store(d);
    /* Keys of all queued commands and of the watched keys. */
    size_t cap = 64;
    size_t nk = 0;
    const void **keys = malloc(cap * sizeof(*keys));
    size_t *lens = malloc(cap * sizeof(*lens));
    struct arg *args = NULL;
    size_t args_cap = 0;
    int all = 0;
    int oom = keys == NULL || lens == NULL;
    for (size_t off = 0; off < ss->queued.len && !oom;) {
        uint32_t cnt;
        memcpy(&cnt, ss->queued.p + off, 4);
        if (cnt > args_cap) {
            struct arg *na = realloc(args, cnt * sizeof(*na));
            if (na == NULL) {
                oom = 1;
                break;
            }
            memset(na, 0, cnt * sizeof(*na)); /* queued_args() fills it; zeroed so every element is defined */
            args = na;
            args_cap = cnt;
        }
        size_t n = queued_args(&ss->queued, off, args, args_cap, &off);
        if (n == 0 || args == NULL) {
            continue; /* queue_command() never stores a command without arguments */
        }
        int id = acl_cmd_find(ACL_RESP, args[0].p, args[0].n);
        const acl_cmd *k = id >= 0 ? acl_cmd_at((unsigned)id) : NULL;
        if (k == NULL || k->first == 0) {
            all |= !engine_free(&args[0]);
            continue;
        }
        long last = k->last < 0 ? (long)n + k->last : k->last;
        for (long i = k->first; i <= last && (size_t)i < n && (size_t)i < args_cap && !oom; i += k->step) {
            if (nk == cap) {
                cap *= 2;
                const void **nkeys = realloc(keys, cap * sizeof(*keys));
                size_t *nlens = nkeys != NULL ? realloc(lens, cap * sizeof(*lens)) : NULL;
                if (nkeys != NULL) {
                    keys = nkeys;
                }
                if (nlens == NULL) {
                    oom = 1;
                    break;
                }
                lens = nlens;
            }
            keys[nk] = args[i].p;
            lens[nk++] = args[i].n;
        }
    }
    for (size_t off = 0; off < ss->watched.len && !oom;) {
        size_t klen = (size_t)(ss->watched.p[off + 8] | (ss->watched.p[off + 9] << 8));
        if (nk == cap) {
            cap *= 2;
            const void **nkeys = realloc(keys, cap * sizeof(*keys));
            size_t *nlens = nkeys != NULL ? realloc(lens, cap * sizeof(*lens)) : NULL;
            if (nkeys != NULL) {
                keys = nkeys;
            }
            if (nlens == NULL) {
                oom = 1;
                break;
            }
            lens = nlens;
        }
        keys[nk] = ss->watched.p + off + 10;
        lens[nk++] = klen;
        off += 10 + klen;
    }
    if (oom) {
        free(keys);
        free(lens);
        free(args);
        txn_reset(ss);
        r_err(o, "EXECABORT out of memory");
        return;
    }
    uint64_t txn = akvd_next_txn(d);
    akv_lockset set;
    if (all) {
        akv_lock_all(s, txn, &set);
    } else {
        akv_lock_keys(s, keys, lens, nk, txn, &set);
    }
    int changed = 0;
    for (size_t off = 0; off < ss->watched.len && !changed;) {
        uint64_t cas;
        memcpy(&cas, ss->watched.p + off, 8);
        size_t klen = (size_t)(ss->watched.p[off + 8] | (ss->watched.p[off + 9] << 8));
        changed = akv_cas_of(s, ss->watched.p + off + 10, klen) != cas;
        off += 10 + klen;
    }
    if (changed) {
        akv_unlock(s, &set);
        r_null_array(o);
    } else {
        unsigned nq = ss->nqueued;
        ss->in_multi = 0; /* the queued commands run as commands */
        tl_locked = 1;
        r_array(o, nq);
        for (size_t off = 0; off < ss->queued.len;) {
            uint32_t cnt;
            memcpy(&cnt, ss->queued.p + off, 4);
            if (cnt > args_cap) {
                struct arg *na = realloc(args, cnt * sizeof(*na));
                if (na == NULL) {
                    break; /* sized for the largest command above */
                }
                args = na;
                args_cap = cnt;
            }
            size_t n = queued_args(&ss->queued, off, args, args_cap, &off);
            dispatch(d, ss, args, n, o);
        }
        akvd_txn_end(d, txn);
        tl_locked = 0;
        akv_unlock(s, &set);
    }
    free(keys);
    free(lens);
    free(args);
    txn_reset(ss);
}

/* ---- pub/sub ------------------------------------------------------------------------------------------ */

/* Replies to (P)SUBSCRIBE and (P)UNSUBSCRIBE: one frame per channel with the session's subscription count. */
static void sub_reply(struct obuf *o, const char *kind, const char *name, size_t n, unsigned count) {
    r_push(o, 3);
    r_bulk_str(o, kind);
    if (name != NULL) {
        r_bulk(o, name, n);
    } else {
        r_null(o);
    }
    r_int(o, count);
}

static void cmd_subscribe(akvd *d, struct session *ss, struct arg *a, size_t n, int pattern, struct obuf *o) {
    if (n < 2) {
        r_arity(o, &a[0]);
        return;
    }
    for (size_t i = 1; i < n; i++) {
        if (ss->user != NULL && !acl_unrestricted(ss->user) &&
            !(pattern ? acl_channel_pattern_allowed(ss->user, a[i].p, a[i].n)
                      : acl_channel_allowed(ss->user, a[i].p, a[i].n))) {
            r_err(o, "NOPERM No permissions to access a channel");
            return;
        }
    }
    for (size_t i = 1; i < n; i++) {
        int r = akvd_subscribe(d, ss, a[i].p, a[i].n, pattern);
        if (r < 0) {
            r_err(o, r == -ENOTSUP ? "ERR pub/sub needs a connection" : "ERR out of memory");
            return;
        }
        sub_reply(o, pattern ? "psubscribe" : "subscribe", a[i].p, a[i].n, ss->nsubs);
    }
}

static void cmd_unsubscribe(akvd *d, struct session *ss, struct arg *a, size_t n, int pattern,
                            struct obuf *o) {
    const char *kind = pattern ? "punsubscribe" : "unsubscribe";
    if (n >= 2) {
        for (size_t i = 1; i < n; i++) {
            akvd_unsubscribe(d, ss, a[i].p, a[i].n, pattern);
            sub_reply(o, kind, a[i].p, a[i].n, ss->nsubs);
        }
        return;
    }
    /* All channels (or patterns) of the session; one frame with a null name when there are none. */
    unsigned sent = 0;
    for (;;) {
        size_t off = 0;
        const char *name = NULL;
        uint32_t len = 0;
        while (off < ss->subs.len) {
            memcpy(&len, ss->subs.p + off + 1, 4);
            if (ss->subs.p[off] == (uint8_t)pattern) {
                name = (const char *)ss->subs.p + off + 5;
                break;
            }
            off += 5 + len;
        }
        if (name == NULL) {
            break;
        }
        char *copy = malloc(len > 0 ? len : 1);
        if (copy == NULL) {
            r_err(o, "ERR out of memory");
            return;
        }
        memcpy(copy, name, len);
        akvd_unsubscribe(d, ss, name, len, pattern);
        sub_reply(o, kind, copy, len, ss->nsubs);
        free(copy);
        sent++;
    }
    if (sent == 0) {
        sub_reply(o, kind, NULL, 0, ss->nsubs);
    }
}

struct chan_list {
    struct obuf b;
    unsigned n;
};

static int add_channel(void *arg, const char *ch, size_t n) {
    struct chan_list *l = arg;
    r_bulk(&l->b, ch, n);
    l->n++;
    return 0;
}

static void cmd_pubsub(akvd *d, struct arg *a, size_t n, struct obuf *o) {
    pubsub *ps = akvd_pubsub(d);
    if (n >= 2 && is(&a[1], "CHANNELS") && n <= 3) {
        struct chan_list l = {0};
        pubsub_channels(ps, n == 3 ? a[2].p : NULL, n == 3 ? a[2].n : 0, add_channel, &l);
        r_array(o, l.n);
        ob_add(o, l.b.p, l.b.len);
        ob_free(&l.b);
    } else if (n >= 2 && is(&a[1], "NUMSUB")) {
        r_map(o, (long long)n - 2);
        for (size_t i = 2; i < n; i++) {
            r_bulk(o, a[i].p, a[i].n);
            r_int(o, (long long)pubsub_numsub(ps, a[i].p, a[i].n));
        }
    } else if (n == 2 && is(&a[1], "NUMPAT")) {
        r_int(o, (long long)pubsub_numpat(ps));
    } else {
        r_err(o, "ERR unknown subcommand or wrong number of arguments for 'pubsub'");
    }
}

/* RESP2 subscriber mode: the commands a session with subscriptions may still send. */
static int allowed_while_subscribed(const struct arg *c) {
    return is(c, "SUBSCRIBE") || is(c, "PSUBSCRIBE") || is(c, "UNSUBSCRIBE") || is(c, "PUNSUBSCRIBE") ||
           is(c, "PING") || is(c, "QUIT") || is(c, "RESET");
}

/* Commands made of several engine operations on their keys (see the file comment). */
static int composite(const struct arg *a, size_t n) {
    const struct arg *c = &a[0];
    if (n < 3) {
        return 0; /* no key and value: the command reports its arity error */
    }
    if (is(c, "GETSET") || is(c, "GETDEL") || is(c, "GETEX") || is(c, "APPEND") || (is(c, "MSET") && n > 3)) {
        return 1;
    }
    if (is(c, "SET")) {
        for (size_t i = 3; i < n; i++) {
            if (is(&a[i], "GET")) {
                return 1;
            }
        }
    }
    return 0;
}

/* Runs a composite command with the shards of its keys locked and its change records grouped. */
static void run_locked(akvd *d, struct session *ss, struct arg *a, size_t n, struct obuf *o) {
    akv *s = akvd_store(d);
    const void *keys[64];
    size_t lens[64];
    size_t nk = 0;
    int all = 0;
    if (is(&a[0], "MSET")) {
        for (size_t i = 1; i < n; i += 2) {
            if (nk == 64) {
                all = 1; /* many keys: every shard */
                break;
            }
            keys[nk] = a[i].p;
            lens[nk++] = a[i].n;
        }
    } else {
        keys[nk] = a[1].p;
        lens[nk++] = a[1].n;
    }
    uint64_t txn = akvd_next_txn(d);
    akv_lockset set;
    if (all) {
        akv_lock_all(s, txn, &set);
    } else {
        akv_lock_keys(s, keys, lens, nk, txn, &set);
    }
    tl_locked = 1;
    dispatch(d, ss, a, n, o);
    tl_locked = 0;
    akvd_txn_end(d, txn);
    akv_unlock(s, &set);
}

static void dispatch(akvd *d, struct session *ss, struct arg *a, size_t n, struct obuf *o) {
    akv *s = akvd_store(d);
    const struct arg *c = &a[0];
    SRV_COUNT(d, commands, 1);
    if (!ss->authed && !is(c, "AUTH") && !is(c, "HELLO") && !is(c, "QUIT")) {
        r_err(o, "NOAUTH Authentication required.");
        return;
    }
    if (akvd_session_refresh(d, ss) != 0) {
        r_err(o, "ERR the user of this connection was removed or disabled");
        return;
    }
    if (ss->nsubs > 0 && ss->proto != 3 && !allowed_while_subscribed(c)) {
        char name[64];
        lower_name(c, name, sizeof(name));
        ob_printf(o,
                  "-ERR Can't execute '%s': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are "
                  "allowed in this context\r\n",
                  name);
        return;
    }
    if (ss->nsubs > 0 && ss->proto != 3 && is(c, "PING")) {
        r_array(o, 2);
        r_bulk_str(o, "pong");
        r_bulk(o, n >= 2 ? a[1].p : "", n >= 2 ? a[1].n : 0);
        return;
    }
    int in_queue = ss->in_multi && !is(c, "EXEC") && !is(c, "DISCARD") && !is(c, "MULTI") &&
                   !is(c, "WATCH") && !is(c, "QUIT") && !is(c, "RESET");
    if (!resp_permitted(ss, a, n, o)) {
        ss->multi_error |= in_queue;
        return;
    }
    if (in_queue) {
        queue_command(ss, a, n, o);
        return;
    }
    if (!tl_locked && composite(a, n)) {
        run_locked(d, ss, a, n, o);
        return;
    }
    if (is(c, "GET")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        get_reply(d, &a[1], o, AKV_TTL_KEEP);
    } else if (is(c, "SET")) {
        cmd_set(d, a, n, o);
    } else if (is(c, "MGET")) {
        if (n < 2) {
            r_arity(o, c);
            return;
        }
        r_array(o, (long long)n - 1);
        for (size_t i = 1; i < n; i++) {
            get_reply(d, &a[i], o, AKV_TTL_KEEP);
        }
    } else if (is(c, "MSET")) {
        if (n < 3 || (n - 1) % 2 != 0) {
            r_arity(o, c);
            return;
        }
        int rc = 0;
        for (size_t i = 1; i + 1 < n && rc == 0; i += 2) {
            SRV_COUNT(d, cmd_set, 1);
            rc = akv_store(s, a[i].p, a[i].n, a[i + 1].p, a[i + 1].n, NULL, NULL);
        }
        if (rc == 0) {
            r_ok(o);
        } else {
            r_store_error(o, rc);
        }
    } else if (is(c, "DEL") || is(c, "UNLINK")) {
        if (n < 2) {
            r_arity(o, c);
            return;
        }
        long long k = 0;
        for (size_t i = 1; i < n; i++) {
            k += akv_del(s, a[i].p, a[i].n) == 0;
        }
        r_int(o, k);
    } else if (is(c, "EXISTS")) {
        if (n < 2) {
            r_arity(o, c);
            return;
        }
        long long k = 0;
        for (size_t i = 1; i < n; i++) {
            k += akv_exists(s, a[i].p, a[i].n);
        }
        r_int(o, k);
    } else if (is(c, "PING")) {
        if (n == 1) {
            r_raw(o, "+PONG\r\n");
        } else if (n == 2) {
            r_bulk(o, a[1].p, a[1].n);
        } else {
            r_arity(o, c);
        }
    } else if (is(c, "INCR") || is(c, "DECR")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        cmd_incr(d, &a[1], is(c, "INCR") ? 1 : -1, o);
    } else if (is(c, "INCRBY") || is(c, "DECRBY")) {
        long long v;
        if (n != 3) {
            r_arity(o, c);
            return;
        }
        if (arg_ll(&a[2], &v) != 0 || (is(c, "DECRBY") && v == LLONG_MIN)) {
            r_err(o, "ERR value is not an integer or out of range");
            return;
        }
        cmd_incr(d, &a[1], is(c, "INCRBY") ? v : -v, o);
    } else if (is(c, "SETNX")) {
        if (n != 3) {
            r_arity(o, c);
            return;
        }
        akv_store_opts opt = {.mode = AKV_ADD};
        SRV_COUNT(d, cmd_set, 1);
        int rc = akv_store(s, a[1].p, a[1].n, a[2].p, a[2].n, &opt, NULL);
        if (rc == 0 || rc == -EEXIST) {
            r_int(o, rc == 0);
        } else {
            r_store_error(o, rc);
        }
    } else if (is(c, "SETEX") || is(c, "PSETEX")) {
        long long v;
        if (n != 4) {
            r_arity(o, c);
            return;
        }
        if (arg_ll(&a[2], &v) != 0 || v <= 0 || (is(c, "SETEX") && v > LLONG_MAX / 1000)) {
            ob_printf(o, "-ERR invalid expire time in '%.*s' command\r\n", (int)c->n, c->p);
            return;
        }
        akv_store_opts opt = {.ttl_ms = is(c, "SETEX") ? v * 1000 : v};
        SRV_COUNT(d, cmd_set, 1);
        int rc = akv_store(s, a[1].p, a[1].n, a[3].p, a[3].n, &opt, NULL);
        if (rc == 0) {
            r_ok(o);
        } else {
            r_store_error(o, rc);
        }
    } else if (is(c, "GETSET")) {
        if (n != 3) {
            r_arity(o, c);
            return;
        }
        get_reply(d, &a[1], o, AKV_TTL_KEEP);
        akv_store(s, a[1].p, a[1].n, a[2].p, a[2].n, NULL, NULL);
    } else if (is(c, "GETDEL")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        get_reply(d, &a[1], o, AKV_TTL_KEEP);
        akv_del(s, a[1].p, a[1].n);
    } else if (is(c, "GETEX")) {
        if (n < 2) {
            r_arity(o, c);
            return;
        }
        int64_t touch = AKV_TTL_KEEP;
        if (n == 3 && is(&a[2], "PERSIST")) {
            touch = 0;
        } else if (n == 4) {
            int bad;
            touch = parse_expiry(&a[2], &a[3], o, &bad);
            if (bad) {
                return;
            }
        } else if (n != 2) {
            r_err(o, "ERR syntax error");
            return;
        }
        get_reply(d, &a[1], o, touch);
    } else if (is(c, "APPEND")) {
        if (n != 3) {
            r_arity(o, c);
            return;
        }
        akv_store_opts app = {.mode = AKV_APPEND};
        akv_store_opts add = {.mode = AKV_ADD};
        SRV_COUNT(d, cmd_set, 1);
        int rc = akv_store(s, a[1].p, a[1].n, a[2].p, a[2].n, &app, NULL);
        for (int tries = 0; rc == -ENOENT && tries < 8; tries++) {
            rc = akv_store(s, a[1].p, a[1].n, a[2].p, a[2].n, &add, NULL);
            if (rc == -EEXIST) {
                rc = akv_store(s, a[1].p, a[1].n, a[2].p, a[2].n, &app, NULL);
            }
        }
        if (rc != 0) {
            r_store_error(o, rc);
            return;
        }
        size_t vlen = 0;
        uint8_t dummy;
        akv_get_meta(s, a[1].p, a[1].n, &dummy, 0, &vlen, NULL, AKV_TTL_KEEP);
        r_int(o, (long long)vlen);
    } else if (is(c, "STRLEN")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        size_t vlen = 0;
        uint8_t dummy;
        int rc = akv_get_meta(s, a[1].p, a[1].n, &dummy, 0, &vlen, NULL, AKV_TTL_KEEP);
        r_int(o, rc == -ENOENT || rc == -EINVAL ? 0 : (long long)vlen);
    } else if (is(c, "TYPE")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        r_raw(o, akv_exists(s, a[1].p, a[1].n) ? "+string\r\n" : "+none\r\n");
    } else if (is(c, "EXPIRE")) {
        cmd_expire(d, a, n, 1000, 0, o);
    } else if (is(c, "PEXPIRE")) {
        cmd_expire(d, a, n, 1, 0, o);
    } else if (is(c, "EXPIREAT")) {
        cmd_expire(d, a, n, 1000, 1, o);
    } else if (is(c, "PEXPIREAT")) {
        cmd_expire(d, a, n, 1, 1, o);
    } else if (is(c, "TTL") || is(c, "PTTL")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        cmd_ttl(d, &a[1], is(c, "PTTL"), o);
    } else if (is(c, "PERSIST")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        akv_meta m;
        size_t vlen;
        uint8_t dummy;
        int rc = akv_get_meta(s, a[1].p, a[1].n, &dummy, 0, &vlen, &m, AKV_TTL_KEEP);
        if ((rc == 0 || rc == -EMSGSIZE) && m.ttl_ms >= 0 && akv_touch(s, a[1].p, a[1].n, 0) == 0) {
            r_int(o, 1);
        } else {
            r_int(o, 0);
        }
    } else if (is(c, "DBSIZE")) {
        akv_stats st;
        akv_stats_get(s, &st);
        r_int(o, (long long)st.entries);
    } else if (is(c, "FLUSHALL") || is(c, "FLUSHDB")) {
        akv_flush(s);
        SRV_COUNT(d, cmd_flush, 1);
        r_ok(o);
    } else if (is(c, "KEYS")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        cmd_keys(d, &a[1], 0, o);
    } else if (is(c, "SCAN")) {
        long long cursor;
        if (n < 2 || arg_ll(&a[1], &cursor) != 0) {
            r_err(o, "ERR invalid cursor");
            return;
        }
        const struct arg *match = NULL;
        for (size_t i = 2; i + 1 < n; i += 2) {
            if (is(&a[i], "MATCH")) {
                match = &a[i + 1];
            } else if (!is(&a[i], "COUNT") && !is(&a[i], "TYPE")) {
                r_err(o, "ERR syntax error");
                return;
            }
        }
        if (cursor != 0) {
            r_array(o, 2);
            r_bulk_str(o, "0");
            r_array(o, 0);
            return;
        }
        cmd_keys(d, match, 1, o);
    } else if (is(c, "ECHO")) {
        if (n != 2) {
            r_arity(o, c);
            return;
        }
        r_bulk(o, a[1].p, a[1].n);
    } else if (is(c, "AUTH")) {
        if (n != 2 && n != 3) {
            r_arity(o, c);
            return;
        }
        if (!akvd_acl_enabled(d)) {
            r_err(o, "ERR AUTH <password> called without any password configured for the default user. "
                     "Are you sure your configuration is correct?");
            return;
        }
        const struct arg *user = n == 3 ? &a[1] : NULL;
        const struct arg *pass = &a[n - 1];
        if (akvd_login(d, ss, user ? user->p : NULL, user ? user->n : 0, pass->p, pass->n)) {
            r_ok(o);
        } else {
            r_err(o, "WRONGPASS invalid username-password pair or user is disabled.");
        }
    } else if (is(c, "HELLO")) {
        long long ver = ss->proto == 3 ? 3 : 2;
        if (n >= 2 && (arg_ll(&a[1], &ver) != 0 || (ver != 2 && ver != 3))) {
            r_err(o, "NOPROTO unsupported protocol version");
            return;
        }
        for (size_t i = 2; i < n; i++) {
            if (is(&a[i], "AUTH") && i + 2 < n) {
                if (akvd_acl_enabled(d) &&
                    !akvd_login(d, ss, a[i + 1].p, a[i + 1].n, a[i + 2].p, a[i + 2].n)) {
                    r_err(o, "WRONGPASS invalid username-password pair or user is disabled.");
                    return;
                }
                ss->authed = 1;
                i += 2;
            } else if (is(&a[i], "SETNAME") && i + 1 < n) {
                size_t k = a[i + 1].n < sizeof(ss->name) - 1 ? a[i + 1].n : sizeof(ss->name) - 1;
                memcpy(ss->name, a[i + 1].p, k);
                ss->name[k] = '\0';
                i++;
            } else {
                r_err(o, "ERR syntax error");
                return;
            }
        }
        if (!ss->authed) {
            r_err(o,
                  "NOAUTH HELLO must be called with the client already authenticated, otherwise the HELLO "
                  "<proto> AUTH <user> <pass> option can be used to authenticate the client and select the "
                  "RESP protocol version at the same time");
            return;
        }
        ss->proto = (int)ver;
        tl_resp3 = ver == 3;
        r_map(o, 7);
        r_bulk_str(o, "server");
        r_bulk_str(o, "arenakv");
        r_bulk_str(o, "version");
        r_bulk_str(o, AKVD_VERSION);
        r_bulk_str(o, "proto");
        r_int(o, ver);
        r_bulk_str(o, "id");
        r_int(o, (long long)ss->id);
        r_bulk_str(o, "mode");
        r_bulk_str(o, "standalone");
        r_bulk_str(o, "role");
        r_bulk_str(o, "master");
        r_bulk_str(o, "modules");
        r_array(o, 0);
    } else if (is(c, "QUIT")) {
        r_ok(o);
        ss->close = 1;
    } else if (is(c, "SELECT")) {
        long long db;
        if (n != 2) {
            r_arity(o, c);
        } else if (arg_ll(&a[1], &db) != 0) {
            r_err(o, "ERR value is not an integer or out of range");
        } else if (db != 0) {
            r_err(o, "ERR DB index is out of range");
        } else {
            r_ok(o);
        }
    } else if (is(c, "CLIENT")) {
        if (n >= 3 && is(&a[1], "SETNAME")) {
            size_t k = a[2].n < sizeof(ss->name) - 1 ? a[2].n : sizeof(ss->name) - 1;
            memcpy(ss->name, a[2].p, k);
            ss->name[k] = '\0';
            r_ok(o);
        } else if (n == 2 && is(&a[1], "GETNAME")) {
            if (ss->name[0] != '\0') {
                r_bulk_str(o, ss->name);
            } else {
                r_null(o);
            }
        } else if (n == 2 && is(&a[1], "ID")) {
            r_int(o, (long long)ss->id);
        } else if (n >= 2 && is(&a[1], "SETINFO")) {
            r_ok(o);
        } else {
            r_err(o, "ERR unknown subcommand");
        }
    } else if (is(c, "COMMAND")) {
        if (n >= 2 && is(&a[1], "COUNT")) {
            r_int(o, 0);
        } else {
            r_array(o, 0);
        }
    } else if (is(c, "CONFIG")) {
        if (n >= 3 && is(&a[1], "GET")) {
            cmd_config_get(d, &a[2], o);
        } else if (n >= 2 && is(&a[1], "GET")) {
            r_arity(o, c);
        } else {
            r_err(o, "ERR CONFIG SET is not supported; use the server options");
        }
    } else if (is(c, "INFO")) {
        cmd_info(d, o);
    } else if (is(c, "SAVE")) {
        int rc = akvd_save(d);
        if (rc == 0) {
            r_ok(o);
        } else if (rc == -EINVAL) {
            r_err(o, "ERR persistence is not configured (snapshot option)");
        } else {
            r_err(o, "ERR snapshot failed");
        }
    } else if (is(c, "BGSAVE")) {
        int rc = akvd_bgsave(d);
        if (rc == 0) {
            r_raw(o, "+Background saving started\r\n");
        } else if (rc == -EBUSY) {
            r_err(o, "ERR Background save already in progress");
        } else {
            r_err(o, "ERR persistence is not configured (snapshot option)");
        }
    } else if (is(c, "ACL")) {
        cmd_acl(d, ss, a, n, o);
    } else if (is(c, "SUBSCRIBE") || is(c, "PSUBSCRIBE")) {
        cmd_subscribe(d, ss, a, n, is(c, "PSUBSCRIBE"), o);
    } else if (is(c, "UNSUBSCRIBE") || is(c, "PUNSUBSCRIBE")) {
        cmd_unsubscribe(d, ss, a, n, is(c, "PUNSUBSCRIBE"), o);
    } else if (is(c, "PUBLISH")) {
        if (n != 3) {
            r_arity(o, c);
        } else if (ss->user != NULL && !acl_unrestricted(ss->user) &&
                   !acl_channel_allowed(ss->user, a[1].p, a[1].n)) {
            r_err(o, "NOPERM No permissions to access a channel");
        } else {
            r_int(o, (long long)akvd_publish(d, a[1].p, a[1].n, a[2].p, a[2].n));
        }
    } else if (is(c, "PUBSUB")) {
        cmd_pubsub(d, a, n, o);
    } else if (is(c, "MULTI")) {
        if (n != 1) {
            r_arity(o, c);
        } else if (ss->in_multi) {
            r_err(o, "ERR MULTI calls can not be nested");
        } else {
            ss->in_multi = 1;
            ss->multi_error = 0;
            ss->queued.len = ss->queued.off = 0;
            ss->nqueued = 0;
            r_ok(o);
        }
    } else if (is(c, "EXEC")) {
        cmd_exec(d, ss, o);
    } else if (is(c, "DISCARD")) {
        if (!ss->in_multi) {
            r_err(o, "ERR DISCARD without MULTI");
        } else {
            txn_reset(ss);
            r_ok(o);
        }
    } else if (is(c, "WATCH")) {
        if (ss->in_multi) {
            r_err(o, "ERR WATCH inside MULTI is not allowed");
        } else if (n < 2) {
            r_arity(o, c);
        } else {
            for (size_t i = 1; i < n; i++) {
                uint8_t h[10];
                uint64_t cas = akv_cas_of(s, a[i].p, a[i].n);
                memcpy(h, &cas, 8);
                h[8] = (uint8_t)a[i].n;
                h[9] = (uint8_t)(a[i].n >> 8);
                if (a[i].n > 0xffff || ob_add(&ss->watched, h, 10) != 0 ||
                    ob_add(&ss->watched, a[i].p, a[i].n) != 0) {
                    r_err(o, "ERR invalid key or out of memory");
                    return;
                }
            }
            r_ok(o);
        }
    } else if (is(c, "UNWATCH")) {
        ss->watched.len = ss->watched.off = 0;
        r_ok(o);
    } else if (is(c, "RESET")) {
        txn_reset(ss);
        if (ss->nsubs > 0) {
            akvd_unsubscribe_all(d, ss);
        }
        ss->proto = 2;
        tl_resp3 = 0;
        ss->name[0] = '\0';
        akvd_session_init(d, ss);
        r_raw(o, "+RESET\r\n");
    } else if (is(c, "BGREWRITEAOF")) {
        int rc = akvd_aof_rewrite(d);
        if (rc == 0) {
            r_raw(o, "+Background append only file rewriting started\r\n");
        } else if (rc == -EBUSY) {
            r_err(o, "ERR Background append only file rewriting already in progress");
        } else {
            r_err(o, "ERR the append-only log is not configured (appendonly option)");
        }
    } else if (is(c, "LASTSAVE")) {
        r_int(o, (long long)akvd_lastsave(d));
    } else if (is(c, "TIME")) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        char sec[24];
        char usec[24];
        snprintf(sec, sizeof(sec), "%lld", (long long)tv.tv_sec);
        snprintf(usec, sizeof(usec), "%lld", (long long)tv.tv_usec);
        r_array(o, 2);
        r_bulk_str(o, sec);
        r_bulk_str(o, usec);
    } else {
        ob_printf(o, "-ERR unknown command '%.*s'\r\n", c->n > 64 ? 64 : (int)c->n, c->p);
    }
}

/* ---- framing ---------------------------------------------------------------------------------------- */

/* Parses "<prefix><decimal>\r\n" at p. Returns the bytes used, 0 when incomplete, -1 on a format error. */
static ssize_t parse_len_line(const uint8_t *p, size_t left, char prefix, long long *v) {
    if (left < 1) {
        return 0;
    }
    if (p[0] != (uint8_t)prefix) {
        return -1;
    }
    const uint8_t *nl = memchr(p, '\n', left < 32 ? left : 32);
    if (nl == NULL) {
        return left >= 32 ? -1 : 0;
    }
    size_t n = (size_t)(nl - p);
    if (n < 2 || p[n - 1] != '\r') {
        return -1;
    }
    struct arg a = {(const char *)p + 1, n - 2};
    if (arg_ll(&a, v) != 0) {
        return -1;
    }
    return (ssize_t)n + 1;
}

/* Parses one inline command (a line without '*'). Returns bytes used, 0 when incomplete, -1 on error. */
static ssize_t parse_inline(const uint8_t *p, size_t left, size_t *nargs) {
    const uint8_t *nl = memchr(p, '\n', left);
    if (nl == NULL) {
        return left > RESP_MAX_INLINE ? -1 : 0;
    }
    size_t n = (size_t)(nl - p);
    size_t end = n > 0 && p[n - 1] == '\r' ? n - 1 : n;
    size_t k = 0;
    size_t i = 0;
    while (i < end) {
        while (i < end && (p[i] == ' ' || p[i] == '\t')) {
            i++;
        }
        if (i == end) {
            break;
        }
        size_t start = i;
        while (i < end && p[i] != ' ' && p[i] != '\t') {
            i++;
        }
        if (args_reserve(k + 1) != 0) {
            return -1;
        }
        tl_args[k].p = (const char *)p + start;
        tl_args[k].n = i - start;
        k++;
    }
    *nargs = k;
    return (ssize_t)n + 1;
}

/* Parses one array command. Returns bytes used, 0 when incomplete, -1 on error. */
static ssize_t parse_array(akvd *d, const uint8_t *p, size_t left, size_t *nargs) {
    long long count;
    ssize_t used = parse_len_line(p, left, '*', &count);
    if (used <= 0) {
        return used;
    }
    if (count < 0 || (unsigned long long)count > RESP_MAX_ARGS) {
        return -1;
    }
    if (args_reserve((size_t)count) != 0) {
        return -1;
    }
    size_t max_bulk = akvd_max_input(d);
    for (long long i = 0; i < count; i++) {
        long long blen;
        ssize_t h = parse_len_line(p + used, left - (size_t)used, '$', &blen);
        if (h <= 0) {
            return h;
        }
        if (blen < 0 || (unsigned long long)blen > max_bulk) {
            return -1;
        }
        size_t need = (size_t)used + (size_t)h + (size_t)blen + 2;
        if (need > left) {
            return 0;
        }
        const uint8_t *data = p + used + h;
        if (data[blen] != '\r' || data[blen + 1] != '\n') {
            return -1;
        }
        tl_args[i].p = (const char *)data;
        tl_args[i].n = (size_t)blen;
        used = (ssize_t)need;
    }
    *nargs = (size_t)count;
    return used;
}

size_t resp_execute(akvd *d, struct session *ss, const uint8_t *in, size_t len, struct obuf *out) {
    size_t used = 0;
    tl_resp3 = ss->proto == 3;
    while (used < len && !ss->close && ob_pending(out) <= OB_SOFT_LIMIT) {
        size_t nargs = 0;
        ssize_t n = in[used] == '*' ? parse_array(d, in + used, len - used, &nargs)
                                    : parse_inline(in + used, len - used, &nargs);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            r_err(out, "ERR Protocol error: invalid request");
            ss->close = 1;
            return len;
        }
        used += (size_t)n;
        if (nargs == 0) {
            continue; /* empty line or empty array */
        }
        dispatch(d, ss, tl_args, nargs, out);
    }
    return used;
}

/* ---- ACL ---------------------------------------------------------------------------------------------- */

static void acl_getuser(const acl_user *u, struct obuf *o) {
    char desc[8192];
    acl_describe(u, desc, sizeof(desc));
    /* desc: "user <name> on|off [nopass] #h... ~k... %R~k... resetchannels|&c... <command rules>" */
    struct obuf flags = {0};
    struct obuf pass = {0};
    struct obuf keys = {0};
    struct obuf chans = {0};
    struct obuf cmds = {0};
    unsigned nflags = 0;
    unsigned npass = 0;
    char *save = NULL;
    int field = 0;
    for (char *tok = strtok_r(desc, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save), field++) {
        if (field < 2) {
            continue; /* "user", name */
        }
        if (strcmp(tok, "on") == 0 || strcmp(tok, "off") == 0 || strcmp(tok, "nopass") == 0) {
            r_bulk_str(&flags, tok);
            nflags++;
        } else if (tok[0] == '#') {
            r_bulk_str(&pass, tok + 1);
            npass++;
        } else if (tok[0] == '~' || tok[0] == '%') {
            ob_printf(&keys, "%s%s", keys.len ? " " : "", tok);
        } else if (tok[0] == '&') {
            ob_printf(&chans, "%s%s", chans.len ? " " : "", tok);
        } else if (strcmp(tok, "resetchannels") != 0) {
            ob_printf(&cmds, "%s%s", cmds.len ? " " : "", tok);
        }
    }
    r_map(o, 5);
    r_bulk_str(o, "flags");
    r_array(o, nflags);
    ob_add(o, flags.p, flags.len);
    r_bulk_str(o, "passwords");
    r_array(o, npass);
    ob_add(o, pass.p, pass.len);
    r_bulk_str(o, "commands");
    r_bulk(o, cmds.p, cmds.len);
    r_bulk_str(o, "keys");
    r_bulk(o, keys.p, keys.len);
    r_bulk_str(o, "channels");
    r_bulk(o, chans.p, chans.len);
    ob_free(&flags);
    ob_free(&pass);
    ob_free(&keys);
    ob_free(&chans);
    ob_free(&cmds);
}

/*
 * ACL WHOAMI | USERS | LIST | CAT [category] | GETUSER name | DRYRUN user command [arg ...] | LOAD.
 * SETUSER, DELUSER and SAVE are refused: the file is the only source of users.
 */
static void cmd_acl(akvd *d, struct session *ss, struct arg *a, size_t n, struct obuf *o) {
    const acl *l = akvd_acl(d);
    if (n < 2) {
        r_arity(o, &a[0]);
        return;
    }
    const struct arg *sub = &a[1];
    if (is(sub, "CAT")) {
        if (n == 2) {
            unsigned k = 0;
            while (acl_categories[k] != NULL) {
                k++;
            }
            r_array(o, k);
            for (unsigned i = 0; i < k; i++) {
                r_bulk_str(o, acl_categories[i]);
            }
            return;
        }
        uint32_t cat = 0;
        for (unsigned i = 0; acl_categories[i] != NULL; i++) {
            if (is(&a[2], acl_categories[i])) {
                cat = 1u << i;
            }
        }
        if (cat == 0) {
            ob_printf(o, "-ERR Unknown category '%.*s'\r\n", (int)(a[2].n > 64 ? 64 : a[2].n), a[2].p);
            return;
        }
        struct obuf b = {0};
        unsigned cnt = 0;
        for (unsigned i = 0; i < acl_ncmds(); i++) {
            const acl_cmd *k = acl_cmd_at(i);
            if (k->proto == ACL_RESP && (k->cats & cat) != 0) {
                char name[32];
                size_t m = strlen(k->name);
                for (size_t j = 0; j <= m; j++) {
                    name[j] = (char)tolower((unsigned char)k->name[j]);
                }
                r_bulk_str(&b, name);
                cnt++;
            }
        }
        r_array(o, cnt);
        ob_add(o, b.p, b.len);
        ob_free(&b);
        return;
    }
    if (l == NULL) {
        if (is(sub, "WHOAMI")) {
            r_bulk_str(o, "default");
        } else {
            r_err(o, "ERR access control is not configured (aclfile option)");
        }
        return;
    }
    if (is(sub, "WHOAMI")) {
        r_bulk_str(o, ss->user != NULL ? acl_user_name(ss->user) : "default");
    } else if (is(sub, "USERS")) {
        r_array(o, (long long)acl_nusers(l));
        for (size_t i = 0; i < acl_nusers(l); i++) {
            r_bulk_str(o, acl_user_name(acl_user_at(l, i)));
        }
    } else if (is(sub, "LIST")) {
        r_array(o, (long long)acl_nusers(l));
        for (size_t i = 0; i < acl_nusers(l); i++) {
            char desc[8192];
            size_t m = acl_describe(acl_user_at(l, i), desc, sizeof(desc));
            r_bulk(o, desc, m < sizeof(desc) ? m : sizeof(desc) - 1);
        }
    } else if (is(sub, "GETUSER") && n == 3) {
        const acl_user *u = acl_find(l, a[2].p, a[2].n);
        if (u == NULL) {
            r_null(o);
        } else {
            acl_getuser(u, o);
        }
    } else if (is(sub, "DRYRUN") && n >= 4) {
        const acl_user *u = acl_find(l, a[2].p, a[2].n);
        if (u == NULL) {
            ob_printf(o, "-ERR User '%.*s' not found\r\n", (int)(a[2].n > 64 ? 64 : a[2].n), a[2].p);
            return;
        }
        if (acl_cmd_find(ACL_RESP, a[3].p, a[3].n) < 0) {
            ob_printf(o, "-ERR Command '%.*s' not found\r\n", (int)(a[3].n > 64 ? 64 : a[3].n), a[3].p);
            return;
        }
        const char *why = acl_unrestricted(u) ? NULL : resp_denied(u, a + 3, n - 3);
        if (why == NULL) {
            r_ok(o);
        } else if (why[0] == 'k') {
            ob_printf(o, "+User %s has no permissions to access one of the keys\r\n", acl_user_name(u));
        } else {
            char name[64];
            lower_name(&a[3], name, sizeof(name));
            ob_printf(o, "+User %s has no permissions to run the '%s' command\r\n", acl_user_name(u), name);
        }
    } else if (is(sub, "LOAD") && n == 2) {
        char err[512];
        int rc = akvd_acl_reload(d, err, sizeof(err));
        if (rc == 0) {
            r_ok(o);
        } else {
            ob_printf(o, "-ERR %s\r\n", err);
        }
    } else if (is(sub, "SETUSER") || is(sub, "DELUSER") || is(sub, "SAVE")) {
        r_err(o, "ERR users are defined in the ACL file only; edit it and run ACL LOAD");
    } else {
        ob_printf(o, "-ERR unknown subcommand or wrong number of arguments for '%.*s'\r\n",
                  (int)(sub->n > 32 ? 32 : sub->n), sub->p);
    }
}
