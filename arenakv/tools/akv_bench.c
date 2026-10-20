/*
 * akv_bench.c - engine micro-benchmarks (no network).
 *
 * Usage:
 *   akv_bench scale [--threads LIST] [--cpus LIST] [--value-size N] [--read-ratio X] [--keys N]
 *                   [--duration SEC] [--warmup SEC] [--shards N] [--out FILE]
 *   akv_bench micro [--out FILE]
 *
 * scale: operations per second of the engine with N threads, each running a closed loop over uniformly
 *   distributed keys. The key space is preloaded and fits in memory, so every GET hits. Threads are pinned
 *   to the listed CPUs (thread i uses entry i mod length). One JSON object per thread count.
 * micro: cost per call of the 16-lane tag comparison (vector implementation and scalar reference) and
 *   throughput of the key hash for several key lengths.
 *
 * Output is a JSON document: {"tool": "akv_bench", "mode": ..., "results": [...]}. Environment data (CPU,
 * kernel, compiler) is recorded by the caller (benchmarks/scripts/env-info.sh), not by this tool.
 */
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hash.h"
#include "akv.h"
#include "simd.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline uint64_t xorshift(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static size_t make_key(uint64_t id, char *key) {
    static const char hex[] = "0123456789abcdef";
    key[0] = 'k';
    for (int i = 0; i < 15; i++) {
        key[1 + i] = hex[(id >> (4 * (14 - i))) & 15];
    }
    return 16;
}

/* ---- scale ---------------------------------------------------------------------------------------- */

struct worker {
    akv *s;
    unsigned id;
    int cpu;
    uint64_t keys;
    uint32_t value_size;
    double read_ratio;
    atomic_int *measure;
    atomic_int *stop;
    uint64_t ops;
    uint64_t hits;
};

static void *worker_main(void *arg) {
    struct worker *w = arg;
    if (w->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(w->cpu, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    uint8_t *val = malloc(w->value_size);
    uint8_t *out = malloc((size_t)w->value_size + 64);
    memset(val, 0x5a, w->value_size);
    uint64_t rng = 0x9e3779b97f4a7c15ull * (w->id + 1);
    uint64_t threshold = (uint64_t)(w->read_ratio * 4294967296.0);
    uint64_t ops = 0;
    uint64_t hits = 0;
    int counting = 0;
    char key[16];
    while (!atomic_load_explicit(w->stop, memory_order_relaxed)) {
        for (int i = 0; i < 256; i++) {
            uint64_t r = xorshift(&rng);
            size_t klen = make_key((r >> 8) % w->keys, key);
            if ((r & 0xffffffffu) < threshold) {
                size_t n;
                hits += akv_get(w->s, key, klen, out, w->value_size + 64, &n) == 0;
            } else {
                akv_put(w->s, key, klen, val, w->value_size);
            }
        }
        if (counting) {
            ops += 256;
        } else if (atomic_load_explicit(w->measure, memory_order_relaxed)) {
            counting = 1;
            hits = 0;
        }
    }
    w->ops = ops;
    w->hits = hits;
    free(val);
    free(out);
    return NULL;
}

static int parse_list(const char *s, int *out, int max) {
    int n = 0;
    char *copy = strdup(s);
    for (char *t = strtok(copy, ","); t != NULL && n < max; t = strtok(NULL, ",")) {
        out[n++] = atoi(t);
    }
    free(copy);
    return n;
}

static int cmd_scale(int argc, char **argv, FILE *out) {
    int threads[32] = {1, 2, 4, 8};
    int nthreads = 4;
    int cpus[64];
    int ncpus = 0;
    uint32_t value_size = 64;
    double read_ratio = 0.9;
    uint64_t keys = 1000000;
    double duration = 3.0;
    double warmup = 1.0;
    uint32_t shards = 64;
    static const struct option lo[] = {
        {"threads", required_argument, NULL, 't'},    {"cpus", required_argument, NULL, 'c'},
        {"value-size", required_argument, NULL, 'v'}, {"read-ratio", required_argument, NULL, 'r'},
        {"keys", required_argument, NULL, 'k'},       {"duration", required_argument, NULL, 'd'},
        {"warmup", required_argument, NULL, 'w'},     {"shards", required_argument, NULL, 's'},
        {"out", required_argument, NULL, 'o'},        {NULL, 0, NULL, 0}};
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 't':
            nthreads = parse_list(optarg, threads, 32);
            break;
        case 'c':
            ncpus = parse_list(optarg, cpus, 64);
            break;
        case 'v':
            value_size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'r':
            read_ratio = strtod(optarg, NULL);
            break;
        case 'k':
            keys = strtoull(optarg, NULL, 10);
            break;
        case 'd':
            duration = strtod(optarg, NULL);
            break;
        case 'w':
            warmup = strtod(optarg, NULL);
            break;
        case 's':
            shards = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'o':
            break; /* handled by main */
        default:
            return 2;
        }
    }
    fprintf(out,
            "{\"tool\": \"akv_bench\", \"mode\": \"scale\", \"simd\": \"%s\", \"keys\": %llu, "
            "\"value_size\": %u, "
            "\"read_ratio\": %.3f, \"shards\": %u, \"duration_s\": %.1f, \"results\": [\n",
            akv_simd_name(), (unsigned long long)keys, value_size, read_ratio, shards, duration);
    for (int ti = 0; ti < nthreads; ti++) {
        int nt = threads[ti];
        akv_config cfg;
        akv_config_default(&cfg);
        cfg.shards = shards;
        cfg.capacity = (uint64_t)keys * (64 + value_size) * 2 + (64ull << 20);
        cfg.avg_entry = 64 + value_size;
        cfg.hash_seed = 42;
        akv *s = akv_create(&cfg);
        if (s == NULL) {
            fprintf(stderr, "akv_create failed\n");
            return 1;
        }
        uint8_t *val = malloc(value_size);
        memset(val, 0x5a, value_size);
        char key[16];
        for (uint64_t i = 0; i < keys; i++) {
            akv_put(s, key, make_key(i, key), val, value_size);
        }
        free(val);
        atomic_int measure = 0;
        atomic_int stop = 0;
        struct worker ws[64];
        pthread_t th[64];
        for (int i = 0; i < nt; i++) {
            ws[i] = (struct worker){.s = s,
                                    .id = (unsigned)i,
                                    .cpu = ncpus ? cpus[i % ncpus] : -1,
                                    .keys = keys,
                                    .value_size = value_size,
                                    .read_ratio = read_ratio,
                                    .measure = &measure,
                                    .stop = &stop};
            pthread_create(&th[i], NULL, worker_main, &ws[i]);
        }
        struct timespec ts = {.tv_sec = (time_t)warmup, .tv_nsec = (long)((warmup - (time_t)warmup) * 1e9)};
        nanosleep(&ts, NULL);
        double t0 = now_s();
        atomic_store(&measure, 1);
        ts = (struct timespec){.tv_sec = (time_t)duration,
                               .tv_nsec = (long)((duration - (time_t)duration) * 1e9)};
        nanosleep(&ts, NULL);
        atomic_store(&stop, 1);
        double elapsed = now_s() - t0;
        uint64_t total = 0;
        for (int i = 0; i < nt; i++) {
            pthread_join(th[i], NULL);
            total += ws[i].ops;
        }
        akv_stats st;
        akv_stats_get(s, &st);
        fprintf(
            out,
            "%s  {\"threads\": %d, \"ops_per_sec\": %.0f, \"ns_per_op_per_thread\": %.1f, \"entries\": %llu, "
            "\"evictions\": %llu}",
            ti ? ",\n" : "", nt, (double)total / elapsed, elapsed * 1e9 * nt / (double)total,
            (unsigned long long)st.entries, (unsigned long long)st.evictions);
        fflush(out);
        akv_destroy(s);
    }
    fprintf(out, "\n]}\n");
    return 0;
}

/* ---- micro ---------------------------------------------------------------------------------------- */

static int cmd_micro(FILE *out) {
    enum { SETS = 4096 };
    static uint16_t tags[SETS][16] __attribute__((aligned(64)));
    uint64_t rng = 12345;
    for (int i = 0; i < SETS; i++) {
        for (int j = 0; j < 16; j++) {
            tags[i][j] =
                (uint16_t)(xorshift(&rng) & 0x1f); /* 32 distinct values: about one match in two buckets */
        }
    }
    const uint64_t iters = 200000000ull;
    uint64_t sink = 0;
    double t0 = now_s();
    for (uint64_t i = 0; i < iters; i++) {
        sink += akv_match16(tags[i & (SETS - 1)], (uint16_t)(i * 7 & 0x1f));
    }
    double vec_ns = (now_s() - t0) * 1e9 / (double)iters;
    t0 = now_s();
    for (uint64_t i = 0; i < iters / 4; i++) {
        sink += akv_match16_scalar(tags[i & (SETS - 1)], (uint16_t)(i * 7 & 0x1f));
    }
    double scalar_ns = (now_s() - t0) * 1e9 / (double)(iters / 4);

    fprintf(out,
            "{\"tool\": \"akv_bench\", \"mode\": \"micro\", \"simd\": \"%s\", \"match16\": "
            "{\"vector_ns_per_call\": %.3f, "
            "\"scalar_ns_per_call\": %.3f, \"speedup\": %.2f}, \"hash\": [\n",
            akv_simd_name(), vec_ns, scalar_ns, scalar_ns / vec_ns);
    static const size_t lens[] = {8, 16, 32, 64, 256, 1024, 4096};
    static uint8_t buf[8192] __attribute__((aligned(64)));
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)xorshift(&rng);
    }
    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        size_t len = lens[li];
        uint64_t n = 40000000000ull / (len + 64) / 10;
        uint64_t h = 0;
        t0 = now_s();
        for (uint64_t i = 0; i < n; i++) {
            h += akv_hash64(buf + (i & 63), len, h);
        }
        double dt = now_s() - t0;
        sink += h;
        fprintf(out, "%s  {\"key_len\": %zu, \"ns_per_hash\": %.2f, \"gb_per_s\": %.2f}", li ? ",\n" : "",
                len, dt * 1e9 / (double)n, (double)len * (double)n / dt / 1e9);
    }
    fprintf(out, "\n], \"sink\": %llu}\n", (unsigned long long)(sink & 1));
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s scale|micro [options]\n", argv[0]);
        return 2;
    }
    const char *out_path = NULL;
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--out") == 0) {
            out_path = argv[i + 1];
        }
    }
    FILE *out = out_path ? fopen(out_path, "w") : stdout;
    if (out == NULL) {
        perror(out_path);
        return 1;
    }
    int rc;
    if (strcmp(argv[1], "scale") == 0) {
        rc = cmd_scale(argc - 1, argv + 1, out);
    } else if (strcmp(argv[1], "micro") == 0) {
        rc = cmd_micro(out);
    } else {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        rc = 2;
    }
    if (out_path) {
        fclose(out);
    }
    return rc;
}
