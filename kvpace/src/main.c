/*
 * main.c - kvpace command line.
 *
 * The option list is the usage text printed by `kvpace --help` (the string `usage` below). Exit status: 0
 * success, 1 setup failure, 2 invalid options, 3 some operations failed (the result is still written).
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "merge.h"
#include "report.h"
#include "runner.h"
#include "sock.h"

enum {
    OPT_DRIVER = 256,
    OPT_DRIVER_OPT,
    OPT_HOST,
    OPT_PORT,
    OPT_THREADS,
    OPT_CONNS,
    OPT_PIPELINE,
    OPT_AUTH,
    OPT_DB,
    OPT_TLS,
    OPT_CACERT,
    OPT_CERT,
    OPT_KEY,
    OPT_SNI,
    OPT_TLS_SKIP,
    OPT_DURATION,
    OPT_WARMUP,
    OPT_REQUESTS,
    OPT_START_AT,
    OPT_KEYS,
    OPT_KEY_PREFIX,
    OPT_DIST,
    OPT_THETA,
    OPT_GAUSS_SD,
    OPT_VALUE_SIZE,
    OPT_READ_RATIO,
    OPT_RATIO,
    OPT_MIX,
    OPT_MULTI_GET,
    OPT_WORKLOAD,
    OPT_SEED,
    OPT_NO_PRELOAD,
    OPT_PIN,
    OPT_SCENARIO,
    OPT_BLOCK_SIZE,
    OPT_GROUPS,
    OPT_SHARED_BLOCKS,
    OPT_TURN_BLOCKS,
    OPT_ACTIVE_CONV,
    OPT_MAX_TURNS,
    OPT_NEW_CONV_PROB,
    OPT_RATE,
    OPT_ARRIVAL,
    OPT_SERVER_PID,
    OPT_SPIN,
    OPT_LABEL,
    OPT_OUT,
    OPT_CSV,
    OPT_HDR,
    OPT_PERCENTILES,
    OPT_HELP,
    OPT_VERSION,
};

/* Printed by --help; the reference for every option. */
static const char usage[] =
    "Usage: kvpace [options]   (-h, --help: this text; --version: build revision)\n"
    "       kvpace merge --out MERGED.json RESULT.json [RESULT.json ...]\n"
    "\n"
    "Connection:\n"
    "  --driver NAME          protocol driver: resp | memcached | native (default: resp)\n"
    "  --driver-opt K=V       driver option, repeatable (native: transport=tcp|rdma|auto, wait=poll|event,\n"
    "                         max-msg=N, depth=N)\n"
    "  --host HOST, --port PORT   server address (default: 127.0.0.1:6379)\n"
    "  --threads N            client threads (default: 1)\n"
    "  --conns-per-thread N   connections served by each thread (default: 1)\n"
    "  --pipeline N           requests in flight per connection (default: 1)\n"
    "  --auth SECRET          password, or user:password (RESP AUTH, memcached ASCII auth, native AUTH)\n"
    "  --db N                 RESP database to SELECT (default: 0)\n"
    "  --tls, --cacert FILE, --cert FILE, --key FILE, --sni NAME, --tls-skip-verify\n"
    "                         TLS for the text drivers (build with TLS=1)\n"
    "Workload (scenario kv):\n"
    "  --keys N               key space size (default: 100000)\n"
    "  --key-prefix TEXT      key prefix (default: k); keys are the prefix followed by 15 digits\n"
    "  --dist NAME            uniform | zipf | sequential | gaussian | latest (default: uniform)\n"
    "  --theta X              Zipf skew in [0, 1) (default: 0.99)\n"
    "  --gauss-sd X           gaussian: standard deviation as a fraction of the key space (default: 1/6)\n"
    "  --value-size SPEC      N, A-B (uniform range) or S:W,S:W,... (weighted list) (default: 64)\n"
    "  --read-ratio X         fraction of GET, the rest SET (default: 0.9)\n"
    "  --ratio SET:GET        memtier-style ratio, for example 1:10\n"
    "  --mix SPEC             weights, for example get=80,set=15,del=3,incr=2 (also mget, rmw, insert)\n"
    "  --multi-get N          keys per MGET (default: 1)\n"
    "  --workload NAME        YCSB preset: ycsb-a | ycsb-b | ycsb-c | ycsb-d | ycsb-f (workload.h)\n"
    "  --seed N               random seed (default: 1)\n"
    "  --no-preload           skip writing the key space before the run\n"
    "Scenario kvcache:\n"
    "  --scenario NAME        kv | kvcache (default: kv). kvcache runs prefix-chain requests "
    "(src/kvcache.h)\n"
    "  --block-size N, --groups N, --shared-blocks N, --turn-blocks N, --active-conv N, --max-turns N,\n"
    "  --new-conv-prob X      kvcache parameters (defaults in kvcache.h; --theta sets the Zipf skew)\n"
    "Load:\n"
    "  --duration SEC         measurement length (default: 10)\n"
    "  --warmup SEC           warm-up length (default: 2)\n"
    "  --requests N           requests per connection instead of a duration (no warm-up)\n"
    "  --start-at UNIXTIME    wait until this time after the preload (synchronized start on several hosts)\n"
    "  --rate N               open loop: aggregate operations per second (default 0 = closed loop)\n"
    "  --arrival NAME         open-loop inter-arrival times: constant | poisson (default: constant)\n"
    "  --spin                 open loop: busy-wait instead of sleeping until the intended send time\n"
    "  --pin LIST             comma-separated CPU list; thread i uses entry i mod length\n"
    "Output:\n"
    "  --server-pid PID       host PID of the server; its CPU time and RSS are recorded\n"
    "  --label TEXT           free-text name of the system under test, stored in the result\n"
    "  --out FILE             write the JSON result to FILE\n"
    "  --csv FILE             write a CSV summary (one row per operation type)\n"
    "  --hdr-prefix PREFIX    write PREFIX_<op>.hgrm HdrHistogram percentile distributions\n"
    "  --percentiles LIST     percentiles to report (default: 50,90,99,99.9,99.99)\n"
    "\n"
    "Exit status: 0 success, 1 setup failure, 2 invalid options, 3 some operations failed (the result is\n"
    "still written).\n";

static const struct option long_opts[] = {
    {"driver", required_argument, NULL, OPT_DRIVER},
    {"driver-opt", required_argument, NULL, OPT_DRIVER_OPT},
    {"host", required_argument, NULL, OPT_HOST},
    {"port", required_argument, NULL, OPT_PORT},
    {"threads", required_argument, NULL, OPT_THREADS},
    {"conns-per-thread", required_argument, NULL, OPT_CONNS},
    {"pipeline", required_argument, NULL, OPT_PIPELINE},
    {"auth", required_argument, NULL, OPT_AUTH},
    {"db", required_argument, NULL, OPT_DB},
    {"tls", no_argument, NULL, OPT_TLS},
    {"cacert", required_argument, NULL, OPT_CACERT},
    {"cert", required_argument, NULL, OPT_CERT},
    {"key", required_argument, NULL, OPT_KEY},
    {"sni", required_argument, NULL, OPT_SNI},
    {"tls-skip-verify", no_argument, NULL, OPT_TLS_SKIP},
    {"duration", required_argument, NULL, OPT_DURATION},
    {"warmup", required_argument, NULL, OPT_WARMUP},
    {"requests", required_argument, NULL, OPT_REQUESTS},
    {"start-at", required_argument, NULL, OPT_START_AT},
    {"keys", required_argument, NULL, OPT_KEYS},
    {"key-prefix", required_argument, NULL, OPT_KEY_PREFIX},
    {"dist", required_argument, NULL, OPT_DIST},
    {"theta", required_argument, NULL, OPT_THETA},
    {"gauss-sd", required_argument, NULL, OPT_GAUSS_SD},
    {"value-size", required_argument, NULL, OPT_VALUE_SIZE},
    {"read-ratio", required_argument, NULL, OPT_READ_RATIO},
    {"ratio", required_argument, NULL, OPT_RATIO},
    {"mix", required_argument, NULL, OPT_MIX},
    {"multi-get", required_argument, NULL, OPT_MULTI_GET},
    {"workload", required_argument, NULL, OPT_WORKLOAD},
    {"seed", required_argument, NULL, OPT_SEED},
    {"no-preload", no_argument, NULL, OPT_NO_PRELOAD},
    {"pin", required_argument, NULL, OPT_PIN},
    {"scenario", required_argument, NULL, OPT_SCENARIO},
    {"block-size", required_argument, NULL, OPT_BLOCK_SIZE},
    {"groups", required_argument, NULL, OPT_GROUPS},
    {"shared-blocks", required_argument, NULL, OPT_SHARED_BLOCKS},
    {"turn-blocks", required_argument, NULL, OPT_TURN_BLOCKS},
    {"active-conv", required_argument, NULL, OPT_ACTIVE_CONV},
    {"max-turns", required_argument, NULL, OPT_MAX_TURNS},
    {"new-conv-prob", required_argument, NULL, OPT_NEW_CONV_PROB},
    {"rate", required_argument, NULL, OPT_RATE},
    {"arrival", required_argument, NULL, OPT_ARRIVAL},
    {"server-pid", required_argument, NULL, OPT_SERVER_PID},
    {"spin", no_argument, NULL, OPT_SPIN},
    {"label", required_argument, NULL, OPT_LABEL},
    {"out", required_argument, NULL, OPT_OUT},
    {"csv", required_argument, NULL, OPT_CSV},
    {"hdr-prefix", required_argument, NULL, OPT_HDR},
    {"percentiles", required_argument, NULL, OPT_PERCENTILES},
    {"help", no_argument, NULL, OPT_HELP},
    {"version", no_argument, NULL, OPT_VERSION},
    {NULL, 0, NULL, 0},
};

/* Parses "0,2,4" into a heap array; returns the entry count or -1. */
static int parse_cpu_list(const char *s, int **out) {
    int n = 0;
    int *v = malloc(strlen(s) * sizeof(int) + sizeof(int));
    if (v == NULL) {
        return -1;
    }
    char *copy = strdup(s);
    for (char *tok = strtok(copy, ","); tok != NULL; tok = strtok(NULL, ",")) {
        char *end = NULL;
        long cpu = strtol(tok, &end, 10);
        if (*end != '\0' || cpu < 0) {
            free(copy);
            free(v);
            return -1;
        }
        v[n++] = (int)cpu;
    }
    free(copy);
    *out = v;
    return n;
}

static int bad(const char *what, const char *value) {
    fprintf(stderr, "invalid %s: %s\n", what, value);
    return 2;
}

int main(int argc, char **argv) {
    /* TLS connections are written with write(2) by OpenSSL; a closed server must not kill the process. */
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2 && strcmp(argv[1], "merge") == 0) {
        return pace_merge_main(argc - 1, argv + 1);
    }
    pace_config cfg;
    pace_config_default(&cfg);
    pace_kvcache_defaults(&cfg.kvcache);
    pace_tls_cfg tls = {0};
    pace_percentiles pc;
    pace_default_percentiles(&pc);
    const char *label = "unlabeled";
    const char *out_path = NULL;
    const char *csv_path = NULL;
    const char *hdr_prefix = NULL;
    const char *workload = NULL;
    int value_size_given = 0;
    int *cpus = NULL;
    char err[160];

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case OPT_DRIVER:
            cfg.driver = pace_driver_find(optarg);
            if (cfg.driver == NULL) {
                return bad("driver", optarg);
            }
            break;
        case OPT_DRIVER_OPT: {
            char *eq = strchr(optarg, '=');
            if (eq == NULL || cfg.driver->set_option == NULL) {
                return bad("driver option (or driver without options)", optarg);
            }
            *eq = '\0';
            if (cfg.driver->set_option(optarg, eq + 1) != 0) {
                *eq = '=';
                return bad("driver option", optarg);
            }
            *eq = '=';
            break;
        }
        case OPT_HOST:
            cfg.host = optarg;
            break;
        case OPT_PORT:
            cfg.port = optarg;
            break;
        case OPT_THREADS:
            cfg.threads = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_CONNS:
            cfg.conns_per_thread = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_PIPELINE:
            cfg.pipeline = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_AUTH:
            pace_driver_set_auth(optarg);
            break;
        case OPT_DB:
            pace_driver_set_db((unsigned)strtoul(optarg, NULL, 10));
            break;
        case OPT_TLS:
            tls.enabled = 1;
            break;
        case OPT_CACERT:
            tls.cacert = optarg;
            break;
        case OPT_CERT:
            tls.cert = optarg;
            break;
        case OPT_KEY:
            tls.key = optarg;
            break;
        case OPT_SNI:
            tls.sni = optarg;
            break;
        case OPT_TLS_SKIP:
            tls.skip_verify = 1;
            break;
        case OPT_DURATION:
            cfg.duration_s = strtod(optarg, NULL);
            break;
        case OPT_WARMUP:
            cfg.warmup_s = strtod(optarg, NULL);
            break;
        case OPT_REQUESTS:
            cfg.requests = strtoull(optarg, NULL, 10);
            break;
        case OPT_START_AT:
            cfg.start_at = strtod(optarg, NULL);
            break;
        case OPT_KEYS:
            cfg.wl.nkeys = strtoull(optarg, NULL, 10);
            break;
        case OPT_KEY_PREFIX:
            if (strlen(optarg) > PACE_KEY_PREFIX_MAX || strpbrk(optarg, " \t\r\n") != NULL) {
                return bad("key prefix (at most 32 bytes, no whitespace)", optarg);
            }
            strcpy(cfg.wl.prefix, optarg);
            cfg.wl.prefix_len = strlen(optarg);
            break;
        case OPT_DIST: {
            static const char *const names[] = {"uniform", "zipf", "sequential", "gaussian", "latest"};
            int found = -1;
            for (int i = 0; i < 5; i++) {
                if (strcmp(optarg, names[i]) == 0) {
                    found = i;
                }
            }
            if (found < 0) {
                return bad("distribution", optarg);
            }
            cfg.wl.dist = (pace_dist)found;
            break;
        }
        case OPT_THETA:
            cfg.wl.theta = strtod(optarg, NULL);
            break;
        case OPT_GAUSS_SD:
            cfg.wl.gauss_sd = strtod(optarg, NULL);
            break;
        case OPT_VALUE_SIZE:
            if (pace_wl_parse_vsize(&cfg.wl, optarg) != 0) {
                return bad("value size", optarg);
            }
            value_size_given = 1;
            break;
        case OPT_READ_RATIO: {
            double r = strtod(optarg, NULL);
            if (r < 0.0 || r > 1.0) {
                return bad("read ratio", optarg);
            }
            memset(cfg.wl.weight, 0, sizeof(cfg.wl.weight));
            cfg.wl.weight[PACE_OP_GET] = r;
            cfg.wl.weight[PACE_OP_SET] = 1.0 - r;
            break;
        }
        case OPT_RATIO:
            if (pace_wl_parse_ratio(&cfg.wl, optarg) != 0) {
                return bad("ratio (SET:GET)", optarg);
            }
            break;
        case OPT_MIX:
            if (pace_wl_parse_mix(&cfg.wl, optarg) != 0) {
                return bad("mix", optarg);
            }
            break;
        case OPT_MULTI_GET:
            cfg.wl.mget_keys = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_WORKLOAD:
            workload = optarg;
            break;
        case OPT_SEED:
            cfg.seed = strtoull(optarg, NULL, 10);
            break;
        case OPT_NO_PRELOAD:
            cfg.preload = 0;
            break;
        case OPT_PIN: {
            int n = parse_cpu_list(optarg, &cpus);
            if (n <= 0) {
                return bad("CPU list", optarg);
            }
            cfg.pin_cpus = cpus;
            cfg.npin = (unsigned)n;
            break;
        }
        case OPT_SCENARIO:
            if (strcmp(optarg, "kv") == 0) {
                cfg.scenario = PACE_SCENARIO_KV;
            } else if (strcmp(optarg, "kvcache") == 0) {
                cfg.scenario = PACE_SCENARIO_KVCACHE;
            } else {
                return bad("scenario", optarg);
            }
            break;
        case OPT_BLOCK_SIZE:
            cfg.kvcache.block_size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_GROUPS:
            cfg.kvcache.groups = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_SHARED_BLOCKS:
            cfg.kvcache.shared_blocks = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_TURN_BLOCKS:
            cfg.kvcache.turn_blocks = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_ACTIVE_CONV:
            cfg.kvcache.active_conv = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_MAX_TURNS:
            cfg.kvcache.max_turns = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_NEW_CONV_PROB:
            cfg.kvcache.new_conv_prob = strtod(optarg, NULL);
            break;
        case OPT_RATE:
            cfg.rate = strtod(optarg, NULL);
            break;
        case OPT_ARRIVAL:
            if (strcmp(optarg, "constant") == 0) {
                cfg.arrival = PACE_ARRIVAL_CONSTANT;
            } else if (strcmp(optarg, "poisson") == 0) {
                cfg.arrival = PACE_ARRIVAL_POISSON;
            } else {
                return bad("arrival process", optarg);
            }
            break;
        case OPT_SPIN:
            cfg.spin = 1;
            break;
        case OPT_SERVER_PID:
            cfg.server_pid = atoi(optarg);
            break;
        case OPT_LABEL:
            label = optarg;
            break;
        case OPT_OUT:
            out_path = optarg;
            break;
        case OPT_CSV:
            csv_path = optarg;
            break;
        case OPT_HDR:
            hdr_prefix = optarg;
            break;
        case OPT_PERCENTILES:
            if (pace_parse_percentiles(optarg, &pc) != 0) {
                return bad("percentile list", optarg);
            }
            break;
        case 'h':
        case OPT_HELP:
            fputs(usage, stdout);
            return 0;
        case OPT_VERSION:
            printf("kvpace %s\n", PACE_GIT);
            return 0;
        default:
            fprintf(stderr, "kvpace --help lists the options\n");
            return 2;
        }
    }
    if (workload != NULL) {
        if (pace_wl_preset(&cfg.wl, workload, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s\n", err);
            return 2;
        }
        if (!value_size_given) {
            /* YCSB default record: 10 fields of 100 bytes (Cooper et al., SoCC 2010). */
            pace_wl_parse_vsize(&cfg.wl, "1000");
        }
    }
    if (cfg.threads == 0 || cfg.wl.nkeys == 0 || (cfg.duration_s <= 0.0 && cfg.requests == 0) ||
        cfg.rate < 0.0) {
        fprintf(stderr, "invalid numeric option\n");
        return 2;
    }
    if (pace_tls_setup(&tls) != 0) {
        return 1;
    }

    cfg.kvcache.theta = cfg.wl.theta < 1.0 ? cfg.wl.theta : 0.9;
    cfg.kvcache.seed = cfg.seed;
    static pace_result res;
    if (pace_run(&cfg, &res) != 0) {
        return 1;
    }
    pace_report_text(stdout, &cfg, &res, &pc);
    if (out_path != NULL) {
        FILE *f = fopen(out_path, "w");
        if (f == NULL) {
            perror(out_path);
            return 1;
        }
        pace_report_json(f, &cfg, &res, &pc, label, argc, argv);
        fclose(f);
    }
    if (csv_path != NULL) {
        FILE *f = fopen(csv_path, "w");
        if (f == NULL) {
            perror(csv_path);
            return 1;
        }
        pace_report_csv(f, &cfg, &res, &pc);
        fclose(f);
    }
    if (hdr_prefix != NULL && pace_report_hgrm(hdr_prefix, &res) != 0) {
        return 1;
    }
    free(cpus);
    return res.errors > 0 ? 3 : 0;
}
