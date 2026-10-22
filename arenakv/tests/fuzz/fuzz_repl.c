/*
 * fuzz_repl.c - libFuzzer harness: the replication stream as a replica receives it. The input is fed to
 * the record parser (repl_parse) in two parts (split at the first byte's value) and every decoded record is
 * applied to a small engine with akv_apply(), as the replica's link thread does.
 */
#include "repl.h"
#include "srv.h"

static int apply(void *arg, const akv_change *c) {
    akv_apply(arg, c);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    static akv *s;
    if (s == NULL) {
        akv_config cfg;
        akv_config_default(&cfg);
        cfg.capacity = 1 << 20;
        cfg.shards = 4;
        s = akv_create(&cfg);
        akv_set_follower(s, 1);
    }
    if (n < 1) {
        return 0;
    }
    size_t split = data[0] < n - 1 ? data[0] : n - 1;
    repl_parser p = {0};
    if (repl_parse(&p, data + 1, split, apply, s) == 0) {
        repl_parse(&p, data + 1 + split, n - 1 - split, apply, s);
    }
    repl_parser_free(&p);
    akv_flush(s);
    return 0;
}
