/*
 * fuzz_snapshot.c - libFuzzer harness: snapshot files (snap_load) written from the input into a temporary
 * file and loaded into a small engine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "srv.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static akv *s;
    static char path[64];
    if (s == NULL) {
        akv_config cfg;
        akv_config_default(&cfg);
        cfg.capacity = 1 << 20;
        cfg.shards = 4;
        s = akv_create(&cfg);
        snprintf(path, sizeof(path), "/tmp/fuzz-snapshot-%d.snap", (int)getpid());
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    fwrite(data, 1, n, f);
    fclose(f);
    uint64_t records;
    snap_load(s, path, &records);
    akv_flush(s);
    unlink(path);
    return 0;
}
