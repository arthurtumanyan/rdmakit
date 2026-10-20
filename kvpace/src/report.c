/* report.c - JSON and text result writers, see report.h. */
#include "report.h"

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef PACE_GIT
#define PACE_GIT "unknown"
#endif

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

/* Writes s as a JSON string literal. */
static void json_str(FILE *f, const char *s) {
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

static void json_hist(FILE *f, const char *name, const pace_hist *h) {
    fprintf(f,
            "    \"%s\": {\"count\": %llu, \"min\": %llu, \"mean\": %.1f, \"p50\": %llu, \"p90\": %llu, "
            "\"p99\": %llu, \"p999\": %llu, \"max\": %llu}",
            name, (unsigned long long)h->total, (unsigned long long)(h->total ? h->min : 0),
            pace_hist_mean(h), (unsigned long long)pace_hist_quantile(h, 0.50),
            (unsigned long long)pace_hist_quantile(h, 0.90), (unsigned long long)pace_hist_quantile(h, 0.99),
            (unsigned long long)pace_hist_quantile(h, 0.999), (unsigned long long)(h->total ? h->max : 0));
}

void pace_report_json(FILE *f, const pace_config *cfg, const pace_result *res, const char *label, int argc,
                      char **argv) {
    struct utsname un;
    uname(&un);
    char cpu[160];
    cpu_model(cpu, sizeof(cpu));
    char when[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(f, "{\n  \"schema\": 1,\n  \"tool\": {\"name\": \"kvpace\", \"commit\": ");
    json_str(f, PACE_GIT);
    fprintf(f, ", \"timestamp\": \"%s\"},\n  \"environment\": {\"cpu\": ", when);
    json_str(f, cpu);
    fprintf(f, ", \"logical_cpus\": %ld, \"kernel\": ", sysconf(_SC_NPROCESSORS_ONLN));
    json_str(f, un.release);
    fprintf(f, ", \"machine\": ");
    json_str(f, un.machine);
    fprintf(f, ", \"compiler\": ");
    json_str(f, __VERSION__);
    fprintf(f, "},\n  \"config\": {\"label\": ");
    json_str(f, label);
    fprintf(f, ", \"driver\": ");
    json_str(f, cfg->driver->name);
    fprintf(f, ", \"host\": ");
    json_str(f, cfg->host);
    fprintf(f, ", \"port\": ");
    json_str(f, cfg->port);
    fprintf(f,
            ", \"threads\": %u, \"warmup_s\": %.1f, \"duration_s\": %.1f, \"keys\": %llu, \"dist\": \"%s\", "
            "\"theta\": %.3f, \"value_size\": %u, \"read_ratio\": %.3f, \"seed\": %llu, \"preload\": %d, "
            "\"rate\": %.1f, \"arrival\": \"%s\", \"argv\": [",
            cfg->threads, cfg->warmup_s, cfg->duration_s, (unsigned long long)cfg->nkeys,
            cfg->dist == PACE_DIST_ZIPF ? "zipf" : "uniform", cfg->theta, cfg->value_size, cfg->read_ratio,
            (unsigned long long)cfg->seed, cfg->preload, cfg->rate,
            cfg->arrival == PACE_ARRIVAL_POISSON ? "poisson" : "constant");
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            fputs(", ", f);
        }
        json_str(f, argv[i]);
    }
    uint64_t ops = res->gets + res->puts;
    fprintf(f,
            "]},\n  \"results\": {\n    \"seconds\": %.3f, \"ops\": %llu, \"gets\": %llu, \"puts\": %llu, "
            "\"misses\": %llu, \"errors\": %llu, \"ops_per_sec\": %.1f,\n",
            res->seconds, (unsigned long long)ops, (unsigned long long)res->gets,
            (unsigned long long)res->puts, (unsigned long long)res->misses, (unsigned long long)res->errors,
            res->seconds > 0 ? (double)ops / res->seconds : 0.0);
    json_hist(f, "get_latency_ns", &res->get_lat);
    fputs(",\n", f);
    json_hist(f, "put_latency_ns", &res->put_lat);
    fputs(",\n", f);
    json_hist(f, "service_time_ns", &res->svc_lat);
    fprintf(f, ",\n    \"max_lag_ns\": %llu,\n    \"series\": [", (unsigned long long)res->max_lag_ns);
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
        json_hist(f, "request_latency_ns", &res->req_lat);
        fputs("}", f);
    }
    if (res->has_server) {
        fprintf(f, ",\n    \"server\": {\"cpu_seconds\": %.3f, \"cpu_us_per_op\": %.3f, \"rss_bytes\": %llu}",
                res->server_cpu_seconds, ops > 0 ? res->server_cpu_seconds * 1e6 / (double)ops : 0.0,
                (unsigned long long)res->server_rss_bytes);
    }
    fputs("\n  }\n}\n", f);
}

void pace_report_text(FILE *f, const pace_config *cfg, const pace_result *res) {
    uint64_t ops = res->gets + res->puts;
    fprintf(f, "driver=%s threads=%u value=%uB read_ratio=%.2f keys=%llu dist=%s\n", cfg->driver->name,
            cfg->threads, cfg->value_size, cfg->read_ratio, (unsigned long long)cfg->nkeys,
            cfg->dist == PACE_DIST_ZIPF ? "zipf" : "uniform");
    if (cfg->rate > 0.0) {
        fprintf(f, "open loop: offered %.0f ops/s (%s arrivals), max lag %.1f us\n", cfg->rate,
                cfg->arrival == PACE_ARRIVAL_POISSON ? "poisson" : "constant", (double)res->max_lag_ns / 1e3);
    }
    fprintf(f, "ops=%llu (gets=%llu puts=%llu misses=%llu errors=%llu) in %.2f s -> %.0f ops/s\n",
            (unsigned long long)ops, (unsigned long long)res->gets, (unsigned long long)res->puts,
            (unsigned long long)res->misses, (unsigned long long)res->errors, res->seconds,
            res->seconds > 0 ? (double)ops / res->seconds : 0.0);
    const struct {
        const char *name;
        const pace_hist *h;
    } rows[] = {{"get", &res->get_lat},
                {"put", &res->put_lat},
                {"service", &res->svc_lat},
                {"request", &res->req_lat}};
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
    for (int i = 0; i < 4; i++) {
        const pace_hist *h = rows[i].h;
        if (h->total == 0) {
            continue;
        }
        fprintf(f, "%s latency us: mean %.1f  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n",
                rows[i].name, pace_hist_mean(h) / 1e3, (double)pace_hist_quantile(h, 0.50) / 1e3,
                (double)pace_hist_quantile(h, 0.90) / 1e3, (double)pace_hist_quantile(h, 0.99) / 1e3,
                (double)pace_hist_quantile(h, 0.999) / 1e3, (double)h->max / 1e3);
    }
}
