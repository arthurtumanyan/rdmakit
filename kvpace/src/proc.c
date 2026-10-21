/* proc.c - process resource sampling, see proc.h. */
#include "proc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int pace_proc_sample_pid(int pid, pace_proc_sample *out) {
    char path[64];
    char buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* Fields follow the parenthesized command name, which may contain spaces. */
    char *p = strrchr(buf, ')');
    if (p == NULL) {
        return -1;
    }
    unsigned long utime = 0;
    unsigned long stime = 0;
    /* After ')': state(3) ppid(4) ... utime(14) stime(15); 11 fields lie between state and utime. */
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) != 2) {
        return -1;
    }
    long hz = sysconf(_SC_CLK_TCK);
    out->cpu_seconds = (double)(utime + stime) / (double)hz;
    out->user_seconds = (double)utime / (double)hz;
    out->system_seconds = (double)stime / (double)hz;

    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    unsigned long size = 0;
    unsigned long resident = 0;
    int ok = fscanf(f, "%lu %lu", &size, &resident) == 2;
    fclose(f);
    if (!ok) {
        return -1;
    }
    out->rss_bytes = (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE);
    return 0;
}
