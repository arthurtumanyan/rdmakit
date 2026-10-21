/* report.c - result writers, see report.h. */
#include "report.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "sock.h"

#ifndef PACE_GIT
#define PACE_GIT "unknown"
#endif

static const char *const dist_names[] = {"uniform", "zipf", "sequential", "gaussian", "latest"};

/* Copies the first "model name" (or "Model") value from /proc/cpuinfo into out. */
static void cpu_model(char *out, size_t cap) {
    snprintf(out, cap, "unknown");
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Model", 5) == 0) {
            char *colon = strchr(line, ':');
            if (colon != NULL) {
                snprintf(out, cap, "%s", colon + 2);
                out[strcspn(out, "\n")] = '\0';
                break;
            }
        }
    }
    fclose(f);
}

void pace_default_percentiles(pace_percentiles *p) {
    static const double d[] = {0.5, 0.9, 0.99, 0.999, 0.9999};
    p->n = sizeof(d) / sizeof(d[0]);
    memcpy(p->q, d, sizeof(d));
}

int pace_parse_percentiles(const char *s, pace_percentiles *p) {
    char buf[256];
    if (strlen(s) >= sizeof(buf)) {
        return -1;
    }
    strcpy(buf, s);
    p->n = 0;
    for (char *tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        char *end;
        double v = strtod(tok, &end);
        if (end == tok || *end != '\0' || v <= 0.0 || v >= 100.0 || p->n == PACE_MAX_PERCENTILES) {
            return -1;
        }
        p->q[p->n++] = v / 100.0;
    }
    return p->n > 0 ? 0 : -1;
}

/* Name of a percentile for JSON and CSV: 0.999 -> "p99.9", 0.5 -> "p50". */
static void pname(double q, char *out, size_t cap) {
    snprintf(out, cap, "p%.10g", q * 100.0);
}

void pace_json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fprintf(f, "\\%c", c);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

void pace_json_hist(FILE *f, const pace_hist *h, const pace_percentiles *pc) {
    fprintf(f,
            "{\"count\": %llu, \"min\": %llu, \"mean\": %.1f, \"p50\": %llu, \"p90\": %llu, \"p99\": %llu, "
            "\"p999\": %llu, \"max\": %llu, \"percentiles\": {",
            (unsigned long long)h->total, (unsigned long long)(h->total ? h->min : 0), pace_hist_mean(h),
            (unsigned long long)pace_hist_quantile(h, 0.50), (unsigned long long)pace_hist_quantile(h, 0.90),
            (unsigned long long)pace_hist_quantile(h, 0.99), (unsigned long long)pace_hist_quantile(h, 0.999),
            (unsigned long long)(h->total ? h->max : 0));
    for (unsigned i = 0; i < pc->n; i++) {
        char name[32];
        pname(pc->q[i], name, sizeof(name));
        fprintf(f, "%s\"%s\": %llu", i ? ", " : "", name,
                (unsigned long long)pace_hist_quantile(h, pc->q[i]));
    }
    fputs("}}", f);
}

void pace_json_buckets(FILE *f, const pace_hist *h) {
    fputc('[', f);
    int first = 1;
    for (unsigned i = 0; i < PACE_HIST_BUCKETS; i++) {
        if (h->counts[i] != 0) {
            fprintf(f, "%s[%u, %llu]", first ? "" : ", ", i, (unsigned long long)h->counts[i]);
            first = 0;
        }
    }
    fputc(']', f);
}

static void json_named_hist(FILE *f, const char *name, const pace_hist *h, const pace_percentiles *pc) {
    fprintf(f, "    \"%s\": ", name);
    pace_json_hist(f, h, pc);
}

/* Writes the value size specification as text. */
static void vsize_text(const pace_workload *w, char *out, size_t cap) {
    if (w->vmode == PACE_VSIZE_FIXED) {
        snprintf(out, cap, "%u", w->vmin);
    } else if (w->vmode == PACE_VSIZE_RANGE) {
        snprintf(out, cap, "%u-%u", w->vmin, w->vmax);
    } else {
        size_t off = 0;
        double prev = 0.0;
        for (unsigned i = 0; i < w->nvsizes && off < cap; i++) {
            off += (size_t)snprintf(out + off, cap - off, "%s%u:%.4g", i ? "," : "", w->vsizes[i],
                                    w->vcum[i] - prev);
            prev = w->vcum[i];
        }
    }
}

void pace_report_json(FILE *f, const pace_config *cfg, const pace_result *res, const pace_percentiles *pc,
                      const char *label, int argc, char **argv) {
    struct utsname un;
    uname(&un);
    char cpu[160];
    cpu_model(cpu, sizeof(cpu));
    char when[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
    const pace_workload *w = &cfg->wl;
    double read_ratio = w->weight[PACE_OP_GET] / (w->cum[PACE_OP_COUNT - 1] > 0 ? 1.0 : 1.0);
    double sumw = 0.0;
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        sumw += w->weight[i];
    }
    read_ratio = sumw > 0 ? w->weight[PACE_OP_GET] / sumw : 0.0;

    fprintf(f, "{\n  \"schema\": 2,\n  \"tool\": {\"name\": \"kvpace\", \"commit\": ");
    pace_json_str(f, PACE_GIT);
    fprintf(f, ", \"timestamp\": \"%s\"},\n  \"environment\": {\"cpu\": ", when);
    pace_json_str(f, cpu);
    fprintf(f, ", \"logical_cpus\": %ld, \"kernel\": ", sysconf(_SC_NPROCESSORS_ONLN));
    pace_json_str(f, un.release);
    fprintf(f, ", \"machine\": ");
    pace_json_str(f, un.machine);
    fprintf(f, ", \"compiler\": ");
    pace_json_str(f, __VERSION__);
    fprintf(f, "},\n  \"config\": {\"label\": ");
    pace_json_str(f, label);
    fprintf(f, ", \"driver\": ");
    pace_json_str(f, cfg->driver->name);
    fprintf(f, ", \"host\": ");
    pace_json_str(f, cfg->host);
    fprintf(f, ", \"port\": ");
    pace_json_str(f, cfg->port);
    char vs[256];
    vsize_text(w, vs, sizeof(vs));
    fprintf(
        f,
        ", \"threads\": %u, \"conns_per_thread\": %u, \"pipeline\": %u, \"warmup_s\": %.1f, \"duration_s\": "
        "%.1f, \"requests\": %llu, \"keys\": %llu, \"dist\": \"%s\", \"theta\": %.3f, \"value_size\": %u, "
        "\"value_sizes\": \"%s\", \"read_ratio\": %.3f, \"mix\": {",
        cfg->threads, cfg->conns_per_thread ? cfg->conns_per_thread : 1, cfg->pipeline ? cfg->pipeline : 1,
        cfg->warmup_s, cfg->duration_s, (unsigned long long)cfg->requests, (unsigned long long)w->nkeys,
        dist_names[w->dist], w->theta, pace_wl_vsize_max(w), vs, read_ratio);
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        fprintf(f, "%s\"%s\": %.4g", i ? ", " : "", pace_op_name((pace_op_type)i),
                sumw > 0 ? w->weight[i] / sumw : 0);
    }
    fprintf(f, "}, \"mget_keys\": %u, \"key_prefix\": ", w->mget_keys);
    pace_json_str(f, w->prefix);
    fprintf(
        f,
        ", \"tls\": %s, \"auth\": %s, \"seed\": %llu, \"preload\": %d, \"rate\": %.1f, \"arrival\": \"%s\", "
        "\"argv\": [",
        pace_tls_enabled() ? "true" : "false", pace_driver_auth() != NULL ? "true" : "false",
        (unsigned long long)cfg->seed, cfg->preload, cfg->rate,
        cfg->arrival == PACE_ARRIVAL_POISSON ? "poisson" : "constant");
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            fputs(", ", f);
        }
        /* Credentials are not written to result files. */
        if (i > 0 && strcmp(argv[i - 1], "--auth") == 0) {
            pace_json_str(f, "(redacted)");
        } else {
            pace_json_str(f, argv[i]);
        }
    }
    uint64_t ops = pace_result_ops(res);
    fprintf(f,
            "]},\n  \"results\": {\n    \"seconds\": %.3f, \"ops\": %llu, \"gets\": %llu, \"puts\": %llu, "
            "\"misses\": %llu, \"errors\": %llu, \"ops_per_sec\": %.1f, \"mget_keys\": %llu, \"mget_hits\": "
            "%llu,\n"
            "    \"ops_by_type\": {",
            res->seconds, (unsigned long long)ops, (unsigned long long)res->gets,
            (unsigned long long)res->puts, (unsigned long long)res->misses, (unsigned long long)res->errors,
            res->seconds > 0 ? (double)ops / res->seconds : 0.0, (unsigned long long)res->mget_keys,
            (unsigned long long)res->mget_hits);
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        fprintf(f, "%s\"%s\": %llu", i ? ", " : "", pace_op_name((pace_op_type)i),
                (unsigned long long)res->ops_of[i]);
    }
    fputs("},\n", f);
    json_named_hist(f, "get_latency_ns", &res->lat[PACE_OP_GET], pc);
    fputs(",\n", f);
    json_named_hist(f, "put_latency_ns", &res->lat[PACE_OP_SET], pc);
    fputs(",\n", f);
    json_named_hist(f, "service_time_ns", &res->svc_lat, pc);
    fputs(",\n    \"latency_ns\": {", f);
    int first = 1;
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        if (res->lat[i].total == 0) {
            continue;
        }
        fprintf(f, "%s\"%s\": ", first ? "" : ", ", pace_op_name((pace_op_type)i));
        pace_json_hist(f, &res->lat[i], pc);
        first = 0;
    }
    fprintf(f, "},\n    \"max_lag_ns\": %llu,\n    \"series\": [", (unsigned long long)res->max_lag_ns);
    for (unsigned i = 0; i < res->nseries; i++) {
        fprintf(f, "%s%llu", i > 0 ? ", " : "", (unsigned long long)res->series[i]);
    }
    fputs("]", f);
    if (cfg->scenario == PACE_SCENARIO_KVCACHE) {
        const pace_kvcache_cfg *k = &cfg->kvcache;
        const pace_kvcache_stats *r = &res->kvcache;
        fprintf(f,
                ",\n    \"kvcache\": {\"block_size\": %u, \"groups\": %u, \"shared_blocks\": %u, "
                "\"turn_blocks\": %u, "
                "\"active_conv\": %u, \"max_turns\": %u, \"new_conv_prob\": %.3f, \"theta\": %.3f, "
                "\"requests\": %llu, \"block_gets\": %llu, \"block_hits\": %llu, \"hit_rate\": %.4f, "
                "\"reuse_rate\": %.4f, "
                "\"block_puts\": %llu, \"bytes_get\": %llu, \"bytes_put\": %llu, \"corrupt\": %llu,\n",
                k->block_size, k->groups, k->shared_blocks, k->turn_blocks, k->active_conv, k->max_turns,
                k->new_conv_prob, k->theta, (unsigned long long)r->requests,
                (unsigned long long)r->block_gets, (unsigned long long)r->block_hits,
                r->block_gets > 0 ? (double)r->block_hits / (double)r->block_gets : 0.0,
                r->block_hits + r->block_puts > 0
                    ? (double)r->block_hits / (double)(r->block_hits + r->block_puts)
                    : 0.0,
                (unsigned long long)r->block_puts, (unsigned long long)r->bytes_get,
                (unsigned long long)r->bytes_put, (unsigned long long)r->corrupt);
        json_named_hist(f, "request_latency_ns", &res->req_lat, pc);
        fputs("}", f);
    }
    if (res->has_server) {
        fprintf(f,
                ",\n    \"server\": {\"cpu_seconds\": %.3f, \"system_cpu_seconds\": %.3f, \"cpu_us_per_op\": "
                "%.3f, "
                "\"rss_bytes\": %llu}",
                res->server_cpu_seconds, res->server_system_seconds,
                ops > 0 ? res->server_cpu_seconds * 1e6 / (double)ops : 0.0,
                (unsigned long long)res->server_rss_bytes);
    }
    fputs(",\n    \"histograms\": {\"service\": ", f);
    pace_json_buckets(f, &res->svc_lat);
    fputs(", \"request\": ", f);
    pace_json_buckets(f, &res->req_lat);
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        fprintf(f, ", \"%s\": ", pace_op_name((pace_op_type)i));
        pace_json_buckets(f, &res->lat[i]);
    }
    fputs("}\n  }\n}\n", f);
}

void pace_report_text(FILE *f, const pace_config *cfg, const pace_result *res, const pace_percentiles *pc) {
    uint64_t ops = pace_result_ops(res);
    char vs[256];
    vsize_text(&cfg->wl, vs, sizeof(vs));
    if (cfg->scenario == PACE_SCENARIO_KVCACHE) {
        const pace_kvcache_cfg *k = &cfg->kvcache;
        fprintf(f,
                "driver=%s threads=%u scenario=kvcache block=%uB groups=%u shared-blocks=%u turn-blocks=%u "
                "active-conv=%u max-turns=%u\n",
                cfg->driver->name, cfg->threads, k->block_size, k->groups, k->shared_blocks, k->turn_blocks,
                k->active_conv, k->max_turns);
    } else {
        fprintf(f, "driver=%s threads=%u conns/thread=%u pipeline=%u value=%sB keys=%llu dist=%s\n",
                cfg->driver->name, cfg->threads, cfg->conns_per_thread ? cfg->conns_per_thread : 1,
                cfg->pipeline ? cfg->pipeline : 1, vs, (unsigned long long)cfg->wl.nkeys,
                dist_names[cfg->wl.dist]);
    }
    if (cfg->rate > 0.0) {
        fprintf(f, "open loop: offered %.0f ops/s (%s arrivals), max lag %.1f us\n", cfg->rate,
                cfg->arrival == PACE_ARRIVAL_POISSON ? "poisson" : "constant", (double)res->max_lag_ns / 1e3);
    }
    fprintf(f, "ops=%llu (gets=%llu puts=%llu misses=%llu errors=%llu) in %.2f s -> %.0f ops/s\n",
            (unsigned long long)ops, (unsigned long long)res->gets, (unsigned long long)res->puts,
            (unsigned long long)res->misses, (unsigned long long)res->errors, res->seconds,
            res->seconds > 0 ? (double)ops / res->seconds : 0.0);
    if (res->mget_keys > 0) {
        fprintf(f, "mget: %llu keys requested, %llu found\n", (unsigned long long)res->mget_keys,
                (unsigned long long)res->mget_hits);
    }
    if (cfg->scenario == PACE_SCENARIO_KVCACHE) {
        const pace_kvcache_stats *r = &res->kvcache;
        fprintf(
            f,
            "kvcache: %llu requests, reuse %.1f%% of needed blocks, probe hit rate %.1f%% (%llu of %llu), "
            "%llu blocks recomputed and written, read %.1f MB/s, write %.1f MB/s, corrupt %llu\n",
            (unsigned long long)r->requests,
            r->block_hits + r->block_puts > 0
                ? 100.0 * (double)r->block_hits / (double)(r->block_hits + r->block_puts)
                : 0.0,
            r->block_gets > 0 ? 100.0 * (double)r->block_hits / (double)r->block_gets : 0.0,
            (unsigned long long)r->block_hits, (unsigned long long)r->block_gets,
            (unsigned long long)r->block_puts,
            res->seconds > 0 ? (double)r->bytes_get / res->seconds / 1e6 : 0.0,
            res->seconds > 0 ? (double)r->bytes_put / res->seconds / 1e6 : 0.0,
            (unsigned long long)r->corrupt);
    }
    if (res->has_server) {
        fprintf(f, "server: %.3f CPU s during run (%.2f us/op), RSS %.1f MiB\n", res->server_cpu_seconds,
                ops > 0 ? res->server_cpu_seconds * 1e6 / (double)ops : 0.0,
                (double)res->server_rss_bytes / 1048576.0);
    }
    struct row {
        const char *name;
        const pace_hist *h;
    } rows[PACE_OP_COUNT + 2];
    unsigned nr = 0;
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        rows[nr++] = (struct row){pace_op_name((pace_op_type)i), &res->lat[i]};
    }
    rows[nr++] = (struct row){"service", &res->svc_lat};
    rows[nr++] = (struct row){"request", &res->req_lat};
    for (unsigned i = 0; i < nr; i++) {
        const pace_hist *h = rows[i].h;
        if (h->total == 0) {
            continue;
        }
        fprintf(f, "%s latency us: mean %.1f", rows[i].name, pace_hist_mean(h) / 1e3);
        for (unsigned k = 0; k < pc->n; k++) {
            char name[32];
            pname(pc->q[k], name, sizeof(name));
            fprintf(f, "  %s %.1f", name, (double)pace_hist_quantile(h, pc->q[k]) / 1e3);
        }
        fprintf(f, "  max %.1f\n", (double)h->max / 1e3);
    }
}

void pace_report_csv(FILE *f, const pace_config *cfg, const pace_result *res, const pace_percentiles *pc) {
    fprintf(f, "driver,operation,count,ops_per_sec,mean_us");
    for (unsigned k = 0; k < pc->n; k++) {
        char name[32];
        pname(pc->q[k], name, sizeof(name));
        fprintf(f, ",%s_us", name);
    }
    fprintf(f, ",max_us\n");
    for (unsigned i = 0; i <= PACE_OP_COUNT; i++) {
        const pace_hist *h = i < PACE_OP_COUNT ? &res->lat[i] : &res->svc_lat;
        if (h->total == 0) {
            continue;
        }
        const char *name = i < PACE_OP_COUNT ? pace_op_name((pace_op_type)i) : "service";
        fprintf(f, "%s,%s,%llu,%.1f,%.3f", cfg->driver->name, name, (unsigned long long)h->total,
                res->seconds > 0 ? (double)h->total / res->seconds : 0.0, pace_hist_mean(h) / 1e3);
        for (unsigned k = 0; k < pc->n; k++) {
            fprintf(f, ",%.3f", (double)pace_hist_quantile(h, pc->q[k]) / 1e3);
        }
        fprintf(f, ",%.3f\n", (double)h->max / 1e3);
    }
}

int pace_report_hgrm(const char *prefix, const pace_result *res) {
    for (unsigned i = 0; i < PACE_OP_COUNT; i++) {
        const pace_hist *h = &res->lat[i];
        if (h->total == 0) {
            continue;
        }
        char path[4096];
        snprintf(path, sizeof(path), "%s_%s.hgrm", prefix, pace_op_name((pace_op_type)i));
        FILE *f = fopen(path, "w");
        if (f == NULL) {
            perror(path);
            return -1;
        }
        fprintf(f, "%12s %14s %10s %14s\n\n", "Value", "Percentile", "TotalCount", "1/(1-Percentile)");
        uint64_t cum = 0;
        double sq = 0.0;
        double mean = pace_hist_mean(h);
        unsigned buckets = 0;
        for (unsigned b = 0; b < PACE_HIST_BUCKETS; b++) {
            if (h->counts[b] == 0) {
                continue;
            }
            buckets++;
            cum += h->counts[b];
            uint64_t hi = pace_hist_bucket_high(b);
            uint64_t lo = pace_hist_bucket_low(b);
            double mid = ((double)lo + (double)hi) / 2.0;
            sq += (double)h->counts[b] * (mid - mean) * (mid - mean);
            double q = (double)cum / (double)h->total;
            uint64_t v = hi < h->max ? hi : h->max;
            if (q < 1.0) {
                fprintf(f, "%12.3f %14.12f %10llu %14.2f\n", (double)v / 1e6, q, (unsigned long long)cum,
                        1.0 / (1.0 - q));
            } else {
                fprintf(f, "%12.3f %14.12f %10llu %14s\n", (double)v / 1e6, q, (unsigned long long)cum,
                        "inf");
            }
        }
        fprintf(f, "#[Mean    = %12.3f, StdDeviation   = %12.3f]\n", mean / 1e6,
                sqrt(sq / (double)h->total) / 1e6);
        fprintf(f, "#[Max     = %12.3f, Total count    = %12llu]\n", (double)h->max / 1e6,
                (unsigned long long)h->total);
        fprintf(f, "#[Buckets = %12u, SubBuckets     = %12u]\n", buckets, PACE_HIST_SUB);
        fclose(f);
    }
    return 0;
}
