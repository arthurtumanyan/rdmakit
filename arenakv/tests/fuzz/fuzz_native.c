/*
 * fuzz_native.c - libFuzzer harness: native protocol requests (akvd_native_execute) on a server without
 * listeners. The first input byte selects whether the session is authenticated and the response
 * capacity; the rest is one request message.
 */
#include <stdlib.h>

#include "srv.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static akvd *d;
    static uint8_t *out;
    if (d == NULL) {
        akvd_config cfg;
        akvd_config_default(&cfg);
        cfg.port = NULL;
        cfg.store.capacity = 4u << 20;
        cfg.store.shards = 4;
        cfg.store.max_entry = 64u << 10;
        cfg.threads = 1;
        cfg.log_level = AKVD_LOG_ERROR;
        d = akvd_start(&cfg, NULL);
        out = malloc(1u << 17);
    }
    if (d == NULL || out == NULL || n < 1) {
        return 0;
    }
    struct session ss = {.authed = (data[0] & 1) == 0};
    size_t cap = (data[0] & 2) ? 64 : (1u << 17);
    akvd_native_execute(d, &ss, data + 1, n - 1, out, cap);
    return 0;
}
