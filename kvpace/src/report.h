/*
 * report.h - result writers: JSON, text, CSV and HdrHistogram percentile distributions.
 *
 * JSON schema (version 2): one object with the members "schema", "tool", "environment", "config",
 * "results". Latencies are in nanoseconds. Version 2 keeps every member of version 1 and adds per
 * operation counts ("ops_by_type"), per operation latency summaries ("latency_ns"), the configured
 * percentiles, MGET key counts, and the non-empty histogram buckets ("histograms": per histogram a list
 * of [bucket index, count] pairs of the log-linear histogram in hist.h), which kvpace merge combines.
 */
#ifndef PACE_REPORT_H
#define PACE_REPORT_H

#include <stdio.h>

#include "runner.h"

#define PACE_MAX_PERCENTILES 16

/* Percentiles reported in addition to p50, p90, p99 and p99.9 (fractions in (0, 1)). */
typedef struct pace_percentiles {
    double q[PACE_MAX_PERCENTILES];
    unsigned n;
} pace_percentiles;

/* Parses "50,99,99.9" into fractions. Returns 0 or -1. */
int pace_parse_percentiles(const char *s, pace_percentiles *p);

/* Default list: 50, 90, 99, 99.9, 99.99. */
void pace_default_percentiles(pace_percentiles *p);

/*
 * Writes the result of one run to f. label identifies the system under test (free text, for example
 * "valkey-9.0.1 docker"). argv is recorded verbatim.
 */
void pace_report_json(FILE *f, const pace_config *cfg, const pace_result *res, const pace_percentiles *pc,
                      const char *label, int argc, char **argv);

/* Writes a short human-readable summary to f. */
void pace_report_text(FILE *f, const pace_config *cfg, const pace_result *res, const pace_percentiles *pc);

/* Writes a CSV header and one row per recorded operation type plus a "service" row (service time) to f.
 * Columns: driver, operation, count, ops_per_sec, mean_us, one column per percentile, max_us. */
void pace_report_csv(FILE *f, const pace_config *cfg, const pace_result *res, const pace_percentiles *pc);

/*
 * Writes "<prefix>_<op>.hgrm" for every operation type with samples: the HdrHistogram percentile
 * distribution format (value in milliseconds, percentile, total count, 1/(1-percentile)), readable by the
 * HdrHistogram plotting tools. Values are bucket upper bounds of the log-linear histogram. Returns 0 or -1.
 */
int pace_report_hgrm(const char *prefix, const pace_result *res);

/* Writes the latency summary of a histogram as a JSON object (used by kvpace merge as well). */
void pace_json_hist(FILE *f, const pace_hist *h, const pace_percentiles *pc);

/* Writes the non-empty buckets of a histogram as a JSON list of [index, count] pairs. */
void pace_json_buckets(FILE *f, const pace_hist *h);

/* Writes s as a JSON string literal. */
void pace_json_str(FILE *f, const char *s);

#endif
