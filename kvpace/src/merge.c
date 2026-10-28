/*
 * merge.c - "kvpace merge": combines result files of load generators that ran at the same time (for
 * example on several hosts, started together with --start-at) into one result.
 *
 * Counters and histogram buckets are summed; the measurement time is the longest of the inputs, so the
 * throughput is the total operation count divided by it; percentiles are recomputed from the merged
 * histograms; the per-second series are added element by element. Minimum and maximum latencies come from
 * the input summaries. Server samples are per host and are not merged. Inputs must be schema 2 files
 * (they carry the histogram buckets).
 *
 * Inputs that record the wall-clock window of their measurement (measure_start_unix_ns,
 * measure_end_unix_ns) are checked for overlap: the merged file records the time during which all inputs
 * were measuring (tool.measure_overlap_s), and a warning is printed when it is shorter than 90% of the
 * shortest input, because the sum of the throughputs then describes no single interval.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hist.h"
#include "json.h"
#include "merge.h"
#include "report.h"
#include "workload.h"

#ifndef PACE_GIT
#define PACE_GIT "unknown"
#endif

/* Adds the buckets of a JSON [[index, count], ...] list and the summary's min, max and mean to h. */
static int add_hist(pace_hist *h, const pace_json *buckets, const pace_json *summary) {
    if (buckets == NULL || buckets->type != PJ_ARR) {
        return -1;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < buckets->n; i++) {
        const pace_json *pair = buckets->items[i];
        if (pair->type != PJ_ARR || pair->n != 2) {
            return -1;
        }
        uint64_t idx = pace_json_u64(pair->items[0]);
        uint64_t cnt = pace_json_u64(pair->items[1]);
        if (idx >= PACE_HIST_BUCKETS) {
            return -1;
        }
        h->counts[idx] += cnt;
        total += cnt;
    }
    if (total == 0) {
        return 0;
    }
    h->total += total;
    double mean = pace_json_num(summary, "mean", 0.0);
    h->sum += (uint64_t)(mean * (double)total);
    uint64_t mn = pace_json_u64(pace_json_get(summary, "min"));
    uint64_t mx = pace_json_u64(pace_json_get(summary, "max"));
    if (summary == NULL) { /* no summary: bucket bounds */
        for (unsigned b = 0; b < PACE_HIST_BUCKETS; b++) {
            if (h->counts[b] != 0) {
                mn = pace_hist_bucket_low(b);
                break;
            }
        }
        for (unsigned b = PACE_HIST_BUCKETS; b-- > 0;) {
            if (h->counts[b] != 0) {
                mx = pace_hist_bucket_high(b);
                break;
            }
        }
    }
    if (mn < h->min) {
        h->min = mn;
    }
    if (mx > h->max) {
        h->max = mx;
    }
    return 0;
}

static uint64_t u64_of(const pace_json *obj, const char *key) {
    return pace_json_u64(pace_json_get(obj, key));
}

int pace_merge_main(int argc, char **argv) {
    const char *out = NULL;
    int first = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (first == 0) {
            first = i;
        }
    }
    if (out == NULL || first == 0) {
        fprintf(stderr, "usage: kvpace merge --out MERGED.json RESULT.json [RESULT.json ...]\n");
        return 2;
    }
    pace_percentiles pc;
    pace_default_percentiles(&pc);
    static pace_hist hs[PACE_OP_COUNT];
    static pace_hist svc;
    static pace_hist req;
    for (unsigned k = 0; k < PACE_OP_COUNT; k++) {
        pace_hist_reset(&hs[k]);
    }
    pace_hist_reset(&svc);
    pace_hist_reset(&req);
    uint64_t ops_of[PACE_OP_COUNT] = {0};
    uint64_t gets = 0, puts = 0, misses = 0, errors = 0, mget_keys = 0, mget_hits = 0, max_lag = 0;
    uint64_t kv[7] = {0}; /* requests, block_gets, block_hits, block_puts, bytes_get, bytes_put, corrupt */
    uint64_t latest_start = 0;
    uint64_t earliest_end = UINT64_MAX;
    double shortest = 0.0;
    int have_window = 1; /* every input records its measurement window */
    int have_kv = 0;
    double seconds = 0.0;
    uint64_t series[PACE_MAX_SERIES] = {0};
    size_t nseries = 0;
    unsigned threads = 0;
    pace_json *base = NULL;
    int ninputs = 0;
    /* tool.timestamp of every input, in input order, so that the overlap of the runs can be checked */
    char(*stamps)[32] = calloc((size_t)argc, sizeof(*stamps));
    if (stamps == NULL) {
        return 1;
    }
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0) {
            i++;
            continue;
        }
        char err[160];
        pace_json *doc = pace_json_load(argv[i], err, sizeof(err));
        if (doc == NULL) {
            fprintf(stderr, "%s: %s\n", argv[i], err);
            free(stamps);
            return 1;
        }
        const pace_json *res = pace_json_get(doc, "results");
        const pace_json *hist = pace_json_get(res, "histograms");
        if (pace_json_num(doc, "schema", 0) < 2 || res == NULL || hist == NULL) {
            fprintf(stderr, "%s: not a kvpace result of schema 2 (histograms missing)\n", argv[i]);
            pace_json_free(doc);
            free(stamps);
            return 1;
        }
        const pace_json *lat = pace_json_get(res, "latency_ns");
        const pace_json *by = pace_json_get(res, "ops_by_type");
        for (unsigned k = 0; k < PACE_OP_COUNT; k++) {
            const char *name = pace_op_name((pace_op_type)k);
            if (add_hist(&hs[k], pace_json_get(hist, name), pace_json_get(lat, name)) != 0) {
                fprintf(stderr, "%s: bad histogram %s\n", argv[i], name);
                pace_json_free(doc);
                free(stamps);
                return 1;
            }
            ops_of[k] += u64_of(by, name);
        }
        const pace_json *kvc = pace_json_get(res, "kvcache");
        if (add_hist(&svc, pace_json_get(hist, "service"), pace_json_get(res, "service_time_ns")) != 0 ||
            add_hist(&req, pace_json_get(hist, "request"), pace_json_get(kvc, "request_latency_ns")) != 0) {
            fprintf(stderr, "%s: bad service or request histogram\n", argv[i]);
            pace_json_free(doc);
            free(stamps);
            return 1;
        }
        gets += u64_of(res, "gets");
        puts += u64_of(res, "puts");
        misses += u64_of(res, "misses");
        errors += u64_of(res, "errors");
        mget_keys += u64_of(res, "mget_keys");
        mget_hits += u64_of(res, "mget_hits");
        uint64_t lag = u64_of(res, "max_lag_ns");
        max_lag = lag > max_lag ? lag : max_lag;
        double s = pace_json_num(res, "seconds", 0.0);
        seconds = s > seconds ? s : seconds;
        uint64_t ws = u64_of(res, "measure_start_unix_ns");
        uint64_t we = u64_of(res, "measure_end_unix_ns");
        if (ws == 0 || we <= ws) {
            have_window = 0;
        } else {
            latest_start = ws > latest_start ? ws : latest_start;
            earliest_end = we < earliest_end ? we : earliest_end;
            double len = (double)(we - ws) / 1e9;
            shortest = ninputs == 0 || len < shortest ? len : shortest;
        }
        if (kvc != NULL) {
            static const char *const f[7] = {"requests",  "block_gets", "block_hits", "block_puts",
                                             "bytes_get", "bytes_put",  "corrupt"};
            for (int k = 0; k < 7; k++) {
                kv[k] += u64_of(kvc, f[k]);
            }
            have_kv = 1;
        }
        const pace_json *ser = pace_json_get(res, "series");
        for (size_t k = 0; ser != NULL && k < ser->n && k < PACE_MAX_SERIES; k++) {
            series[k] += pace_json_u64(ser->items[k]);
            nseries = k + 1 > nseries ? k + 1 : nseries;
        }
        threads += (unsigned)pace_json_num(pace_json_get(doc, "config"), "threads", 0);
        const pace_json *ts = pace_json_get(pace_json_get(doc, "tool"), "timestamp");
        snprintf(stamps[ninputs], sizeof(stamps[ninputs]), "%s",
                 ts != NULL && ts->type == PJ_STR ? ts->text : "");
        ninputs++;
        if (base == NULL) {
            base = doc;
        } else {
            pace_json_free(doc);
        }
    }
    FILE *f = fopen(out, "w");
    if (f == NULL) {
        perror(out);
        pace_json_free(base);
        free(stamps);
        return 1;
    }
    char when[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
    uint64_t ops = 0;
    for (unsigned k = 0; k < PACE_OP_COUNT; k++) {
        ops += ops_of[k];
    }
    fprintf(f, "{\n  \"schema\": 2,\n  \"tool\": {\"name\": \"kvpace\", \"commit\": ");
    pace_json_str(f, PACE_GIT);
    fprintf(f, ", \"timestamp\": \"%s\", \"merged_from\": [", when);
    int k = 0;
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0) {
            i++;
            continue;
        }
        fprintf(f, "%s", k++ ? ", " : "");
        pace_json_str(f, argv[i]);
    }
    fputs("], \"input_timestamps\": [", f);
    for (int i = 0; i < ninputs; i++) {
        fputs(i ? ", " : "", f);
        pace_json_str(f, stamps[i]);
    }
    free(stamps);
    double overlap =
        have_window && earliest_end > latest_start ? (double)(earliest_end - latest_start) / 1e9 : 0.0;
    fprintf(f, "], \"inputs\": %d, \"threads_total\": %u, \"measure_overlap_s\": ", ninputs, threads);
    if (have_window) {
        fprintf(f, "%.3f", overlap);
    } else {
        fputs("null", f);
    }
    fputs("},\n  \"environment\": ", f);
    pace_json_write(f, pace_json_get(base, "environment"));
    fputs(",\n  \"config\": ", f);
    pace_json_write(f, pace_json_get(base, "config"));
    fprintf(f,
            ",\n  \"results\": {\n    \"seconds\": %.3f, \"ops\": %llu, \"gets\": %llu, \"puts\": %llu, "
            "\"misses\": "
            "%llu, \"errors\": %llu, \"ops_per_sec\": %.1f, \"mget_keys\": %llu, \"mget_hits\": %llu,\n"
            "    \"ops_by_type\": {",
            seconds, (unsigned long long)ops, (unsigned long long)gets, (unsigned long long)puts,
            (unsigned long long)misses, (unsigned long long)errors, seconds > 0 ? (double)ops / seconds : 0.0,
            (unsigned long long)mget_keys, (unsigned long long)mget_hits);
    for (unsigned t = 0; t < PACE_OP_COUNT; t++) {
        fprintf(f, "%s\"%s\": %llu", t ? ", " : "", pace_op_name((pace_op_type)t),
                (unsigned long long)ops_of[t]);
    }
    fputs("},\n    \"get_latency_ns\": ", f);
    pace_json_hist(f, &hs[PACE_OP_GET], &pc);
    fputs(",\n    \"put_latency_ns\": ", f);
    pace_json_hist(f, &hs[PACE_OP_SET], &pc);
    fputs(",\n    \"service_time_ns\": ", f);
    pace_json_hist(f, &svc, &pc);
    fputs(",\n    \"latency_ns\": {", f);
    int firsth = 1;
    for (unsigned t = 0; t < PACE_OP_COUNT; t++) {
        if (hs[t].total == 0) {
            continue;
        }
        fprintf(f, "%s\"%s\": ", firsth ? "" : ", ", pace_op_name((pace_op_type)t));
        pace_json_hist(f, &hs[t], &pc);
        firsth = 0;
    }
    fprintf(f, "},\n    \"max_lag_ns\": %llu,\n    \"series\": [", (unsigned long long)max_lag);
    for (size_t t = 0; t < nseries; t++) {
        fprintf(f, "%s%llu", t ? ", " : "", (unsigned long long)series[t]);
    }
    fputs("]", f);
    if (have_kv) {
        fprintf(f,
                ",\n    \"kvcache\": {\"requests\": %llu, \"block_gets\": %llu, \"block_hits\": %llu, "
                "\"hit_rate\": "
                "%.4f, \"reuse_rate\": %.4f, \"block_puts\": %llu, \"bytes_get\": %llu, \"bytes_put\": %llu, "
                "\"corrupt\": %llu, \"request_latency_ns\": ",
                (unsigned long long)kv[0], (unsigned long long)kv[1], (unsigned long long)kv[2],
                kv[1] ? (double)kv[2] / (double)kv[1] : 0.0,
                kv[2] + kv[3] ? (double)kv[2] / (double)(kv[2] + kv[3]) : 0.0, (unsigned long long)kv[3],
                (unsigned long long)kv[4], (unsigned long long)kv[5], (unsigned long long)kv[6]);
        pace_json_hist(f, &req, &pc);
        fputs("}", f);
    }
    fputs(",\n    \"histograms\": {\"service\": ", f);
    pace_json_buckets(f, &svc);
    fputs(", \"request\": ", f);
    pace_json_buckets(f, &req);
    for (unsigned t = 0; t < PACE_OP_COUNT; t++) {
        fprintf(f, ", \"%s\": ", pace_op_name((pace_op_type)t));
        pace_json_buckets(f, &hs[t]);
    }
    fputs("}\n  }\n}\n", f);
    fclose(f);
    pace_json_free(base);
    printf("merged %d results: %llu ops in %.2f s -> %.0f ops/s\n", ninputs, (unsigned long long)ops, seconds,
           seconds > 0 ? (double)ops / seconds : 0.0);
    if (have_window) {
        printf("measured together for %.2f s (shortest input %.2f s)\n", overlap, shortest);
        if (overlap < 0.9 * shortest) {
            fprintf(
                stderr,
                "warning: the inputs measured together for only %.2f s of %.2f s; their throughputs do not "
                "add up to one interval (start them together: --start-at or --coordinator)\n",
                overlap, shortest);
        }
    }
    return 0;
}
