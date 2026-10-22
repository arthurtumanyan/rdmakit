/*
 * fuzz_text.c - libFuzzer harness: the memcached and RESP protocol modules of a server without listeners.
 * The first input byte selects the protocol and whether the session is authenticated; the rest is fed
 * in two parts (split at the second byte's value) to exercise incremental parsing. The modules must
 * never consume more than they were given.
 */
#include <stdlib.h>
#include <string.h>

#include "srv.h"

static akvd *server(void) {
    static akvd *d;
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
    }
    return d;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    akvd *d = server();
    if (d == NULL || n < 2) {
        return 0;
    }
    int resp = data[0] & 1;
    struct session ss = {.authed = (data[0] & 2) == 0};
    size_t split = data[1];
    data += 2;
    n -= 2;
    if (split > n) {
        split = n;
    }
    struct obuf in = {0};
    struct obuf out = {0};
    size_t parts[2] = {split, n - split};
    const uint8_t *p = data;
    for (int i = 0; i < 2 && !ss.close; i++) {
        if (parts[i] == 0) {
            continue;
        }
        ob_add(&in, p, parts[i]);
        p += parts[i];
        size_t avail = in.len - in.off;
        size_t used = resp ? resp_execute(d, &ss, in.p + in.off, avail, &out)
                           : mc_execute(d, &ss, in.p + in.off, avail, &out);
        if (used > avail) {
            abort();
        }
        in.off += used;
        ob_compact(&in);
        out.len = out.off = 0;
    }
    ob_free(&in);
    ob_free(&out);
    resp_session_free(&ss); /* transaction queue and watched keys */
    akv_flush(akvd_store(d));
    return 0;
}
