/*
 * simd.h - tag matching for the hash index.
 *
 * akv_match16() compares 16 16-bit tags with one value and returns a bit mask (bit i set when
 * tags[i] == tag). Implementations: SSE2 (x86-64, always available there), NEON (aarch64), and a portable
 * scalar version. The scalar version is always compiled so that tests can check the vector versions
 * against it.
 */
#ifndef AKV_SIMD_H
#define AKV_SIMD_H

#include <stdint.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#define AKV_SIMD_NAME "sse2"
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#define AKV_SIMD_NAME "neon"
#else
#define AKV_SIMD_NAME "scalar"
#endif

/* Portable reference implementation. */
static inline uint32_t akv_match16_scalar(const uint16_t *tags, uint16_t tag) {
    uint32_t mask = 0;
    for (unsigned i = 0; i < 16; i++) {
        mask |= (uint32_t)(tags[i] == tag) << i;
    }
    return mask;
}

/* Returns a mask of the positions i in [0, 16) with tags[i] == tag. tags must be 16-byte aligned. */
static inline uint32_t akv_match16(const uint16_t *tags, uint16_t tag) {
#if defined(__SSE2__)
    __m128i needle = _mm_set1_epi16((short)tag);
    __m128i lo = _mm_cmpeq_epi16(_mm_load_si128((const __m128i *)tags), needle);
    __m128i hi = _mm_cmpeq_epi16(_mm_load_si128((const __m128i *)(tags + 8)), needle);
    /* Saturating pack turns each 0xFFFF / 0x0000 lane into 0xFF / 0x00; movemask collects the top bits. */
    return (uint32_t)_mm_movemask_epi8(_mm_packs_epi16(lo, hi));
#elif defined(__ARM_NEON)
    uint16x8_t needle = vdupq_n_u16(tag);
    uint16x8_t lo = vceqq_u16(vld1q_u16(tags), needle);
    uint16x8_t hi = vceqq_u16(vld1q_u16(tags + 8), needle);
    /* Narrow to 16 bytes of 0xFF / 0x00, then keep one bit per lane (NEON has no movemask). */
    uint8x16_t bytes = vcombine_u8(vmovn_u16(lo), vmovn_u16(hi));
    static const uint8_t weights[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t bits = vandq_u8(bytes, vld1q_u8(weights));
    return (uint32_t)vaddv_u8(vget_low_u8(bits)) | ((uint32_t)vaddv_u8(vget_high_u8(bits)) << 8);
#else
    return akv_match16_scalar(tags, tag);
#endif
}

#endif
