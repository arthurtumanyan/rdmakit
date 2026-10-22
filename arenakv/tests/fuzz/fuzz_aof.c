/*
 * fuzz_aof.c - libFuzzer harness: append-only logs. The input becomes the content of the last increment
 * file of a log directory (with a valid manifest and an empty base), which aof_open() loads into a small
 * engine; torn tails are cut off, damage is refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aof.h"

static void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static akv *s;
    static char dir[64];
    if (s == NULL) {
        akv_config cfg;
        akv_config_default(&cfg);
        cfg.capacity = 1 << 20;
        cfg.shards = 4;
        s = akv_create(&cfg);
        snprintf(dir, sizeof(dir), "/tmp/fuzz-aof-%d", (int)getpid());
        mkdir(dir, 0700);
    }
    char path[128];
    static const uint8_t base[24] = {'A', 'K', 'V', 'L', 'O', 'G', '0', '1', 1, 0, 0, 0, 0, 0, 0, 0};
    snprintf(path, sizeof(path), "%s/base-1.aof", dir);
    write_file(path, base, sizeof(base));
    snprintf(path, sizeof(path), "%s/incr-2.aof", dir);
    write_file(path, data, n);
    snprintf(path, sizeof(path), "%s/manifest", dir);
    static const char manifest[] = "akv-aof 1\nbase base-1.aof\nincr incr-2.aof\n";
    write_file(path, manifest, sizeof(manifest) - 1);
    aof_config c = {.dir = dir, .fsync = AOF_FSYNC_NO};
    aof *a = NULL;
    aof_load_info li;
    if (aof_open(&c, s, 1, &a, &li) == 0) {
        aof_close(a);
    }
    akv_flush(s);
    return 0;
}
