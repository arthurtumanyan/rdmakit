/*
 * report.h - JSON result writer.
 *
 * Output schema (version 1): one JSON object with the members "schema",
 * "tool", "environment", "config", "results". Latencies are in nanoseconds.
 */
#ifndef PACE_REPORT_H
#define PACE_REPORT_H

#include <stdio.h>

#include "runner.h"

/*
 * Writes the result of one run to f. label identifies the system under test
 * (free text, for example "valkey-9.0.1 docker"). argv is recorded verbatim.
 */
void pace_report_json(FILE *f, const pace_config *cfg, const pace_result *res, const char *label, int argc,
                      char **argv);

/* Writes a short human-readable summary to f. */
void pace_report_text(FILE *f, const pace_config *cfg, const pace_result *res);

#endif
