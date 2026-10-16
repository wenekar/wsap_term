#ifndef WSAP_TERM_KERNELS_H
#define WSAP_TERM_KERNELS_H

#include "cell.h"
#include "compat.h"
#include "simd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline size_t scan_printable_span_avx512(const char *buf, size_t len, unsigned char hi)
{
    size_t i = 0;
    __m512i v20_512 = _mm512_set1_epi8(0x20);
    __m512i vlim_512 = _mm512_set1_epi8((char)(hi - 0x20));
    for (; i + 64 <= len; i += 64) {
        __m512i v = _mm512_loadu_si512((const void *)(buf + i));
        __mmask64 mask = _mm512_cmpge_epu8_mask(_mm512_sub_epi8(v, v20_512), vlim_512);
        if (mask != 0)
            return i + wsap_vtcore_ctz64(mask);
    }
    if (i < len) {
        __mmask64 load_mask = ((uint64_t)1 << (len - i)) - 1;
        __m512i v = _mm512_maskz_loadu_epi8(load_mask, buf + i);
        __mmask64 mask = _mm512_mask_cmpge_epu8_mask(load_mask, _mm512_sub_epi8(v, v20_512), vlim_512);
        if (mask != 0)
            return i + wsap_vtcore_ctz64(mask);
    }
    return len;
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline size_t scan_printable_span_avx2(const char *buf, size_t len, unsigned char hi)
{
    size_t i = 0;
    const __m256i v_offset = _mm256_set1_epi8((char)(0x20 + 0x80));
    const __m256i v_limit = _mm256_set1_epi8((char)(hi - 0xa1));
    for (; i + 64 <= len; i += 64) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(buf + i));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(buf + i + 32));
        __m256i sub0 = _mm256_sub_epi8(v0, v_offset);
        __m256i sub1 = _mm256_sub_epi8(v1, v_offset);
        int m0 = _mm256_movemask_epi8(_mm256_cmpgt_epi8(sub0, v_limit));
        int m1 = _mm256_movemask_epi8(_mm256_cmpgt_epi8(sub1, v_limit));
        if (WSAP_VTCORE_EXPECT((m0 | m1) != 0, 0)) {
            if (m0 != 0)
                return i + wsap_vtcore_ctz32(m0);
            return i + 32 + wsap_vtcore_ctz32(m1);
        }
    }
    if (i + 32 <= len) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(buf + i));
        __m256i sub = _mm256_sub_epi8(v, v_offset);
        int mask = _mm256_movemask_epi8(_mm256_cmpgt_epi8(sub, v_limit));
        if (mask != 0)
            return i + wsap_vtcore_ctz32(mask);
        i += 32;
    }
    if (i + 16 <= len) {
        __m128i v = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i sub = _mm_sub_epi8(v, _mm_set1_epi8((char)(0x20 + 0x80)));
        int mask = _mm_movemask_epi8(_mm_cmpgt_epi8(sub, _mm_set1_epi8((char)(hi - 0xa1)))) & 0xFFFF;
        if (mask != 0)
            return i + wsap_vtcore_ctz32(mask);
        i += 16;
    }
    for (; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 || c >= hi)
            return i;
    }
    return len;
}
#endif

static inline size_t scan_printable_span(const char *buf, size_t len,
                                         unsigned char hi)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (len >= 64 && wsap_vtcore_use_avx512())
        return scan_printable_span_avx512(buf, len, hi);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (len >= 32 && wsap_vtcore_use_avx2())
        return scan_printable_span_avx2(buf, len, hi);
#endif
    size_t i = 0;
#if defined(__aarch64__)
    uint8x16_t v20 = vdupq_n_u8(0x20);
    uint8x16_t vlim = vdupq_n_u8((uint8_t)(hi - 0x20));
    for (; i + 64 <= len; i += 64) {
        uint8x16_t v0 = vld1q_u8((const uint8_t *)buf + i);
        uint8x16_t v1 = vld1q_u8((const uint8_t *)buf + i + 16);
        uint8x16_t v2 = vld1q_u8((const uint8_t *)buf + i + 32);
        uint8x16_t v3 = vld1q_u8((const uint8_t *)buf + i + 48);

        uint8x16_t m0 = vcgeq_u8(vsubq_u8(v0, v20), vlim);
        uint8x16_t m1 = vcgeq_u8(vsubq_u8(v1, v20), vlim);
        uint8x16_t m2 = vcgeq_u8(vsubq_u8(v2, v20), vlim);
        uint8x16_t m3 = vcgeq_u8(vsubq_u8(v3, v20), vlim);

        uint8x16_t mor = vorrq_u8(vorrq_u8(m0, m1), vorrq_u8(m2, m3));
        uint64x2_t mor64 = vreinterpretq_u64_u8(mor);
        if (WSAP_VTCORE_EXPECT((vgetq_lane_u64(mor64, 0) | vgetq_lane_u64(mor64, 1)) != 0, 0)) {
            uint64x2_t m0_64 = vreinterpretq_u64_u8(m0);
            uint64_t lo0 = vgetq_lane_u64(m0_64, 0), hi0 = vgetq_lane_u64(m0_64, 1);
            if (lo0) return i + (wsap_vtcore_ctz64(lo0) >> 3);
            if (hi0) return i + 8 + (wsap_vtcore_ctz64(hi0) >> 3);

            uint64x2_t m1_64 = vreinterpretq_u64_u8(m1);
            uint64_t lo1 = vgetq_lane_u64(m1_64, 0), hi1 = vgetq_lane_u64(m1_64, 1);
            if (lo1) return i + 16 + (wsap_vtcore_ctz64(lo1) >> 3);
            if (hi1) return i + 24 + (wsap_vtcore_ctz64(hi1) >> 3);

            uint64x2_t m2_64 = vreinterpretq_u64_u8(m2);
            uint64_t lo2 = vgetq_lane_u64(m2_64, 0), hi2 = vgetq_lane_u64(m2_64, 1);
            if (lo2) return i + 32 + (wsap_vtcore_ctz64(lo2) >> 3);
            if (hi2) return i + 40 + (wsap_vtcore_ctz64(hi2) >> 3);

            uint64x2_t m3_64 = vreinterpretq_u64_u8(m3);
            uint64_t lo3 = vgetq_lane_u64(m3_64, 0), hi3 = vgetq_lane_u64(m3_64, 1);
            if (lo3) return i + 48 + (wsap_vtcore_ctz64(lo3) >> 3);
            if (hi3) return i + 56 + (wsap_vtcore_ctz64(hi3) >> 3);
        }
    }
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8((const uint8_t *)buf + i);
        uint8x16_t mask = vcgeq_u8(vsubq_u8(v, v20), vlim);
        uint64x2_t m64 = vreinterpretq_u64_u8(mask);
        uint64_t lo = vgetq_lane_u64(m64, 0);
        uint64_t hi64 = vgetq_lane_u64(m64, 1);
        if (lo) return i + (wsap_vtcore_ctz64(lo) >> 3);
        if (hi64) return i + 8 + (wsap_vtcore_ctz64(hi64) >> 3);
    }
    if (i + 8 <= len) {
        uint8x8_t v8 = vld1_u8((const uint8_t *)buf + i);
        uint8x8_t mask8 = vcge_u8(vsub_u8(v8, vget_low_u8(v20)), vget_low_u8(vlim));
        uint64_t m64 = vget_lane_u64(vreinterpret_u64_u8(mask8), 0);
        if (m64) return i + (wsap_vtcore_ctz64(m64) >> 3);
        i += 8;
    }
#elif defined(__x86_64__)
    const __m128i v_offset = _mm_set1_epi8((char)(0x20 + 0x80));
    const __m128i v_limit = _mm_set1_epi8((char)(hi - 0xa1));
    for (; i + 64 <= len; i += 64) {
        __m128i m0 = _mm_cmpgt_epi8(_mm_sub_epi8(_mm_loadu_si128((const __m128i *)(buf + i)), v_offset), v_limit);
        __m128i m1 = _mm_cmpgt_epi8(_mm_sub_epi8(_mm_loadu_si128((const __m128i *)(buf + i + 16)), v_offset), v_limit);
        __m128i m2 = _mm_cmpgt_epi8(_mm_sub_epi8(_mm_loadu_si128((const __m128i *)(buf + i + 32)), v_offset), v_limit);
        __m128i m3 = _mm_cmpgt_epi8(_mm_sub_epi8(_mm_loadu_si128((const __m128i *)(buf + i + 48)), v_offset), v_limit);
        if (WSAP_VTCORE_EXPECT(_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(m0, m1), _mm_or_si128(m2, m3))) != 0, 0)) {
            uint64_t mask = (uint64_t)(unsigned)_mm_movemask_epi8(m0) |
                            (uint64_t)(unsigned)_mm_movemask_epi8(m1) << 16 |
                            (uint64_t)(unsigned)_mm_movemask_epi8(m2) << 32 |
                            (uint64_t)(unsigned)_mm_movemask_epi8(m3) << 48;
            return i + (size_t)wsap_vtcore_ctz64(mask);
        }
    }
    for (; i + 16 <= len; i += 16) {
        int mask = _mm_movemask_epi8(_mm_cmpgt_epi8(
            _mm_sub_epi8(_mm_loadu_si128((const __m128i *)(buf + i)), v_offset), v_limit));
        if (mask != 0)
            return i + (size_t)wsap_vtcore_ctz32((unsigned)mask);
    }
#endif
    for (; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 || c >= hi)
            return i;
    }
    return len;
}

static inline WSAP_VTCORE_ALWAYS_INLINE void bytes_copy_small(uint8_t *restrict dst,
                                                                   const char *restrict src, int n)
{
    if (WSAP_VTCORE_EXPECT((unsigned)(n - 16) <= 16u, 1)) {
        memcpy(dst, src, 16);
        memcpy(dst + n - 16, src + n - 16, 16);
    } else if (n > 256) {
        memcpy(dst, src, (size_t)n);
    } else if (n >= 16) {
        int k = 0;
        for (; k + 16 <= n; k += 16)
            memcpy(dst + k, src + k, 16);
        if (k < n)
            memcpy(dst + n - 16, src + n - 16, 16);
    } else if (n >= 8) {
        memcpy(dst, src, 8);
        memcpy(dst + n - 8, src + n - 8, 8);
    } else if (n >= 4) {
        memcpy(dst, src, 4);
        memcpy(dst + n - 4, src + n - 4, 4);
    } else if (n >= 2) {
        memcpy(dst, src, 2);
        memcpy(dst + n - 2, src + n - 2, 2);
    } else if (n == 1) {
        dst[0] = (uint8_t)src[0];
    }
}

static inline size_t scan_crlf_tail(const char *buf, size_t i, size_t len)
{
    while (i + 1 < len && buf[i] == '\r' && buf[i + 1] == '\n')
        i += 2;
    return i;
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline size_t scan_crlf_pairs_avx512(const char *buf, size_t len)
{
    size_t i = 0;
    const __m512i pair = _mm512_set1_epi16(0x0A0D);
    for (; i + 64 <= len; i += 64) {
        uint64_t eq = _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void *)(buf + i)), pair);
        if (eq != ~0ull)
            return i + ((size_t)wsap_vtcore_ctz64(~eq) & ~(size_t)1);
    }
    return scan_crlf_tail(buf, i, len);
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline size_t scan_crlf_pairs_avx2(const char *buf, size_t len)
{
    size_t i = 0;
    const __m256i pair = _mm256_set1_epi16(0x0A0D);
    for (; i + 32 <= len; i += 32) {
        uint32_t eq = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(const void *)(buf + i)), pair));
        if (eq != ~0u)
            return i + ((size_t)wsap_vtcore_ctz32(~eq) & ~(size_t)1);
    }
    return scan_crlf_tail(buf, i, len);
}
#endif

static inline size_t scan_crlf_pairs(const char *buf, size_t len)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return scan_crlf_pairs_avx512(buf, len);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return scan_crlf_pairs_avx2(buf, len);
#endif
    size_t i = 0;
#if defined(__aarch64__)
    const uint8x16_t pair = vreinterpretq_u8_u16(vdupq_n_u16(0x0A0D));
    for (; i + 16 <= len; i += 16) {
        uint8x16_t eq = vceqq_u8(vld1q_u8((const uint8_t *)buf + i), pair);
        uint16_t bits = wsap_vtcore_lane_bits(eq);
        if (bits != 0xFFFF)
            return i + ((size_t)wsap_vtcore_ctz32((unsigned)~bits & 0xFFFFu) & ~(size_t)1);
    }
#endif
    return scan_crlf_tail(buf, i, len);
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline size_t scan_byte_run_avx512(const char *buf, size_t len, char byte)
{
    size_t i = 0;
    const __m512i want = _mm512_set1_epi8(byte);
    for (; i + 64 <= len; i += 64) {
        uint64_t eq = _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void *)(buf + i)), want);
        if (eq != ~0ull)
            return i + (size_t)wsap_vtcore_ctz64(~eq);
    }
    while (i < len && buf[i] == byte)
        i++;
    return i;
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline size_t scan_byte_run_avx2(const char *buf, size_t len, char byte)
{
    size_t i = 0;
    const __m256i want = _mm256_set1_epi8(byte);
    for (; i + 32 <= len; i += 32) {
        uint32_t eq = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(const void *)(buf + i)), want));
        if (eq != ~0u)
            return i + (size_t)wsap_vtcore_ctz32(~eq);
    }
    while (i < len && buf[i] == byte)
        i++;
    return i;
}
#endif

static inline size_t scan_byte_run(const char *buf, size_t len, char byte)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return scan_byte_run_avx512(buf, len, byte);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return scan_byte_run_avx2(buf, len, byte);
#endif
    size_t i = 0;
#if defined(__aarch64__)
    const uint8x16_t want = vdupq_n_u8((uint8_t)byte);
    for (; i + 16 <= len; i += 16) {
        uint16_t bits = wsap_vtcore_lane_bits(vceqq_u8(vld1q_u8((const uint8_t *)buf + i), want));
        if (bits != 0xFFFF)
            return i + (size_t)wsap_vtcore_ctz32((unsigned)~bits & 0xFFFFu);
    }
#elif defined(__x86_64__)
    const __m128i want = _mm_set1_epi8(byte);
    for (; i + 16 <= len; i += 16) {
        unsigned eq = (unsigned)_mm_movemask_epi8(
            _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(const void *)(buf + i)), want));
        if (eq != 0xFFFFu)
            return i + (size_t)wsap_vtcore_ctz32(~eq & 0xFFFFu);
    }
#endif
    while (i < len && buf[i] == byte)
        i++;
    return i;
}

static inline int scan_ascii_run(const char *buf, int len)
{
    return (int)scan_printable_span(buf, (size_t)len, 0x7f);
}

static inline int scan_ascii_lf_tail(const char *buf, int i, int len, uint16_t *ends, int count)
{
    for (; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == '\n') ends[++count] = (uint16_t)(i + 1);
        else if (ch - 0x20u >= 0x5fu) break;
    }
    return count;
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline int store_lf_offsets_avx512(uint16_t *ends, uint64_t nl,
                                         __m512i lanes, __m128i block)
{
    unsigned n = (unsigned)wsap_vtcore_popcount64(nl);
    __m512i packed = _mm512_maskz_compress_epi8(nl, lanes);
    __m128i offsets = _mm_add_epi16(_mm_cvtepu8_epi16(_mm512_castsi512_si128(packed)), block);
    _mm_storeu_si128((__m128i *)ends, offsets);
    if (WSAP_VTCORE_EXPECT(n > 8, 0)) {
        __m512i base = _mm512_broadcast_i32x4(block);
        __m512i wide = _mm512_add_epi16(_mm512_cvtepu8_epi16(_mm512_castsi512_si256(packed)), base);
        _mm512_storeu_si512(ends, wide);
        if (n > 32) {
            wide = _mm512_add_epi16(_mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(packed, 1)), base);
            _mm512_storeu_si512(ends + 32, wide);
        }
    }
    return (int)n;
}

WSAP_VTCORE_TARGET_AVX512 WSAP_VTCORE_MAYBE_UNUSED
static int scan_ascii_lf_avx512(const char *buf, int len, uint16_t *ends)
{
    int i = 0, count = 0;
    ends[0] = 0;
    const __m512i lanes = _mm512_setr_epi32(
        0x03020100,0x07060504,0x0b0a0908,0x0f0e0d0c,
        0x13121110,0x17161514,0x1b1a1918,0x1f1e1d1c,
        0x23222120,0x27262524,0x2b2a2928,0x2f2e2d2c,
        0x33323130,0x37363534,0x3b3a3938,0x3f3e3d3c);
    __m128i block = _mm_set1_epi16(1);
    for (; i + 256 <= len; i += 256) {
        __m512i v[4];
        uint64_t nl[4];
        uint64_t bad = 0;
        for (int q = 0; q < 4; q++) {
            v[q] = _mm512_loadu_si512(buf + i + q * 64);
            nl[q] = _mm512_cmpeq_epi8_mask(v[q], _mm512_set1_epi8('\n'));
            bad |= _mm512_cmpge_epu8_mask(_mm512_sub_epi8(v[q], _mm512_set1_epi8(0x20)),
                                         _mm512_set1_epi8(0x5f)) & ~nl[q];
        }
        if (bad) break;
        for (int q = 0; q < 4; q++) {
            count += store_lf_offsets_avx512(ends + count + 1, nl[q], lanes, block);
            block = _mm_add_epi16(block, _mm_set1_epi16(64));
        }
    }
    for (; i + 64 <= len; i += 64) {
        __m512i v = _mm512_loadu_si512(buf + i);
        uint64_t nl = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\n'));
        uint64_t bad = _mm512_cmpge_epu8_mask(
            _mm512_sub_epi8(v, _mm512_set1_epi8(0x20)), _mm512_set1_epi8(0x5f)) & ~nl;
        if (bad) break;
        count += store_lf_offsets_avx512(ends + count + 1, nl, lanes, block);
        block = _mm_add_epi16(block, _mm_set1_epi16(64));
    }
    return scan_ascii_lf_tail(buf, i, len, ends, count);
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2 WSAP_VTCORE_MAYBE_UNUSED
static int scan_ascii_lf_avx2(const char *buf, int len, uint16_t *ends)
{
    int i = 0, count = 0;
    ends[0] = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(buf + i));
        uint32_t nl = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n')));
        __m256i d = _mm256_sub_epi8(v, _mm256_set1_epi8((char)0xa0));
        uint32_t bad = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpgt_epi8(d, _mm256_set1_epi8((char)0xde))) & ~nl;
        if (bad) break;
        while (nl) {
            ends[++count] = (uint16_t)(i + wsap_vtcore_ctz32(nl) + 1);
            nl &= nl - 1;
        }
    }
    return scan_ascii_lf_tail(buf, i, len, ends, count);
}
#endif

static WSAP_VTCORE_NOINLINE WSAP_VTCORE_MAYBE_UNUSED int scan_ascii_lf(const char *buf, int len, uint16_t *ends)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return scan_ascii_lf_avx512(buf, len, ends);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return scan_ascii_lf_avx2(buf, len, ends);
#endif
    int i = 0, count = 0;
    ends[0] = 0;
#if defined(__aarch64__)
    for (; i + 32 <= len; i += 32) {
        uint32_t nl = 0, bad = 0;
        for (int q = 0; q < 2; q++) {
            uint8x16_t v = vld1q_u8((const uint8_t *)buf + i + q * 16);
            nl |= (uint32_t)wsap_vtcore_lane_bits(vceqq_u8(v, vdupq_n_u8('\n'))) << (q * 16);
            bad |= (uint32_t)wsap_vtcore_lane_bits(vcgeq_u8(vsubq_u8(v, vdupq_n_u8(0x20)),
                                                    vdupq_n_u8(0x5f))) << (q * 16);
        }
        bad &= ~nl;
        if (bad) break;
        while (nl) {
            ends[++count] = (uint16_t)(i + wsap_vtcore_ctz32(nl) + 1);
            nl &= nl - 1;
        }
    }
#endif
    return scan_ascii_lf_tail(buf, i, len, ends, count);
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline void fill_cells_avx512(struct cell * restrict row, int cols, uint32_t fg, uint32_t bg)
{
    __m128i cell = _mm_setr_epi32(' ', (int)fg, (int)bg, 0);
    __m512i blank = _mm512_broadcast_i32x4(cell);
    int c = 0;
    for (; c + 8 <= cols; c += 8) {
        _mm512_storeu_si512((void *)(row + c), blank);
        _mm512_storeu_si512((void *)(row + c + 4), blank);
    }
    int rem = cols - c;
    if (rem >= 4) {
        _mm512_storeu_si512((void *)(row + c), blank);
        c += 4;
        rem -= 4;
    }
    if (rem > 0) {
        uint16_t store_mask = (uint16_t)((1u << (4 * rem)) - 1);
        _mm512_mask_storeu_epi32((void *)(row + c), store_mask, blank);
    }
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline void fill_cells_avx2(struct cell * restrict row, int cols, uint32_t fg, uint32_t bg)
{
    struct cell fill = { .c = ' ', .fg = fg, .bg = bg, .flags = 0, .pad = 0 };
    __m256i blank = _mm256_setr_epi32(' ', (int)fg, (int)bg, 0, ' ', (int)fg, (int)bg, 0);
    int c = 0;
    for (; c + 8 <= cols; c += 8) {
        char *d = (char *)(row + c);
        _mm256_storeu_si256((__m256i *)(d), blank);
        _mm256_storeu_si256((__m256i *)(d + 32), blank);
        _mm256_storeu_si256((__m256i *)(d + 64), blank);
        _mm256_storeu_si256((__m256i *)(d + 96), blank);
    }
    for (; c < cols; c++) {
        row[c] = fill;
    }
}
#endif

static inline void fill_cells(struct cell * restrict row, int cols, uint32_t fg, uint32_t bg)
{
    struct cell fill = { .c = ' ', .fg = fg, .bg = bg, .flags = 0, .pad = 0 };
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512()) {
        if (WSAP_VTCORE_EXPECT(cols > 0, 1))
            fill_cells_avx512(row, cols, fg, bg);
        return;
    }
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2() && WSAP_VTCORE_EXPECT(cols >= 8, 1)) {
        fill_cells_avx2(row, cols, fg, bg);
        return;
    }
#endif
#if defined(__aarch64__)
    if (WSAP_VTCORE_EXPECT(cols > 0, 1)) {
        uint32x4_t blank = { ' ', fg, bg, 0 };
        uint32x4x4_t b4 = {{ blank, blank, blank, blank }};
        int c = 0;
        for (; c + 8 <= cols; c += 8) {
            vst1q_u32_x4((uint32_t *)(void *)(row + c), b4);
            vst1q_u32_x4((uint32_t *)(void *)(row + c + 4), b4);
        }
        for (; c + 4 <= cols; c += 4) {
            vst1q_u32_x4((uint32_t *)(void *)(row + c), b4);
        }
        for (; c < cols; c++) {
            vst1q_u32((uint32_t *)(void *)(row + c), blank);
        }
        return;
    }
#elif defined(__x86_64__)
    {
        __m128i blank = _mm_setr_epi32(' ', (int)fg, (int)bg, 0);
        int c = 0;
        for (; c + 4 <= cols; c += 4) {
            _mm_storeu_si128((__m128i *)(void *)(row + c), blank);
            _mm_storeu_si128((__m128i *)(void *)(row + c + 1), blank);
            _mm_storeu_si128((__m128i *)(void *)(row + c + 2), blank);
            _mm_storeu_si128((__m128i *)(void *)(row + c + 3), blank);
        }
        for (; c < cols; c++)
            _mm_storeu_si128((__m128i *)(void *)(row + c), blank);
        return;
    }
#endif
    for (int c = 0; c < cols; c++) {
        row[c] = fill;
    }
}

static inline void clear_cells_default(struct cell *row, int cols)
{
    fill_cells(row, cols, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline void put_ascii_chunk_avx512(struct cell * restrict dst, const char * restrict src, int n,
                                         uint32_t fg, uint32_t bg, uint32_t flags)
{
    int k = 0;
    __m128i cell = _mm_setr_epi32(0, (int)fg, (int)bg, (int)flags);
    __m512i base = _mm512_broadcast_i32x4(cell);
    const __m512i idx0 = _mm512_setr_epi32(0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0);
    const __m512i idx1 = _mm512_setr_epi32(4, 0, 0, 0, 5, 0, 0, 0, 6, 0, 0, 0, 7, 0, 0, 0);
    const __m512i idx2 = _mm512_setr_epi32(8, 0, 0, 0, 9, 0, 0, 0, 10, 0, 0, 0, 11, 0, 0, 0);
    const __m512i idx3 = _mm512_setr_epi32(12, 0, 0, 0, 13, 0, 0, 0, 14, 0, 0, 0, 15, 0, 0, 0);

    for (; k + 16 <= n; k += 16) {
        __m512i cps = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *)(src + k)));
        _mm512_storeu_si512(dst + k + 0, _mm512_mask_permutexvar_epi32(base, 0x1111, idx0, cps));
        _mm512_storeu_si512(dst + k + 4, _mm512_mask_permutexvar_epi32(base, 0x1111, idx1, cps));
        _mm512_storeu_si512(dst + k + 8, _mm512_mask_permutexvar_epi32(base, 0x1111, idx2, cps));
        _mm512_storeu_si512(dst + k + 12, _mm512_mask_permutexvar_epi32(base, 0x1111, idx3, cps));
    }
    int rem = n - k;
    if (rem > 0) {
        __m128i v = _mm_maskz_loadu_epi8((1 << rem) - 1, src + k);
        __m512i cps = _mm512_cvtepu8_epi32(v);
        if (rem <= 4) {
            uint16_t m = (uint16_t)((1u << (4 * rem)) - 1);
            _mm512_mask_storeu_epi32(dst + k, m, _mm512_mask_permutexvar_epi32(base, 0x1111, idx0, cps));
            return;
        }
        _mm512_storeu_si512(dst + k, _mm512_mask_permutexvar_epi32(base, 0x1111, idx0, cps));
        if (rem <= 8) {
            uint16_t m = (uint16_t)((1u << (4 * (rem - 4))) - 1);
            _mm512_mask_storeu_epi32(dst + k + 4, m, _mm512_mask_permutexvar_epi32(base, 0x1111, idx1, cps));
            return;
        }
        _mm512_storeu_si512(dst + k + 4, _mm512_mask_permutexvar_epi32(base, 0x1111, idx1, cps));
        if (rem <= 12) {
            uint16_t m = (uint16_t)((1u << (4 * (rem - 8))) - 1);
            _mm512_mask_storeu_epi32(dst + k + 8, m, _mm512_mask_permutexvar_epi32(base, 0x1111, idx2, cps));
            return;
        }
        _mm512_storeu_si512(dst + k + 8, _mm512_mask_permutexvar_epi32(base, 0x1111, idx2, cps));
        uint16_t m = (uint16_t)((1u << (4 * (rem - 12))) - 1);
        _mm512_mask_storeu_epi32(dst + k + 12, m, _mm512_mask_permutexvar_epi32(base, 0x1111, idx3, cps));
    }
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline void put_ascii_chunk_avx2(struct cell * restrict dst, const char * restrict src, int n,
                                       uint32_t fg, uint32_t bg, uint32_t flags)
{
    int k = 0;
    __m128i cell = _mm_setr_epi32(0, (int)fg, (int)bg, (int)flags);
    __m256i base = _mm256_broadcastsi128_si256(cell);
#define ASCII_PAIR(o, q) _mm256_storeu_si256((__m256i *)(void *)(dst + (o) + 2 * (q)), \
        _mm256_blend_epi32(_mm256_permutevar8x32_epi32(cps, \
            _mm256_setr_epi32(2*(q), 0, 0, 0, 2*(q)+1, 0, 0, 0)), base, 0xee))
#define ASCII_8(o) do { \
        __m256i cps = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(void *)(src + (o)))); \
        ASCII_PAIR(o, 0); ASCII_PAIR(o, 1); ASCII_PAIR(o, 2); ASCII_PAIR(o, 3); \
    } while (0)
    for (; k + 16 <= n; k += 16) {
        ASCII_8(k);
        ASCII_8(k + 8);
    }
    if (k + 8 <= n) {
        ASCII_8(k);
        k += 8;
    }
    if (k < n && n >= 8) {
        ASCII_8(n - 8);
        k = n;
    }
#undef ASCII_8
#undef ASCII_PAIR
    for (; k < n; k++)
        _mm_storeu_si128((__m128i *)(void *)(dst + k),
            _mm_or_si128(cell, _mm_cvtsi32_si128((unsigned char)src[k])));
}
#endif

#if defined(__aarch64__)
static inline void store_ascii_quad_neon(struct cell *dst, uint32x4_t cps,
                                         uint32x4_t fg, uint32x4_t bg_flags)
{
    uint64x2_t lo = vreinterpretq_u64_u32(vzip1q_u32(cps, fg));
    uint64x2_t hi = vreinterpretq_u64_u32(vzip2q_u32(cps, fg));
    uint64x2_t tail = vreinterpretq_u64_u32(bg_flags);
    uint32x4x4_t cells = {{
        vreinterpretq_u32_u64(vzip1q_u64(lo, tail)),
        vreinterpretq_u32_u64(vzip2q_u64(lo, tail)),
        vreinterpretq_u32_u64(vzip1q_u64(hi, tail)),
        vreinterpretq_u32_u64(vzip2q_u64(hi, tail)),
    }};
    vst1q_u32_x4((uint32_t *)(void *)dst, cells);
}

static inline void put_ascii_quad_neon(struct cell *dst, const char *src,
                                       uint32x4_t fg, uint32x4_t bg_flags)
{
    uint32_t bytes;
    memcpy(&bytes, src, sizeof(bytes));
    uint16x4_t cps = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(bytes))));
    store_ascii_quad_neon(dst, vmovl_u16(cps), fg, bg_flags);
}
#endif

WSAP_VTCORE_ALWAYS_INLINE
static inline void put_ascii_chunk(struct cell * restrict dst, const char * restrict src, int n,
                                  uint32_t fg, uint32_t bg, uint32_t flags)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (n >= 16 && wsap_vtcore_use_avx512()) {
        put_ascii_chunk_avx512(dst, src, n, fg, bg, flags);
        return;
    }
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (n >= 16 && wsap_vtcore_use_avx2()) {
        put_ascii_chunk_avx2(dst, src, n, fg, bg, flags);
        return;
    }
#endif
    int k = 0;
#if defined(__aarch64__)
    uint32x4_t fg4 = vdupq_n_u32(fg);
    uint32x4_t bg_flags = vcombine_u32(vcreate_u32(((uint64_t)flags << 32) | bg),
                                       vcreate_u32(((uint64_t)flags << 32) | bg));
    for (; k + 8 <= n; k += 8) {
        uint16x8_t cp16 = vmovl_u8(vld1_u8((const uint8_t *)src + k));
        store_ascii_quad_neon(dst + k, vmovl_u16(vget_low_u16(cp16)), fg4, bg_flags);
        store_ascii_quad_neon(dst + k + 4, vmovl_u16(vget_high_u16(cp16)), fg4, bg_flags);
    }
    if (k + 4 <= n) {
        put_ascii_quad_neon(dst + k, src + k, fg4, bg_flags);
        k += 4;
    }
    if (k < n && n >= 4) {
        put_ascii_quad_neon(dst + n - 4, src + n - 4, fg4, bg_flags);
        k = n;
    }
#elif defined(__x86_64__)
    const __m128i zero = _mm_setzero_si128();
    const __m128i fg4 = _mm_set1_epi32((int)fg);
    const __m128i bg_flags = _mm_setr_epi32((int)bg, (int)flags, (int)bg, (int)flags);
#define ASCII_4(o, cp32) do { \
        __m128i c_fg_01 = _mm_unpacklo_epi32((cp32), fg4); \
        __m128i c_fg_23 = _mm_unpackhi_epi32((cp32), fg4); \
        _mm_storeu_si128((__m128i *)(void *)(dst + (o) + 0), _mm_unpacklo_epi64(c_fg_01, bg_flags)); \
        _mm_storeu_si128((__m128i *)(void *)(dst + (o) + 1), _mm_unpackhi_epi64(c_fg_01, bg_flags)); \
        _mm_storeu_si128((__m128i *)(void *)(dst + (o) + 2), _mm_unpacklo_epi64(c_fg_23, bg_flags)); \
        _mm_storeu_si128((__m128i *)(void *)(dst + (o) + 3), _mm_unpackhi_epi64(c_fg_23, bg_flags)); \
    } while (0)
    for (; k + 8 <= n; k += 8) {
        __m128i cp16 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)(src + k)), zero);
        ASCII_4(k, _mm_unpacklo_epi16(cp16, zero));
        ASCII_4(k + 4, _mm_unpackhi_epi16(cp16, zero));
    }
#define ASCII_4_AT(o) do { \
        uint32_t bytes; \
        memcpy(&bytes, src + (o), sizeof(bytes)); \
        __m128i cp16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)bytes), zero); \
        ASCII_4(o, _mm_unpacklo_epi16(cp16, zero)); \
    } while (0)
    if (k + 4 <= n) {
        ASCII_4_AT(k);
        k += 4;
    }
    if (k < n && n >= 4) {
        ASCII_4_AT(n - 4);
        k = n;
    }
#undef ASCII_4_AT
#undef ASCII_4
#endif
    for (; k < n; k++) {
        dst[k] = (struct cell){
            .c = (unsigned char)src[k],
            .fg = fg,
            .bg = bg,
            .flags = (uint16_t)flags,
            .pad = (uint16_t)(flags >> 16),
        };
    }
}

#endif
