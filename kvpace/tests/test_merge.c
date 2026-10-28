/*
 * test_merge.c - result files, kvpace merge, CSV and HdrHistogram outputs.
 *
 * Two synthetic results with known histograms are written as JSON and merged. The merged file must hold
 * the summed counters, the input timestamps, the longer measurement time, the element-wise sum of the per-second series, the
 * summed histogram buckets, and percentiles equal to those of the histograms merged in memory. A schema-1
 * file is refused. Inputs with measurement windows record their overlap (none without windows, the
 * common part of overlapping windows, zero for disjoint ones), and KV-cache counters including corrupt
 * blocks are summed. The CSV and .hgrm writers are checked for their header and the final percentile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "merge.h"
#include "report.h"
#include "runner.h"

static int failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

static pace_result ra;
static pace_result rb;
static pace_hist merged;

static void fill(pace_result *r, unsigned seed, uint64_t base_ns, unsigned n, double seconds) {
    memset(r, 0, sizeof(*r));
    for (unsigned k = 0; k < PACE_OP_COUNT; k++) {
        pace_hist_reset(&r->lat[k]);
    }
    pace_hist_reset(&r->svc_lat);
    pace_hist_reset(&r->req_lat);
    uint64_t x = seed;
    for (unsigned i = 0; i < n; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t v = base_ns + (x >> 40) % 1000000;
        pace_hist_record(&r->lat[PACE_OP_GET], v);
        pace_hist_record(&r->svc_lat, v / 2);
        r->ops_of[PACE_OP_GET]++;
    }
    r->gets = n;
    r->seconds = seconds;
    r->nseries = (unsigned)seconds;
    for (unsigned i = 0; i < r->nseries; i++) {
        r->series[i] = n / r->nseries + i;
    }
}

static void write_json(const char *path, const pace_config *cfg, const pace_result *r,
                       const pace_percentiles *pc) {
    FILE *f = fopen(path, "w");
    char *argv[2] = {"kvpace", "--test"};
    pace_report_json(f, cfg, r, pc, "test", 2, argv);
    fclose(f);
}

int main(void) {
    pace_config cfg;
    pace_config_default(&cfg);
    pace_percentiles pc;
    pace_default_percentiles(&pc);
    fill(&ra, 1, 10000, 30000, 2.0);
    fill(&rb, 2, 500000, 50000, 3.0);
    pace_hist_reset(&merged);
    pace_hist_merge(&merged, &ra.lat[PACE_OP_GET]);
    pace_hist_merge(&merged, &rb.lat[PACE_OP_GET]);

    char dir[] = "/tmp/pace-merge-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        return 1;
    }
    char a[256];
    char b[256];
    char out[256];
    char old[256];
    snprintf(a, sizeof(a), "%s/a.json", dir);
    snprintf(b, sizeof(b), "%s/b.json", dir);
    snprintf(out, sizeof(out), "%s/m.json", dir);
    snprintf(old, sizeof(old), "%s/old.json", dir);
    write_json(a, &cfg, &ra, &pc);
    write_json(b, &cfg, &rb, &pc);

    char *argv[] = {"merge", "--out", out, a, b};
    EXPECT(pace_merge_main(5, argv) == 0, "merge");
    char err[160];
    pace_json *m = pace_json_load(out, err, sizeof(err));
    EXPECT(m != NULL, "merged file parses: %s", err);
    if (m != NULL) {
        const pace_json *res = pace_json_get(m, "results");
        EXPECT(pace_json_u64(pace_json_get(res, "ops")) == 80000, "ops summed");
        EXPECT(pace_json_num(res, "seconds", 0) == 3.0, "longest measurement time");
        const pace_json *get = pace_json_get(pace_json_get(res, "latency_ns"), "get");
        uint64_t p99 = pace_json_u64(pace_json_get(get, "p99"));
        uint64_t p50 = pace_json_u64(pace_json_get(get, "p50"));
        EXPECT(p99 == pace_hist_quantile(&merged, 0.99) && p50 == pace_hist_quantile(&merged, 0.5),
               "percentiles of the merged histogram (p50 %llu p99 %llu)", (unsigned long long)p50,
               (unsigned long long)p99);
        EXPECT(pace_json_u64(pace_json_get(get, "count")) == 80000, "histogram count");
        const pace_json *ser = pace_json_get(res, "series");
        EXPECT(ser != NULL && ser->n == 3 && pace_json_u64(ser->items[0]) == 15000 + 16666 &&
                   pace_json_u64(ser->items[2]) == 16666 + 2,
               "series summed element-wise");
        const pace_json *inputs = pace_json_get(pace_json_get(m, "tool"), "inputs");
        EXPECT(pace_json_u64(inputs) == 2, "two inputs recorded");
        const pace_json *ov = pace_json_get(pace_json_get(m, "tool"), "measure_overlap_s");
        EXPECT(ov != NULL && ov->type == PJ_NULL, "no overlap without measurement windows");
        const pace_json *stamps = pace_json_get(pace_json_get(m, "tool"), "input_timestamps");
        EXPECT(stamps != NULL && stamps->n == 2 && stamps->items[0]->type == PJ_STR &&
                   strlen(stamps->items[0]->text) == 20,
               "input timestamps recorded");
        /* merging the merged file with itself doubles every counter and keeps the percentiles */
        char twice[256];
        snprintf(twice, sizeof(twice), "%s/t.json", dir);
        char *argv2[] = {"merge", "--out", twice, out, out};
        EXPECT(pace_merge_main(5, argv2) == 0, "merge of merged files");
        pace_json *t = pace_json_load(twice, err, sizeof(err));
        const pace_json *tres = pace_json_get(t, "results");
        const pace_json *tget = pace_json_get(pace_json_get(tres, "latency_ns"), "get");
        EXPECT(t != NULL && pace_json_u64(pace_json_get(tres, "ops")) == 160000 &&
                   pace_json_u64(pace_json_get(tget, "p99")) == p99,
               "idempotent percentiles");
        pace_json_free(t);
        pace_json_free(m);
    }
    FILE *f = fopen(old, "w");
    fputs("{\"schema\": 1, \"results\": {\"ops\": 1}}\n", f);
    fclose(f);
    char *argv3[] = {"merge", "--out", out, old};
    EXPECT(pace_merge_main(4, argv3) != 0, "schema 1 refused");

    /* measurement windows: [1000 s, 1002 s] and [1000.5 s, 1003.5 s] overlap for 1.5 s; [1005 s, 1008 s] not */
    static const uint64_t s = 1000000000ull;
    ra.wall_start_ns = 1000 * s;
    ra.wall_end_ns = 1002 * s;
    rb.wall_start_ns = 1000 * s + s / 2;
    rb.wall_end_ns = 1003 * s + s / 2;
    write_json(a, &cfg, &ra, &pc);
    write_json(b, &cfg, &rb, &pc);
    char *argv4[] = {"merge", "--out", out, a, b};
    EXPECT(pace_merge_main(5, argv4) == 0, "merge with windows");
    pace_json *w = pace_json_load(out, err, sizeof(err));
    EXPECT(w != NULL && pace_json_num(pace_json_get(w, "tool"), "measure_overlap_s", -1) == 1.5,
           "overlap 1.5 s");
    pace_json_free(w);
    rb.wall_start_ns = 1005 * s;
    rb.wall_end_ns = 1008 * s;
    write_json(b, &cfg, &rb, &pc);
    EXPECT(pace_merge_main(5, argv4) == 0, "merge of disjoint runs (warning only)");
    w = pace_json_load(out, err, sizeof(err));
    EXPECT(w != NULL && pace_json_num(pace_json_get(w, "tool"), "measure_overlap_s", -1) == 0.0,
           "no overlap");
    pace_json_free(w);

    /* KV-cache counters, including corrupt blocks, are summed */
    pace_config kcfg = cfg;
    kcfg.scenario = PACE_SCENARIO_KVCACHE;
    ra.kvcache = (pace_kvcache_stats){
        .requests = 10, .block_gets = 40, .block_hits = 30, .block_puts = 12, .corrupt = 2};
    rb.kvcache = (pace_kvcache_stats){
        .requests = 5, .block_gets = 20, .block_hits = 10, .block_puts = 8, .corrupt = 3};
    write_json(a, &kcfg, &ra, &pc);
    write_json(b, &kcfg, &rb, &pc);
    EXPECT(pace_merge_main(5, argv4) == 0, "merge of kvcache results");
    w = pace_json_load(out, err, sizeof(err));
    const pace_json *kv = pace_json_get(pace_json_get(w, "results"), "kvcache");
    EXPECT(w != NULL && pace_json_u64(pace_json_get(kv, "requests")) == 15 &&
               pace_json_u64(pace_json_get(kv, "block_hits")) == 40 &&
               pace_json_u64(pace_json_get(kv, "corrupt")) == 5,
           "kvcache counters summed (corrupt %llu)",
           (unsigned long long)pace_json_u64(pace_json_get(kv, "corrupt")));
    pace_json_free(w);

    /* CSV and HdrHistogram outputs */
    char csv[256];
    snprintf(csv, sizeof(csv), "%s/r.csv", dir);
    f = fopen(csv, "w");
    pace_report_csv(f, &cfg, &ra, &pc);
    fclose(f);
    f = fopen(csv, "r");
    char line[512];
    EXPECT(fgets(line, sizeof(line), f) != NULL && strncmp(line, "driver,operation,count", 22) == 0,
           "csv header");
    EXPECT(fgets(line, sizeof(line), f) != NULL && strncmp(line, "resp,get,30000,", 15) == 0, "csv row: %s",
           line);
    fclose(f);
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s/h", dir);
    EXPECT(pace_report_hgrm(prefix, &ra) == 0, "hgrm written");
    char hgrm[300];
    snprintf(hgrm, sizeof(hgrm), "%s_get.hgrm", prefix);
    f = fopen(hgrm, "r");
    int has_header = 0;
    int has_total = 0;
    double last_p = 0;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        has_header |= strstr(line, "Percentile") != NULL;
        has_total |= strstr(line, "Total count    =        30000") != NULL;
        double v;
        double p;
        if (sscanf(line, "%lf %lf", &v, &p) == 2) {
            last_p = p;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    EXPECT(has_header && has_total && last_p == 1.0, "hgrm format (header %d, total %d, last percentile %g)",
           has_header, has_total, last_p);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        EXPECT(0, "cleanup");
    }
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_merge: ok");
    return 0;
}
