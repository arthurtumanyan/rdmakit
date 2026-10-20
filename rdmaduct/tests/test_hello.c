/* test_hello.c - handshake encoding and validation tests. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

static int failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

int main(void) {
    struct duct_hello in = {.max_msg = 0x01020304u,
                            .recv_depth = 0x0506u,
                            .credit_addr = 0x1112131415161718ull,
                            .credit_rkey = 0x21222324u};
    uint8_t wire[DUCT_HELLO_SIZE];
    duct_hello_encode(&in, wire);

    static const uint8_t expect[DUCT_HELLO_SIZE] = {
        0x52, 0x54, 0x50, 0x31, 0x03, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01, 0x06, 0x05, 0x00, 0x00,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x24, 0x23, 0x22, 0x21, 0x00, 0x00, 0x00, 0x00};
    EXPECT(memcmp(wire, expect, sizeof(wire)) == 0, "wire layout is little-endian and fixed");

    struct duct_hello out;
    EXPECT(duct_hello_decode(wire, sizeof(wire), &out) == 0, "decode");
    EXPECT(out.max_msg == in.max_msg && out.recv_depth == in.recv_depth &&
               out.credit_addr == in.credit_addr && out.credit_rkey == in.credit_rkey,
           "round trip");

    EXPECT(duct_hello_decode(wire, sizeof(wire) - 1, &out) == DUCT_EPROTO, "short input");

    uint8_t bad[DUCT_HELLO_SIZE];
    memcpy(bad, wire, sizeof(bad));
    bad[0] ^= 1;
    EXPECT(duct_hello_decode(bad, sizeof(bad), &out) == DUCT_EPROTO, "bad magic");

    memcpy(bad, wire, sizeof(bad));
    bad[4] = 2;
    EXPECT(duct_hello_decode(bad, sizeof(bad), &out) == DUCT_EPROTO, "previous version rejected");
    bad[4] = 4;
    EXPECT(duct_hello_decode(bad, sizeof(bad), &out) == DUCT_EPROTO, "unsupported version");

    /* Region descriptor: fixed little-endian layout and round trip. */
    struct duct_desc dd = {
        .access = 5, .addr = 0x0102030405060708ull, .len = 0x1112131415161718ull, .key = 0x21222324u};
    struct duct_mr_desc dw;
    duct_desc_encode(&dd, &dw);
    static const uint8_t dexpect[DUCT_MR_DESC_SIZE] = {
        0x44, 0x4d, 0x52, 0x31, 0x05, 0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x24, 0x23, 0x22, 0x21, 0x00, 0x00, 0x00, 0x00};
    EXPECT(memcmp(dw.bytes, dexpect, sizeof(dexpect)) == 0, "descriptor layout is little-endian and fixed");
    struct duct_desc dr;
    EXPECT(duct_desc_decode(&dw, &dr) == 0 && dr.access == dd.access && dr.addr == dd.addr &&
               dr.len == dd.len && dr.key == dd.key,
           "descriptor round trip");
    dw.bytes[0] ^= 1;
    EXPECT(duct_desc_decode(&dw, &dr) == -EINVAL, "descriptor with bad magic rejected");

    struct duct_hello zero = {.max_msg = 0, .recv_depth = 1};
    duct_hello_encode(&zero, bad);
    EXPECT(duct_hello_decode(bad, sizeof(bad), &out) == DUCT_EPROTO, "zero max_msg");

    struct duct_hello huge = {.max_msg = DUCT_LIMIT_MAX_MSG + 1, .recv_depth = 1};
    duct_hello_encode(&huge, bad);
    EXPECT(duct_hello_decode(bad, sizeof(bad), &out) == DUCT_EPROTO, "max_msg above limit");

    struct duct_hello nodepth = {.max_msg = 1, .recv_depth = 0};
    duct_hello_encode(&nodepth, bad);
    EXPECT(duct_hello_decode(bad, sizeof(bad), &out) == DUCT_EPROTO, "zero depth");

    /* Random and bit-flipped inputs: decoding never misbehaves, accepted values respect the limits. */
    uint64_t state = 0x9e3779b97f4a7c15ull;
    int accepted = 0;
    for (int iter = 0; iter < 2000000; iter++) {
        uint8_t buf[DUCT_HELLO_SIZE];
        if (iter % 2 == 0) {
            memcpy(buf, wire, sizeof(buf));
            for (int flips = 1 + iter % 3; flips > 0; flips--) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                buf[(state >> 33) % sizeof(buf)] ^= (uint8_t)(1u << ((state >> 20) & 7));
            }
        } else {
            for (size_t i = 0; i < sizeof(buf); i++) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                buf[i] = (uint8_t)(state >> 40);
            }
        }
        struct duct_hello h;
        size_t len = (iter % 7 == 0) ? (size_t)(state >> 50) % (sizeof(buf) + 1) : sizeof(buf);
        int rc = duct_hello_decode(buf, len, &h);
        if (rc == 0) {
            accepted++;
            EXPECT(len >= DUCT_HELLO_SIZE && h.max_msg >= 1 && h.max_msg <= DUCT_LIMIT_MAX_MSG &&
                       h.recv_depth >= 1 && h.recv_depth <= DUCT_LIMIT_DEPTH,
                   "accepted hello violates limits");
        } else {
            EXPECT(rc == DUCT_EPROTO, "unexpected decode result %d", rc);
        }
    }
    printf("hello fuzz: %d of 2000000 inputs accepted\n", accepted);

    if (failures != 0) {
        return 1;
    }
    puts("test_hello: ok");
    return 0;
}
