/*
 * main.c - kvpace command line.
 *
 * Usage: kvpace [options]
 *   --driver NAME      protocol driver: resp | memcached | native (default: resp)
 *   --driver-opt K=V   driver option, repeatable (native: transport=tcp|rdma|auto, wait=poll|event, max-msg=N, depth=N)
 *   --host HOST        server address (default: 127.0.0.1)
 *   --port PORT        server port (default: 6379)
 *   --threads N        client threads, one connection each (default: 1)
 *   --duration SEC     measurement length (default: 10)
 *   --warmup SEC       warm-up length (default: 2)
 *   --keys N           key space size (default: 100000)
 *   --dist NAME        uniform | zipf (default: uniform)
 *   --theta X          Zipf skew in [0, 1) (default: 0.99)
 *   --value-size N     value size in bytes (default: 64)
 *   --read-ratio X     fraction of GET operations in [0, 1] (default: 0.9)
 *   --seed N           random seed (default: 1)
 *   --no-preload       skip writing the key space before the run
 *   --scenario NAME    kv | kvcache (default: kv). kvcache runs prefix-chain requests (src/kvcache.h)
 *   --block-size N     kvcache: bytes per block (default 16384)
 *   --groups N         kvcache: prefix groups (default 16)
 *   --shared-blocks N  kvcache: template blocks shared by a group (default 32)
 *   --turn-blocks N    kvcache: conversation blocks added per turn (default 8)
 *   --active-conv N    kvcache: conversation slots per thread (default 256)
 *   --max-turns N      kvcache: turns before a conversation restarts (default 16)
 *   --new-conv-prob X  kvcache: probability of starting a new conversation per request (default 0.05)
 *                      (--theta sets the Zipf skew of group and slot selection; default 0.99, use 0.9 for typical)
 *   --rate N           open loop: aggregate operations per second (default 0 = closed loop)
 *   --arrival NAME     open-loop inter-arrival times: constant | poisson (default: constant)
 *   --spin             open loop: busy-wait instead of sleeping until the intended send time
 *   --server-pid PID   host PID of the server; its CPU time and RSS are recorded
 *   --pin LIST         comma-separated CPU list; thread i uses entry i mod length
 *   --label TEXT       free-text name of the system under test, stored in the result
 *   --out FILE         write the JSON result to FILE
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "report.h"
#include "runner.h"

enum {
    OPT_DRIVER = 256,
    OPT_DRIVER_OPT,
    OPT_HOST,
    OPT_PORT,
    OPT_THREADS,
    OPT_DURATION,
    OPT_WARMUP,
    OPT_KEYS,
    OPT_DIST,
    OPT_THETA,
    OPT_VALUE_SIZE,
    OPT_READ_RATIO,
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
};

static const struct option long_opts[] = {
    {"driver", required_argument, NULL, OPT_DRIVER},
    {"driver-opt", required_argument, NULL, OPT_DRIVER_OPT},
    {"host", required_argument, NULL, OPT_HOST},
    {"port", required_argument, NULL, OPT_PORT},
    {"threads", required_argument, NULL, OPT_THREADS},
    {"duration", required_argument, NULL, OPT_DURATION},
    {"warmup", required_argument, NULL, OPT_WARMUP},
    {"keys", required_argument, NULL, OPT_KEYS},
    {"dist", required_argument, NULL, OPT_DIST},
    {"theta", required_argument, NULL, OPT_THETA},
    {"value-size", required_argument, NULL, OPT_VALUE_SIZE},
    {"read-ratio", required_argument, NULL, OPT_READ_RATIO},
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

int main(int argc, char **argv) {
    pace_config cfg = {
        .driver = &pace_driver_resp,
        .host = "127.0.0.1",
        .port = "6379",
        .threads = 1,
        .warmup_s = 2.0,
        .duration_s = 10.0,
        .nkeys = 100000,
        .dist = PACE_DIST_UNIFORM,
        .theta = 0.99,
        .value_size = 64,
        .read_ratio = 0.9,
        .seed = 1,
        .preload = 1,
    };
    pace_kvcache_defaults(&cfg.kvcache);
    const char *label = "unlabeled";
    const char *out_path = NULL;
    int *cpus = NULL;

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case OPT_DRIVER:
            cfg.driver = pace_driver_find(optarg);
            if (cfg.driver == NULL) {
                fprintf(stderr, "unknown driver: %s\n", optarg);
                return 2;
            }
            break;
        case OPT_DRIVER_OPT: {
            char *eq = strchr(optarg, '=');
            if (eq == NULL || cfg.driver->set_option == NULL) {
                fprintf(stderr, "invalid driver option (or driver without options): %s\n", optarg);
                return 2;
            }
            *eq = '\0';
            if (cfg.driver->set_option(optarg, eq + 1) != 0) {
                fprintf(stderr, "driver rejected option %s=%s\n", optarg, eq + 1);
                return 2;
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
        case OPT_DURATION:
            cfg.duration_s = strtod(optarg, NULL);
            break;
        case OPT_WARMUP:
            cfg.warmup_s = strtod(optarg, NULL);
            break;
        case OPT_KEYS:
            cfg.nkeys = strtoull(optarg, NULL, 10);
            break;
        case OPT_DIST:
            if (strcmp(optarg, "uniform") == 0) {
                cfg.dist = PACE_DIST_UNIFORM;
            } else if (strcmp(optarg, "zipf") == 0) {
                cfg.dist = PACE_DIST_ZIPF;
            } else {
                fprintf(stderr, "unknown distribution: %s\n", optarg);
                return 2;
            }
            break;
        case OPT_THETA:
            cfg.theta = strtod(optarg, NULL);
            break;
        case OPT_VALUE_SIZE:
            cfg.value_size = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case OPT_READ_RATIO:
            cfg.read_ratio = strtod(optarg, NULL);
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
                fprintf(stderr, "invalid CPU list: %s\n", optarg);
                return 2;
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
                fprintf(stderr, "unknown scenario: %s\n", optarg);
                return 2;
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
                fprintf(stderr, "unknown arrival process: %s\n", optarg);
                return 2;
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
        default:
            fprintf(stderr, "see the option list at the top of src/main.c\n");
            return 2;
        }
    }
    if (cfg.threads == 0 || cfg.nkeys == 0 || cfg.value_size == 0 || cfg.duration_s <= 0.0 ||
        cfg.read_ratio < 0.0 || cfg.read_ratio > 1.0 || cfg.rate < 0.0) {
        fprintf(stderr, "invalid numeric option\n");
        return 2;
    }

    cfg.kvcache.theta = cfg.theta < 1.0 ? cfg.theta : 0.9;
    cfg.kvcache.seed = cfg.seed;
    static pace_result res;
    if (pace_run(&cfg, &res) != 0) {
        return 1;
    }
    pace_report_text(stdout, &cfg, &res);
    if (out_path != NULL) {
        FILE *f = fopen(out_path, "w");
        if (f == NULL) {
            perror(out_path);
            return 1;
        }
        pace_report_json(f, &cfg, &res, label, argc, argv);
        fclose(f);
    }
    free(cpus);
    return res.errors > 0 ? 3 : 0;
}
