/*
 * proc.h - process resource sampling from /proc.
 *
 * Used to attribute server CPU time and memory to a benchmark run. The sampled
 * process may belong to another container; only its host PID is needed.
 */
#ifndef PACE_PROC_H
#define PACE_PROC_H

#include <stdint.h>

typedef struct pace_proc_sample {
    double cpu_seconds; /* user + system CPU time consumed since process start */
    uint64_t rss_bytes; /* resident set size */
} pace_proc_sample;

/* Samples process pid. Returns 0, or -1 when /proc/<pid> cannot be read or parsed. */
int pace_proc_sample_pid(int pid, pace_proc_sample *out);

#endif
