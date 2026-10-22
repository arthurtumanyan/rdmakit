/*
 * akv_cli.c - command-line client for the native arenakv protocol (akvclient.h) over TCP or RDMA.
 *
 * Usage: akv_cli [options] COMMAND [ARGS]
 *   --host HOST            server address (default: 127.0.0.1; RDMA: the address of the RDMA interface)
 *   --port PORT            native port (default: 7600)
 *   --transport tcp|rdma|auto   (default: tcp)
 *   --auth CREDENTIALS     "user:password" or a password of the default user
 *   --ttl SECONDS          expiry for set, add, replace (default: none)
 *   --flags N              client flags for set, add, replace (default: 0)
 *   --max-msg BYTES        message size limit, bounds key + value + 36 (default: 1 MiB + 64)
 * Commands:
 *   get KEY                 print the value (raw bytes, no newline added)
 *   meta KEY                print flags, CAS version and remaining time to live
 *   set|add|replace|append|prepend KEY VALUE   store; VALUE "-" reads the value from standard input
 *   del KEY, exists KEY, touch KEY SECONDS (0: no expiry), incr KEY DELTA (signed, RESP semantics)
 *   stats, flush, ping
 * Exit status: 0 success, 1 key absent or condition not met (NOTFOUND, EXISTS), 2 invalid usage,
 * 3 other server status or transport error.
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "akvclient.h"

#define DEFAULT_MAX_MSG ((1u << 20) + 64u)

static const char usage[] =
    "Usage: akv_cli [options] COMMAND [ARGS]\n"
    "  --host HOST            server address (default: 127.0.0.1)\n"
    "  --port PORT            native port (default: 7600)\n"
    "  --transport tcp|rdma|auto   (default: tcp)\n"
    "  --auth CREDENTIALS     \"user:password\" or a password of the default user\n"
    "  --ttl SECONDS          expiry for set, add, replace (default: none)\n"
    "  --flags N              client flags for set, add, replace (default: 0)\n"
    "  --max-msg BYTES        message size limit, bounds key + value + 36 (default: 1048640)\n"
    "Commands:\n"
    "  get KEY                print the value (raw bytes)\n"
    "  meta KEY               print flags, CAS version and remaining time to live\n"
    "  set|add|replace|append|prepend KEY VALUE   store (VALUE - reads standard input)\n"
    "  del KEY | exists KEY | touch KEY SECONDS | incr KEY DELTA\n"
    "  stats | flush | ping\n"
    "Exit status: 0 success, 1 absent or condition not met, 2 invalid usage, 3 error.\n";

/* Names of the STATS counters in payload order (akvproto.h). */
static const char *const stat_names[AKVP_STATS_FIELDS] = {
    "entries",
    "arena_bytes",
    "log_bytes",
    "gets",
    "hits",
    "puts",
    "overwrites",
    "dels",
    "evictions",
    "reinsertions",
    "index_evictions",
    "rejected",
    "expired",
    "index_bytes",
    "index_grows",
    "curr_connections",
    "total_connections",
    "commands",
    "uptime_seconds",
};

static const char *status_name(int rc) {
    static const char *const names[] = {"OK",     "NOTFOUND", "BADREQ", "TOOBIG", "ERROR",
                                        "EXISTS", "NOMEM",    "NOAUTH", "NOTNUM", "NOPERM"};
    if (rc >= 0 && rc < (int)(sizeof(names) / sizeof(names[0]))) {
        return names[rc];
    }
    return "unknown status";
}

/* Maps a client result to the exit status and prints a message for failures. */
static int finish(int rc) {
    if (rc == AKVP_OK) {
        return 0;
    }
    if (rc == -EMSGSIZE || rc == AKVP_TOOBIG) {
        fprintf(stderr,
                "akv_cli: key + value exceed the message limit (the smaller of --max-msg here and the "
                "server's --max-msg) or the server's --max-entry\n");
        return 3;
    }
    if (rc < 0) {
        fprintf(stderr, "akv_cli: %s\n", duct_strerror(rc));
        return 3;
    }
    fprintf(stderr, "akv_cli: %s\n", status_name(rc));
    return rc == AKVP_NOTFOUND || rc == AKVP_EXISTS ? 1 : 3;
}

/* Reads standard input completely into a heap buffer of at most limit bytes. Returns NULL on failure. */
static char *read_stdin(size_t limit, size_t *len) {
    size_t cap = 4096;
    size_t n = 0;
    char *buf = malloc(cap);
    while (buf != NULL) {
        size_t got = fread(buf + n, 1, cap - n, stdin);
        n += got;
        if (got == 0) {
            break;
        }
        if (n == cap) {
            if (cap >= limit) {
                free(buf);
                return NULL;
            }
            char *b = realloc(buf, cap * 2);
            if (b == NULL) {
                free(buf);
                return NULL;
            }
            buf = b;
            cap *= 2;
        }
    }
    *len = n;
    return buf;
}

/* Parses a signed decimal integer. Returns 0, or -1 when s is not one. */
static int parse_i64(const char *s, int64_t *out) {
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    const char *port = "7600";
    const char *auth = NULL;
    int64_t ttl_s = 0;
    int64_t uflags = 0;
    int64_t max_msg = DEFAULT_MAX_MSG;
    struct duct_opts opts;
    duct_opts_default(&opts);
    opts.transport = DUCT_TRANSPORT_TCP;
    static const struct option lo[] = {
        {"host", required_argument, NULL, 'H'},
        {"port", required_argument, NULL, 'p'},
        {"transport", required_argument, NULL, 't'},
        {"auth", required_argument, NULL, 'a'},
        {"ttl", required_argument, NULL, 'T'},
        {"flags", required_argument, NULL, 'f'},
        {"max-msg", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "+h", lo, NULL)) != -1) {
        switch (c) {
        case 'H':
            host = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 't':
            if (strcmp(optarg, "tcp") == 0) {
                opts.transport = DUCT_TRANSPORT_TCP;
            } else if (strcmp(optarg, "rdma") == 0) {
                opts.transport = DUCT_TRANSPORT_RDMA;
            } else if (strcmp(optarg, "auto") == 0) {
                opts.transport = DUCT_TRANSPORT_AUTO;
            } else {
                fprintf(stderr, "akv_cli: invalid transport '%s'\n", optarg);
                return 2;
            }
            break;
        case 'a':
            auth = optarg;
            break;
        case 'T':
            if (parse_i64(optarg, &ttl_s) != 0 || ttl_s < 0 || ttl_s > INT64_MAX / 1000) {
                fprintf(stderr, "akv_cli: invalid --ttl '%s'\n", optarg);
                return 2;
            }
            break;
        case 'f':
            if (parse_i64(optarg, &uflags) != 0 || uflags < 0 || uflags > UINT32_MAX) {
                fprintf(stderr, "akv_cli: invalid --flags '%s'\n", optarg);
                return 2;
            }
            break;
        case 'm':
            if (parse_i64(optarg, &max_msg) != 0 || max_msg < 64 || max_msg > (1 << 30)) {
                fprintf(stderr, "akv_cli: invalid --max-msg '%s'\n", optarg);
                return 2;
            }
            break;
        case 'h':
            fputs(usage, stdout);
            return 0;
        default:
            fputs(usage, stderr);
            return 2;
        }
    }
    if (optind >= argc) {
        fputs(usage, stderr);
        return 2;
    }
    const char *cmd = argv[optind];
    char **arg = argv + optind + 1;
    int nargs = argc - optind - 1;
    static const struct {
        const char *name;
        int nargs;
        uint8_t op;
    } cmds[] = {
        {"get", 1, AKVP_GET},         {"meta", 1, AKVP_GETM},       {"set", 2, AKVP_PUT},
        {"add", 2, AKVP_ADD},         {"replace", 2, AKVP_REPLACE}, {"append", 2, AKVP_APPEND},
        {"prepend", 2, AKVP_PREPEND}, {"del", 1, AKVP_DEL},         {"exists", 1, AKVP_HAS},
        {"touch", 2, AKVP_TOUCH},     {"incr", 2, AKVP_INCRS},      {"stats", 0, AKVP_STATS},
        {"flush", 0, AKVP_FLUSH},     {"ping", 0, AKVP_NOOP},
    };
    int which = -1;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (strcmp(cmd, cmds[i].name) == 0) {
            which = (int)i;
        }
    }
    if (which < 0 || nargs != cmds[which].nargs) {
        fprintf(stderr, which < 0 ? "akv_cli: unknown command '%s'\n" : "akv_cli: wrong arguments for '%s'\n",
                cmd);
        return 2;
    }
    uint8_t op = cmds[which].op;
    opts.max_msg = (uint32_t)max_msg;

    akvc *cl;
    int rc = akvc_connect(&cl, &opts, host, port);
    if (rc != 0) {
        fprintf(stderr, "akv_cli: cannot connect to %s:%s: %s\n", host, port, duct_strerror(rc));
        return 3;
    }
    if (auth != NULL && (rc = akvc_auth(cl, auth)) != AKVP_OK) {
        akvc_close(cl);
        return finish(rc);
    }
    size_t cap = (size_t)max_msg;
    char *buf = malloc(cap);
    if (buf == NULL) {
        akvc_close(cl);
        return 3;
    }
    const char *key = nargs > 0 ? arg[0] : NULL;
    size_t klen = key != NULL ? strlen(key) : 0;
    size_t vlen = 0;
    switch (op) {
    case AKVP_GET:
        rc = akvc_get(cl, key, klen, buf, cap, &vlen);
        if (rc == AKVP_OK) {
            fwrite(buf, 1, vlen, stdout);
        }
        break;
    case AKVP_GETM: {
        uint32_t fl;
        uint64_t cas;
        int64_t ttl_ms;
        rc = akvc_get_meta(cl, key, klen, buf, cap, &vlen, &fl, &cas, &ttl_ms, INT64_MIN);
        if (rc == AKVP_OK) {
            printf("length %zu\nflags %" PRIu32 "\ncas %" PRIu64 "\nttl_ms %" PRId64 "\n", vlen, fl, cas,
                   ttl_ms);
        }
        break;
    }
    case AKVP_PUT:
    case AKVP_ADD:
    case AKVP_REPLACE:
    case AKVP_APPEND:
    case AKVP_PREPEND: {
        const char *val = arg[1];
        char *in = NULL;
        vlen = strlen(val);
        if (strcmp(val, "-") == 0) {
            in = read_stdin(cap, &vlen);
            if (in == NULL) {
                fprintf(stderr, "akv_cli: cannot read the value from standard input (limit %zu bytes)\n",
                        cap);
                rc = AKVP_TOOBIG;
                break;
            }
            val = in;
        }
        struct akvp_ext x = {.ttl_ms = ttl_s * 1000, .uflags = (uint32_t)uflags};
        uint64_t cas = 0;
        rc = akvc_store(cl, op, key, klen, val, vlen, &x, &cas);
        free(in);
        break;
    }
    case AKVP_DEL:
        rc = akvc_del(cl, key, klen);
        break;
    case AKVP_HAS:
        rc = akvc_exists(cl, key, klen);
        break;
    case AKVP_TOUCH: {
        int64_t s;
        if (parse_i64(arg[1], &s) != 0 || s < 0 || s > INT64_MAX / 1000) {
            fprintf(stderr, "akv_cli: invalid seconds '%s'\n", arg[1]);
            free(buf);
            akvc_close(cl);
            return 2;
        }
        rc = akvc_touch(cl, key, klen, s * 1000);
        break;
    }
    case AKVP_INCRS: {
        int64_t delta;
        int64_t result;
        if (parse_i64(arg[1], &delta) != 0) {
            fprintf(stderr, "akv_cli: invalid delta '%s'\n", arg[1]);
            free(buf);
            akvc_close(cl);
            return 2;
        }
        rc = akvc_incr(cl, key, klen, delta, 1, &result);
        if (rc == AKVP_OK) {
            printf("%" PRId64 "\n", result);
        }
        break;
    }
    case AKVP_STATS: {
        uint64_t st[AKVP_STATS_FIELDS];
        rc = akvc_stats(cl, st);
        for (unsigned i = 0; rc == AKVP_OK && i < AKVP_STATS_FIELDS; i++) {
            printf("%s %" PRIu64 "\n", stat_names[i], st[i]);
        }
        break;
    }
    case AKVP_FLUSH:
        rc = akvc_flush(cl);
        break;
    default: {
        uint8_t req[AKVP_REQ_HDR] = {AKVP_NOOP};
        const uint8_t *resp;
        size_t rlen;
        rc = akvc_raw(cl, req, sizeof(req), &resp, &rlen);
        if (rc == 0) {
            rc = rlen >= AKVP_RESP_HDR ? resp[0] : AKVP_ERROR;
        }
        if (rc == AKVP_OK) {
            puts("PONG");
        }
        break;
    }
    }
    free(buf);
    akvc_close(cl);
    return finish(rc);
}
