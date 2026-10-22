/*
 * fuzz_hello.c - libFuzzer harness: handshake messages (duct_hello_decode) and region descriptors
 * (duct_desc_decode). A decoded handshake must satisfy the limits the decoder promises.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    struct duct_hello h;
    if (duct_hello_decode(data, n, &h) == 0) {
        if (h.max_msg == 0 || h.max_msg > DUCT_LIMIT_MAX_MSG || h.recv_depth == 0 ||
            h.recv_depth > DUCT_LIMIT_DEPTH) {
            abort();
        }
    }
    if (n >= sizeof(struct duct_mr_desc)) {
        struct duct_mr_desc in;
        memcpy(&in, data, sizeof(in));
        struct duct_desc d;
        duct_desc_decode(&in, &d);
    }
    return 0;
}
