/*
 * fuzz_acl.c - libFuzzer harness: ACL files (acl_parse) in both formats, then queries on every user.
 */
#include <stdlib.h>
#include <string.h>

#include "acl.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    char *text = malloc(n + 1);
    if (text == NULL) {
        return 0;
    }
    memcpy(text, data, n);
    text[n] = '\0';
    for (int compat = 0; compat < 2; compat++) {
        char err[256];
        acl *a = acl_parse(text, compat, err, sizeof(err));
        if (a == NULL) {
            continue;
        }
        for (size_t i = 0; i < acl_nusers(a); i++) {
            const acl_user *u = acl_user_at(a, i);
            char desc[4096];
            acl_describe(u, desc, sizeof(desc));
            acl_key_allowed(u, text, n < 64 ? n : 64, ACL_KEY_READ | ACL_KEY_WRITE);
            acl_channel_allowed(u, text, n < 64 ? n : 64);
            for (unsigned c = 0; c < acl_ncmds(); c++) {
                acl_cmd_allowed(u, c);
            }
            acl_login(a, acl_user_name(u), strlen(acl_user_name(u)), text, n < 32 ? n : 32);
        }
        acl_default_login(a);
        acl_free(a);
    }
    free(text);
    return 0;
}
