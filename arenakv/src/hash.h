/*
 * hash.h - 64-bit non-cryptographic hash for keys.
 *
 * Processes 8 bytes per step with a 64x64 -> 128 bit multiply whose halves are folded together. The seed
 * selects an independent function. Not suitable against adversarial inputs (keys chosen to collide);
 * the seed of a server instance is random, which makes precomputed collisions ineffective.
 */
#ifndef AKV_HASH_H
#define AKV_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint64_t akv_mulfold(uint64_t a, uint64_t b) {
    unsigned __int128 p = (unsigned __int128)a * b;
    return (uint64_t)p ^ (uint64_t)(p >> 64);
}

/* Hashes len bytes at data. */
static inline uint64_t akv_hash64(const void *data, size_t len, uint64_t seed) {
    static const uint64_t k0 = 0xa0761d6478bd642full;
    static const uint64_t k1 = 0xe7037ed1a0b428dbull;
    static const uint64_t k2 = 0x8ebc6af09c88c6e3ull;
    const uint8_t *p = data;
    uint64_t h = seed ^ k0 ^ ((uint64_t)len * k2);
    while (len >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = akv_mulfold(h ^ w, k1);
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t w = 0;
        memcpy(&w, p, len); /* remaining 1..7 bytes, zero padded; the length is already mixed into the seed */
        h = akv_mulfold(h ^ w, k2);
    }
    return akv_mulfold(h ^ k2, k0 ^ h);
}

#endif
