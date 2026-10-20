/*
 * arenakvd.c - key-value cache server daemon.
 *
 * Usage: arenakvd [options]; every option can also be given in a configuration file (--config FILE) as
 * "name = value" or "name value" per line (names without the leading dashes, '#' starts a comment, a
 * flag is written alone or as "name = yes"). Command-line options override the file.
 *
 * Native protocol (rdmaduct):
 *   --transport tcp|rdma|auto   transport (default: auto = RDMA when a device exists, else TCP)
 *   --wait poll|event           waiting strategy of RDMA connections (default: event)
 *   --addr ADDRESS              listen address (default: all IPv4 addresses)
 *   --port PORT|off             listen port (default: 7600)
 *   --max-msg BYTES             largest native message, bounds key + value + 36 (default: 65536)
 *   --depth N                   receive and send buffers per connection (default: 16)
 * Text protocols:
 *   --text-addr ADDRESS         listen address of the text listeners (default: all IPv4 addresses)
 *   --memcached-port PORT       memcached text and meta protocol (default: off)
 *   --resp-port PORT            RESP2 / Redis protocol (default: off)
 *   --tls-memcached-port PORT, --tls-resp-port PORT, --tls-cert FILE, --tls-key FILE, --tls-ca FILE
 *                               TLS listeners (build with TLS=1)
 * Engine:
 *   --capacity SIZE             cache memory, with optional K, M or G suffix (default: 256M)
 *   --shards N                  shards, power of two (default: 64)
 *   --max-entry BYTES           largest key + value (default: 1048576)
 *   --avg-entry BYTES           presize the index for this mean entry size (default: grow on demand)
 *   --no-evict                  refuse stores when full instead of evicting
 *   --huge-pages                advise transparent huge pages for the cache memory
 * Server:
 *   --threads N                 worker threads (default: online CPUs)
 *   --max-conns N               connection limit (default: unlimited)
 *   --idle-timeout SECONDS      close idle connections (default: never)
 *   --auth-file FILE            credentials, "user:password" or a password per line
 *   --snapshot FILE             snapshot file, loaded at start, written on SAVE/BGSAVE and at exit
 *   --save-interval SECONDS     periodic snapshot when data changed (default: off)
 *   --no-save-on-stop           do not write the snapshot at exit
 *   --log-level error|warn|info|debug, --log-file FILE (reopened on SIGHUP)
 *   --stats SECONDS             log a statistics line every SECONDS seconds
 * Signals: SIGINT and SIGTERM stop the server (after the final snapshot); SIGHUP reopens the log file.
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "akvd.h"

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t reopen_requested;

static void on_signal(int sig) {
    if (sig == SIGHUP) {
        reopen_requested = 1;
    } else {
        stop_requested = 1;
    }
}

/* Parses a size with optional K/M/G suffix; returns 0 on error. */
static uint64_t parse_size(const char *s) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0) {
        return 0;
    }
    switch (*end) {
    case '\0':
        break;
    case 'k':
    case 'K':
        v *= 1024.0;
        break;
    case 'm':
    case 'M':
        v *= 1024.0 * 1024.0;
        break;
    case 'g':
    case 'G':
        v *= 1024.0 * 1024.0 * 1024.0;
        break;
    default:
        return 0;
    }
    return (uint64_t)v;
}

static const struct option lo[] = {
    {"transport", required_argument, NULL, 't'},      {"wait", required_argument, NULL, 'w'},
    {"addr", required_argument, NULL, 'a'},           {"port", required_argument, NULL, 'p'},
    {"capacity", required_argument, NULL, 'c'},       {"shards", required_argument, NULL, 's'},
    {"max-msg", required_argument, NULL, 'm'},        {"max-entry", required_argument, NULL, 'E'},
    {"avg-entry", required_argument, NULL, 'e'},      {"depth", required_argument, NULL, 'd'},
    {"huge-pages", no_argument, NULL, 'H'},           {"no-evict", no_argument, NULL, 'N'},
    {"stats", required_argument, NULL, 'S'},          {"threads", required_argument, NULL, 'T'},
    {"max-conns", required_argument, NULL, 'C'},      {"idle-timeout", required_argument, NULL, 'i'},
    {"text-addr", required_argument, NULL, 'A'},      {"memcached-port", required_argument, NULL, 'M'},
    {"resp-port", required_argument, NULL, 'R'},      {"tls-memcached-port", required_argument, NULL, 1001},
    {"tls-resp-port", required_argument, NULL, 1002}, {"tls-cert", required_argument, NULL, 1003},
    {"tls-key", required_argument, NULL, 1004},       {"tls-ca", required_argument, NULL, 1005},
    {"auth-file", required_argument, NULL, 1006},     {"snapshot", required_argument, NULL, 1007},
    {"save-interval", required_argument, NULL, 1008}, {"no-save-on-stop", no_argument, NULL, 1009},
    {"log-level", required_argument, NULL, 1010},     {"log-file", required_argument, NULL, 1011},
    {"config", required_argument, NULL, 1012},        {NULL, 0, NULL, 0},
};

/* Returns the option table entry of a long option name, or NULL. */
static const struct option *find_option(const char *name) {
    for (const struct option *o = lo; o->name != NULL; o++) {
        if (strcmp(o->name, name) == 0) {
            return o;
        }
    }
    return NULL;
}

/* Strings that stay referenced by the configuration for the life of the process. */
static char *keep(const char *s) {
    char *p = strdup(s);
    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

/*
 * Reads a configuration file into argument form ("--name", "value") appended to *args.
 * Returns 0, or -1 after printing an error.
 */
static int read_config(const char *path, char ***args, int *nargs) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t n = strcspn(p, " \t=\r\n");
        if (n == 0) {
            continue;
        }
        char name[128];
        if (n >= sizeof(name)) {
            fprintf(stderr, "%s:%d: option name too long\n", path, lineno);
            fclose(f);
            return -1;
        }
        memcpy(name, p, n);
        name[n] = '\0';
        char *v = p + n;
        while (*v == ' ' || *v == '\t' || *v == '=') {
            v++;
        }
        size_t vn = strcspn(v, "\r\n");
        while (vn > 0 && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) {
            vn--;
        }
        v[vn] = '\0';
        const struct option *o = find_option(name);
        if (o == NULL || strcmp(name, "config") == 0) {
            fprintf(stderr, "%s:%d: unknown option '%s'\n", path, lineno, name);
            fclose(f);
            return -1;
        }
        char **a = realloc(*args, (size_t)(*nargs + 2) * sizeof(char *));
        if (a == NULL) {
            fclose(f);
            return -1;
        }
        *args = a;
        char opt[132];
        snprintf(opt, sizeof(opt), "--%s", name);
        if (o->has_arg == no_argument) {
            if (vn != 0 && strcmp(v, "yes") != 0 && strcmp(v, "true") != 0 && strcmp(v, "1") != 0) {
                continue; /* "flag = no" leaves the flag unset */
            }
            a[(*nargs)++] = keep(opt);
        } else {
            if (vn == 0) {
                fprintf(stderr, "%s:%d: option '%s' needs a value\n", path, lineno, name);
                fclose(f);
                return -1;
            }
            a[(*nargs)++] = keep(opt);
            a[(*nargs)++] = keep(v);
        }
    }
    fclose(f);
    return 0;
}

static const char *port_or_null(const char *v) {
    return strcmp(v, "off") == 0 ? NULL : v;
}

int main(int argc, char **argv) {
    akvd_config cfg;
    akvd_config_default(&cfg);
    cfg.store.max_entry = 1u << 20;
    cfg.save_on_stop = 1;
    unsigned stats_every = 0;

    /* The configuration file comes first, so that command-line options override it. */
    char **args = malloc(sizeof(char *));
    int nargs = 0;
    if (args == NULL) {
        return 1;
    }
    args[nargs++] = argv[0];
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && read_config(argv[i + 1], &args, &nargs) != 0) {
            return 2;
        }
    }
    char **all = realloc(args, (size_t)(nargs + argc) * sizeof(char *));
    if (all == NULL) {
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        all[nargs++] = argv[i];
    }
    all[nargs] = NULL;

    int c;
    while ((c = getopt_long(nargs, all, "", lo, NULL)) != -1) {
        switch (c) {
        case 't':
            cfg.transport = strcmp(optarg, "tcp") == 0    ? DUCT_TRANSPORT_TCP
                            : strcmp(optarg, "rdma") == 0 ? DUCT_TRANSPORT_RDMA
                                                          : DUCT_TRANSPORT_AUTO;
            break;
        case 'w':
            cfg.wait = strcmp(optarg, "poll") == 0 ? DUCT_WAIT_POLL : DUCT_WAIT_EVENT;
            break;
        case 'a':
            cfg.addr = optarg;
            break;
        case 'p':
            cfg.port = port_or_null(optarg);
            break;
        case 'c':
            cfg.store.capacity = parse_size(optarg);
            break;
        case 's':
            cfg.store.shards = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'e':
            cfg.store.avg_entry = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'E':
            cfg.store.max_entry = (uint32_t)parse_size(optarg);
            break;
        case 'm':
            cfg.max_msg = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            cfg.recv_depth = cfg.send_depth = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'H':
            cfg.store.huge_pages = 1;
            break;
        case 'N':
            cfg.store.no_evict = 1;
            break;
        case 'S':
            stats_every = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'T':
            cfg.threads = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'C':
            cfg.max_conns = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'i':
            cfg.idle_timeout_s = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'A':
            cfg.text_addr = optarg;
            break;
        case 'M':
            cfg.memcached_port = port_or_null(optarg);
            break;
        case 'R':
            cfg.resp_port = port_or_null(optarg);
            break;
        case 1001:
            cfg.tls_memcached_port = port_or_null(optarg);
            break;
        case 1002:
            cfg.tls_resp_port = port_or_null(optarg);
            break;
        case 1003:
            cfg.tls_cert = optarg;
            break;
        case 1004:
            cfg.tls_key = optarg;
            break;
        case 1005:
            cfg.tls_ca = optarg;
            break;
        case 1006:
            cfg.auth_file = optarg;
            break;
        case 1007:
            cfg.snapshot_path = optarg;
            break;
        case 1008:
            cfg.save_interval_s = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 1009:
            cfg.save_on_stop = 0;
            break;
        case 1010:
            if (strcmp(optarg, "error") == 0) {
                cfg.log_level = AKVD_LOG_ERROR;
            } else if (strcmp(optarg, "warn") == 0) {
                cfg.log_level = AKVD_LOG_WARN;
            } else if (strcmp(optarg, "info") == 0) {
                cfg.log_level = AKVD_LOG_INFO;
            } else if (strcmp(optarg, "debug") == 0) {
                cfg.log_level = AKVD_LOG_DEBUG;
            } else {
                fprintf(stderr, "invalid log level '%s'\n", optarg);
                return 2;
            }
            break;
        case 1011:
            cfg.log_path = optarg;
            break;
        case 1012:
            break; /* already read */
        default:
            return 2;
        }
    }
    if (cfg.store.capacity == 0 || cfg.store.max_entry == 0) {
        fprintf(stderr, "invalid capacity or maximum entry size\n");
        return 2;
    }
    if (cfg.port == NULL && cfg.memcached_port == NULL && cfg.resp_port == NULL &&
        cfg.tls_memcached_port == NULL && cfg.tls_resp_port == NULL) {
        fprintf(stderr, "no listener configured\n");
        return 2;
    }
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int err = 0;
    akvd *d = akvd_start(&cfg, &err);
    if (d == NULL) {
        fprintf(stderr, "cannot start server: %s\n",
                err == -ENOTSUP ? "TLS options need a build with TLS=1" : duct_strerror(err));
        return 1;
    }
    akvd_log(
        d, AKVD_LOG_INFO,
        "arenakvd listening: native %d, memcached %d, resp %d, tls memcached %d, tls resp %d; %llu MiB in %u "
        "shards; tag matching: %s",
        akvd_port(d), akvd_memcached_port(d), akvd_resp_port(d), akvd_tls_memcached_port(d),
        akvd_tls_resp_port(d), (unsigned long long)(cfg.store.capacity >> 20),
        cfg.store.shards ? cfg.store.shards : 64, akv_simd_name());
    unsigned elapsed = 0;
    while (!stop_requested) {
        sleep(1);
        if (reopen_requested) {
            reopen_requested = 0;
            akvd_reopen_log(d);
            akvd_log(d, AKVD_LOG_INFO, "log reopened");
        }
        if (stats_every != 0 && ++elapsed % stats_every == 0) {
            akv_stats st;
            akv_stats_get(akvd_store(d), &st);
            akvd_log(
                d, AKVD_LOG_INFO,
                "entries=%llu log=%llu/%llu MiB index=%llu KiB gets=%llu hits=%llu puts=%llu evictions=%llu "
                "expired=%llu conns=%u",
                (unsigned long long)st.entries, (unsigned long long)(st.log_bytes >> 20),
                (unsigned long long)(st.arena_bytes >> 20), (unsigned long long)(st.index_bytes >> 10),
                (unsigned long long)st.gets, (unsigned long long)st.hits, (unsigned long long)st.puts,
                (unsigned long long)st.evictions, (unsigned long long)st.expired, akvd_connections(d));
        }
    }
    akvd_log(d, AKVD_LOG_INFO, "stopping");
    akvd_stop(d);
    return 0;
}
