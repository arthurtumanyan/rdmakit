/* hello.c - connection handshake encoding, see internal.h for the layout. */
#include <string.h>

#include "internal.h"

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p);

void duct_hello_encode(const struct duct_hello *h, uint8_t *out) {
    put_le32(out, DUCT_HELLO_MAGIC);
    put_le16(out + 4, DUCT_HELLO_VERSION);
    put_le16(out + 6, 0);
    put_le32(out + 8, h->max_msg);
    put_le32(out + 12, h->recv_depth);
    put_le64(out + 16, h->credit_addr);
    put_le32(out + 24, h->credit_rkey);
    put_le32(out + 28, 0);
}

static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

int duct_hello_decode(const uint8_t *in, size_t len, struct duct_hello *h) {
    if (len < DUCT_HELLO_SIZE) {
        return DUCT_EPROTO;
    }
    if (get_le32(in) != DUCT_HELLO_MAGIC || get_le16(in + 4) != DUCT_HELLO_VERSION) {
        return DUCT_EPROTO;
    }
    h->max_msg = get_le32(in + 8);
    h->recv_depth = get_le32(in + 12);
    h->credit_addr = get_le64(in + 16);
    h->credit_rkey = get_le32(in + 24);
    if (h->max_msg == 0 || h->max_msg > DUCT_LIMIT_MAX_MSG || h->recv_depth == 0 ||
        h->recv_depth > DUCT_LIMIT_DEPTH) {
        return DUCT_EPROTO;
    }
    return 0;
}
