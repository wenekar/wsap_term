#ifndef WSAP_RENDER_CPU_KERNELS_H
#define WSAP_RENDER_CPU_KERNELS_H

/*
 * CPU pixel kernels.
 *
 * Pixels are 0xAARRGGBB in a uint32_t. A row kernel writes dst[0..n) and
 * nothing else. n can be any value; a value of 0 or less does nothing.
 * No pointer needs alignment. No kernel reads past the end of an input.
 * The inputs of a kernel must not overlap its dst.
 *
 * Coverage layouts:
 * - Alpha coverage is one byte per pixel (the struct glyph alpha rows).
 * - Subpixel coverage is three bytes per pixel, in the order red, green,
 *   blue (the struct glyph subpx rows, src/font/font.h). Byte 3*i is the
 *   red coverage of pixel i. Red coverage drives bits 16-23 of the pixel,
 *   green drives bits 8-15, and blue drives bits 0-7.
 * - Color glyph pixels are premultiplied ARGB.
 *
 * Each kernel has one variant per level: _scalar, _avx2, _avx512 (x86-64),
 * and _neon (aarch64). The _scalar variants are the reference. They use the
 * paint/blend.h helpers directly. Every other variant gives the same bits as
 * _scalar for every input. The render_kernel_* dispatchers take a constant
 * level first. Inline them into a per-level wrapper, and the compiler keeps
 * only the matching variant.
 *
 * The SIMD variants use the lane forms below in place of some divisions.
 * kernels_test checks each lane form over its whole input domain.
 *
 * Some variants use the scalar math because it is faster on the measured
 * CPU (Zen 5), where a gather costs more per element than a scalar load:
 * - compose_alpha_lut and compose_subpx_lut use scalar loads at every level.
 * - blend_any_subpx at AVX2 runs the scalar reference.
 * - blend_any and blend_any_subpx run the scalar reference for n <= 8. A
 *   block with at most RENDER_SCALAR_LANES pixels that need the blend
 *   formula runs those pixels with the scalar form.
 * - The NEON variants of the blend kernels run the scalar reference.
 */

#include "paint/blend.h"
#include "render/cpu/isa.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(RENDER_KERNEL_X86)
#include <immintrin.h>
#endif
#if defined(RENDER_KERNEL_NEON)
#include <arm_neon.h>
#endif

/* ---- Reference formulas ---------------------------------------------- */

/* Premultiplied ARGB over bg. The output is opaque. Faint halves the coverage. */
static inline uint32_t render_blend_premul_px(uint32_t premul, uint32_t bg, bool faint)
{
    unsigned a = premul >> 24;
    if (!a) return bg;
    unsigned coverage = faint ? (a + 1) / 2 : a;
    uint32_t result = 0xFF000000;

    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned channel = (((premul >> shift) & 255) * 255 + a / 2) / a;
        if (channel > 255) channel = 255;
        result |= blend_channel(channel, (bg >> shift) & 255, coverage) << shift;
    }
    return result;
}

/* ---- Lane forms ------------------------------------------------------ */

/* floor(t / 255) for t < 2^24 - 1. */
static inline uint32_t render_div255_wide(uint32_t t)
{
    uint32_t h = t >> 8;
    uint32_t z = h + (t & 255) + 1;
    return h + ((z + (z << 8)) >> 16);
}

/* floor(x / 255) for x < 65535. */
static inline uint32_t render_div255_narrow(uint32_t x)
{
    return (x + 1 + (x >> 8)) >> 8;
}

/* floor(x / 10000) for x <= 655355000, the blend_lum16 numerator range. */
static inline uint32_t render_div10000(uint32_t x)
{
    return (uint32_t)(((uint64_t)x * 3518437209u) >> 45);
}

/* Truncating num / den in double precision, for |num| < 2^25 and 0 < |den| < 2^16. */
static inline int render_div_f64(int num, int den)
{
    return (int)((double)num / (double)den);
}

/* floor(x / a) in single precision, for 0 <= x < 2^16 and 0 < a < 256. */
static inline int render_div_f32(int x, int a)
{
    return (int)((float)x / (float)a);
}

/* srgb_coverage() in the lane form. The test checks it for every input. */
static inline uint8_t render_srgb_coverage_lanes(int fg_s, int bg_s, uint8_t a)
{
    int mix = fg_s * a + bg_s * (255 - a);
    int i = (int)render_div255_narrow((uint32_t)mix);
    int f = mix - i * 255;
    int lo = srgb_to_linear[i], hi = srgb_to_linear[i + 1];
    int blend_l = lo + (int)render_div255_wide((uint32_t)((hi - lo) * f));
    int bg_l = srgb_to_linear[bg_s];
    int diff = srgb_to_linear[fg_s] - bg_l;
    if (!diff)
        return a;
    int c = render_div_f64((blend_l - bg_l) * 255 + diff / 2, diff);
    return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
}

/* blend_lum16() in the lane form. */
static inline unsigned render_lum16_lanes(uint32_t rgb)
{
    return render_div10000(2126u * srgb_to_linear[(rgb >> 16) & 255] +
                           7152u * srgb_to_linear[(rgb >> 8) & 255] +
                            722u * srgb_to_linear[rgb & 255] + 5000);
}

/* Load n bytes (0..8) into the low bytes of a word. No read leaves [p, p + n). */
static inline uint64_t render_load_bytes(const uint8_t *p, int n)
{
    if (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        return v;
    }
    if (n >= 4) {
        uint32_t lo, hi;
        memcpy(&lo, p, 4);
        memcpy(&hi, p + n - 4, 4);
        return lo | (uint64_t)hi << ((n - 4) * 8);
    }
    if (n <= 0)
        return 0;
    return (uint64_t)p[0] | (uint64_t)p[n >> 1] << ((n >> 1) * 8) |
           (uint64_t)p[n - 1] << ((n - 1) * 8);
}

/* ---- Scalar ---------------------------------------------------------- */

static inline void render_fill_scalar(uint32_t *dst, int n, uint32_t color)
{
    for (int i = 0; i < n; i++)
        dst[i] = color;
}

/* Tile rows are contiguous, cw pixels each. */
static inline void render_blit_tile_scalar(uint32_t *dst, size_t dst_stride_px,
                                           const uint32_t *tile, int cw, int ch)
{
    if (cw <= 0)
        return;
    for (int y = 0; y < ch; y++)
        memcpy(dst + (size_t)y * dst_stride_px, tile + (size_t)y * (size_t)cw,
               (size_t)cw * sizeof(*dst));
}

static inline void render_compose_alpha_lut_scalar(uint32_t *dst, const uint8_t *cov,
                                                   int n, const uint32_t *lut256)
{
    for (int i = 0; i < n; i++)
        dst[i] = lut256[cov[i]];
}

static inline void render_compose_subpx_lut_scalar(uint32_t *dst, const uint8_t *cov, int n,
                                                   const struct subpx_lut *lut, uint32_t bg)
{
    for (int i = 0; i < n; i++)
        dst[i] = blend_subpx_lut(lut, bg, cov[3 * i], cov[3 * i + 1], cov[3 * i + 2]);
}

static inline void render_compose_color_scalar(uint32_t *dst, const uint32_t *premul,
                                               int n, bool faint)
{
    for (int i = 0; i < n; i++)
        dst[i] = render_blend_premul_px(premul[i], dst[i], faint);
}

/* dst[i] = blend_px(fg, dst[i], cov[i]) for any dst alpha. */
static inline void render_blend_any_scalar(uint32_t *dst, const uint8_t *cov, int n,
                                           uint32_t fg)
{
    if (n <= 0)
        return;
    int fg_l = (int)blend_lum16(fg);
    for (int i = 0; i < n; i++) {
        uint8_t a = cov[i];
        if (!a)
            continue;
        if (a == 255) {
            dst[i] = fg;
            continue;
        }
        dst[i] = blend_px_lum(fg, dst[i], fg_l, (int)blend_lum16(dst[i]), a);
    }
}

/* dst[i] = blend_subpx(fg, dst[i], r, g, b) for any dst alpha. */
static inline void render_blend_any_subpx_scalar(uint32_t *dst, const uint8_t *cov, int n,
                                                 uint32_t fg)
{
    for (int i = 0; i < n; i++)
        dst[i] = blend_subpx(fg, dst[i], cov[3 * i], cov[3 * i + 1], cov[3 * i + 2]);
}

/* dst[i] = blend_px(color, dst[i], a): the translucent rect row. */
static inline void render_blend_rect_scalar(uint32_t *dst, int n, uint32_t color, uint8_t a)
{
    if (n <= 0 || !a)
        return;
    if (a == 255) {
        render_fill_scalar(dst, n, color);
        return;
    }
    int fg_l = (int)blend_lum16(color);
    uint32_t last_bg = dst[0];
    uint32_t last = blend_px_lum(color, last_bg, fg_l, (int)blend_lum16(last_bg), a);
    for (int i = 0; i < n; i++) {
        uint32_t bg = dst[i];
        if (bg != last_bg) {
            last_bg = bg;
            last = blend_px_lum(color, bg, fg_l, (int)blend_lum16(bg), a);
        }
        dst[i] = last;
    }
}

/* ---- x86-64: shared -------------------------------------------------- */

/*
 * On the CPUs we measured, a gather costs more per element than a scalar
 * load. So the SIMD blend kernels gather only for pixels that need the blend
 * formula. They move those pixels to the front lanes and gather at the
 * narrowest width that holds them. The LUT compose kernels use scalar loads.
 */

#if defined(RENDER_KERNEL_X86)

#define RENDER_V4(x) _mm_set1_epi32((int)(x))
#define RENDER_V8(x) _mm256_set1_epi32((int)(x))
#define RENDER_V16(x) _mm512_set1_epi32((int)(x))

/* dst[i] = lut256[cov[i]], four pixels per step. */
static inline RENDER_ALWAYS_INLINE void render_lut_rows(uint32_t *dst, const uint8_t *cov,
                                                        int n, const uint32_t *lut256)
{
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t c;
        memcpy(&c, cov + i, 4);
        dst[i] = lut256[c & 255];
        dst[i + 1] = lut256[(c >> 8) & 255];
        dst[i + 2] = lut256[(c >> 16) & 255];
        dst[i + 3] = lut256[c >> 24];
    }
    for (; i < n; i++)
        dst[i] = lut256[cov[i]];
}

/* The luminance of the last bg. The zero value is valid: blend_lum16(0) is 0. */
struct render_bg_lum {
    uint32_t bg;
    int bg_l;
};

/* blend_px() for one pixel, with the bg luminance from c. */
static inline RENDER_ALWAYS_INLINE uint32_t render_blend_px_cached(uint32_t fg, int fg_l,
                                                                   uint32_t bg, uint8_t a,
                                                                   struct render_bg_lum *c)
{
    if (c->bg != bg) {
        c->bg = bg;
        c->bg_l = (int)blend_lum16(bg);
    }
    return blend_px_lum(fg, bg, fg_l, c->bg_l, a);
}

/*
 * A blend kernel block with at most this many pixels that need the blend
 * formula runs them with the scalar form. Measured on Zen 5.
 */
#define RENDER_SCALAR_LANES 3

/* ---- x86-64: AVX2 ---------------------------------------------------- */

/*
 * Lane moves for 8 lanes. Entry m of render_avx2_pack holds, 3 bits each,
 * the lanes of the set bits of m in order. Entry m of render_avx2_unpack
 * holds, at lane j, the front position of lane j.
 */
static const uint32_t render_avx2_pack[256] = {
    0x000000, 0x000000, 0x000001, 0x000008, 0x000002, 0x000010, 0x000011, 0x000088,
    0x000003, 0x000018, 0x000019, 0x0000c8, 0x00001a, 0x0000d0, 0x0000d1, 0x000688,
    0x000004, 0x000020, 0x000021, 0x000108, 0x000022, 0x000110, 0x000111, 0x000888,
    0x000023, 0x000118, 0x000119, 0x0008c8, 0x00011a, 0x0008d0, 0x0008d1, 0x004688,
    0x000005, 0x000028, 0x000029, 0x000148, 0x00002a, 0x000150, 0x000151, 0x000a88,
    0x00002b, 0x000158, 0x000159, 0x000ac8, 0x00015a, 0x000ad0, 0x000ad1, 0x005688,
    0x00002c, 0x000160, 0x000161, 0x000b08, 0x000162, 0x000b10, 0x000b11, 0x005888,
    0x000163, 0x000b18, 0x000b19, 0x0058c8, 0x000b1a, 0x0058d0, 0x0058d1, 0x02c688,
    0x000006, 0x000030, 0x000031, 0x000188, 0x000032, 0x000190, 0x000191, 0x000c88,
    0x000033, 0x000198, 0x000199, 0x000cc8, 0x00019a, 0x000cd0, 0x000cd1, 0x006688,
    0x000034, 0x0001a0, 0x0001a1, 0x000d08, 0x0001a2, 0x000d10, 0x000d11, 0x006888,
    0x0001a3, 0x000d18, 0x000d19, 0x0068c8, 0x000d1a, 0x0068d0, 0x0068d1, 0x034688,
    0x000035, 0x0001a8, 0x0001a9, 0x000d48, 0x0001aa, 0x000d50, 0x000d51, 0x006a88,
    0x0001ab, 0x000d58, 0x000d59, 0x006ac8, 0x000d5a, 0x006ad0, 0x006ad1, 0x035688,
    0x0001ac, 0x000d60, 0x000d61, 0x006b08, 0x000d62, 0x006b10, 0x006b11, 0x035888,
    0x000d63, 0x006b18, 0x006b19, 0x0358c8, 0x006b1a, 0x0358d0, 0x0358d1, 0x1ac688,
    0x000007, 0x000038, 0x000039, 0x0001c8, 0x00003a, 0x0001d0, 0x0001d1, 0x000e88,
    0x00003b, 0x0001d8, 0x0001d9, 0x000ec8, 0x0001da, 0x000ed0, 0x000ed1, 0x007688,
    0x00003c, 0x0001e0, 0x0001e1, 0x000f08, 0x0001e2, 0x000f10, 0x000f11, 0x007888,
    0x0001e3, 0x000f18, 0x000f19, 0x0078c8, 0x000f1a, 0x0078d0, 0x0078d1, 0x03c688,
    0x00003d, 0x0001e8, 0x0001e9, 0x000f48, 0x0001ea, 0x000f50, 0x000f51, 0x007a88,
    0x0001eb, 0x000f58, 0x000f59, 0x007ac8, 0x000f5a, 0x007ad0, 0x007ad1, 0x03d688,
    0x0001ec, 0x000f60, 0x000f61, 0x007b08, 0x000f62, 0x007b10, 0x007b11, 0x03d888,
    0x000f63, 0x007b18, 0x007b19, 0x03d8c8, 0x007b1a, 0x03d8d0, 0x03d8d1, 0x1ec688,
    0x00003e, 0x0001f0, 0x0001f1, 0x000f88, 0x0001f2, 0x000f90, 0x000f91, 0x007c88,
    0x0001f3, 0x000f98, 0x000f99, 0x007cc8, 0x000f9a, 0x007cd0, 0x007cd1, 0x03e688,
    0x0001f4, 0x000fa0, 0x000fa1, 0x007d08, 0x000fa2, 0x007d10, 0x007d11, 0x03e888,
    0x000fa3, 0x007d18, 0x007d19, 0x03e8c8, 0x007d1a, 0x03e8d0, 0x03e8d1, 0x1f4688,
    0x0001f5, 0x000fa8, 0x000fa9, 0x007d48, 0x000faa, 0x007d50, 0x007d51, 0x03ea88,
    0x000fab, 0x007d58, 0x007d59, 0x03eac8, 0x007d5a, 0x03ead0, 0x03ead1, 0x1f5688,
    0x000fac, 0x007d60, 0x007d61, 0x03eb08, 0x007d62, 0x03eb10, 0x03eb11, 0x1f5888,
    0x007d63, 0x03eb18, 0x03eb19, 0x1f58c8, 0x03eb1a, 0x1f58d0, 0x1f58d1, 0xfac688,
};
static const uint32_t render_avx2_unpack[256] = {
    0x000000, 0x000000, 0x000000, 0x000008, 0x000000, 0x000040, 0x000040, 0x000088,
    0x000000, 0x000200, 0x000200, 0x000408, 0x000200, 0x000440, 0x000440, 0x000688,
    0x000000, 0x001000, 0x001000, 0x002008, 0x001000, 0x002040, 0x002040, 0x003088,
    0x001000, 0x002200, 0x002200, 0x003408, 0x002200, 0x003440, 0x003440, 0x004688,
    0x000000, 0x008000, 0x008000, 0x010008, 0x008000, 0x010040, 0x010040, 0x018088,
    0x008000, 0x010200, 0x010200, 0x018408, 0x010200, 0x018440, 0x018440, 0x020688,
    0x008000, 0x011000, 0x011000, 0x01a008, 0x011000, 0x01a040, 0x01a040, 0x023088,
    0x011000, 0x01a200, 0x01a200, 0x023408, 0x01a200, 0x023440, 0x023440, 0x02c688,
    0x000000, 0x040000, 0x040000, 0x080008, 0x040000, 0x080040, 0x080040, 0x0c0088,
    0x040000, 0x080200, 0x080200, 0x0c0408, 0x080200, 0x0c0440, 0x0c0440, 0x100688,
    0x040000, 0x081000, 0x081000, 0x0c2008, 0x081000, 0x0c2040, 0x0c2040, 0x103088,
    0x081000, 0x0c2200, 0x0c2200, 0x103408, 0x0c2200, 0x103440, 0x103440, 0x144688,
    0x040000, 0x088000, 0x088000, 0x0d0008, 0x088000, 0x0d0040, 0x0d0040, 0x118088,
    0x088000, 0x0d0200, 0x0d0200, 0x118408, 0x0d0200, 0x118440, 0x118440, 0x160688,
    0x088000, 0x0d1000, 0x0d1000, 0x11a008, 0x0d1000, 0x11a040, 0x11a040, 0x163088,
    0x0d1000, 0x11a200, 0x11a200, 0x163408, 0x11a200, 0x163440, 0x163440, 0x1ac688,
    0x000000, 0x200000, 0x200000, 0x400008, 0x200000, 0x400040, 0x400040, 0x600088,
    0x200000, 0x400200, 0x400200, 0x600408, 0x400200, 0x600440, 0x600440, 0x800688,
    0x200000, 0x401000, 0x401000, 0x602008, 0x401000, 0x602040, 0x602040, 0x803088,
    0x401000, 0x602200, 0x602200, 0x803408, 0x602200, 0x803440, 0x803440, 0xa04688,
    0x200000, 0x408000, 0x408000, 0x610008, 0x408000, 0x610040, 0x610040, 0x818088,
    0x408000, 0x610200, 0x610200, 0x818408, 0x610200, 0x818440, 0x818440, 0xa20688,
    0x408000, 0x611000, 0x611000, 0x81a008, 0x611000, 0x81a040, 0x81a040, 0xa23088,
    0x611000, 0x81a200, 0x81a200, 0xa23408, 0x81a200, 0xa23440, 0xa23440, 0xc2c688,
    0x200000, 0x440000, 0x440000, 0x680008, 0x440000, 0x680040, 0x680040, 0x8c0088,
    0x440000, 0x680200, 0x680200, 0x8c0408, 0x680200, 0x8c0440, 0x8c0440, 0xb00688,
    0x440000, 0x681000, 0x681000, 0x8c2008, 0x681000, 0x8c2040, 0x8c2040, 0xb03088,
    0x681000, 0x8c2200, 0x8c2200, 0xb03408, 0x8c2200, 0xb03440, 0xb03440, 0xd44688,
    0x440000, 0x688000, 0x688000, 0x8d0008, 0x688000, 0x8d0040, 0x8d0040, 0xb18088,
    0x688000, 0x8d0200, 0x8d0200, 0xb18408, 0x8d0200, 0xb18440, 0xb18440, 0xd60688,
    0x688000, 0x8d1000, 0x8d1000, 0xb1a008, 0x8d1000, 0xb1a040, 0xb1a040, 0xd63088,
    0x8d1000, 0xb1a200, 0xb1a200, 0xd63408, 0xb1a200, 0xd63440, 0xd63440, 0xfac688,
};

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_tail_mask(int r)
{
    return _mm256_cmpgt_epi32(_mm256_set1_epi32(r), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
}

/* Store lanes [0, r) of v, for 0 < r < 8. */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE void render_avx2_store_part(uint32_t *dst, __m256i v, int r)
{
    _mm256_maskstore_epi32((int *)dst, render_avx2_tail_mask(r), v);
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_load_part(const uint32_t *src, int r)
{
    return _mm256_maskload_epi32((const int *)src, render_avx2_tail_mask(r));
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_bytes(uint64_t bytes)
{
    return _mm256_cvtepu8_epi32(_mm_cvtsi64_si128((long long)bytes));
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE unsigned render_avx2_bits(__m256i m)
{
    return (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(m));
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE bool render_avx2_all(__m256i m)
{
    return _mm256_movemask_epi8(m) == -1;
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_index(uint32_t packed)
{
    return _mm256_and_si256(_mm256_srlv_epi32(RENDER_V8(packed),
                                              _mm256_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21)),
                            RENDER_V8(7));
}

/* Move the lanes in m to the front. front masks the first popcount(m) lanes. */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_pack_lanes(__m256i v, unsigned m,
                                                                  __m256i front)
{
    return _mm256_and_si256(
        _mm256_permutevar8x32_epi32(v, render_avx2_index(render_avx2_pack[m])), front);
}

/* Move the front lanes of v back to the lanes in m, over base. mv is m as a vector. */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_unpack_lanes(__m256i base, __m256i v,
                                                                    unsigned m, __m256i mv)
{
    return _mm256_blendv_epi8(
        base, _mm256_permutevar8x32_epi32(v, render_avx2_index(render_avx2_unpack[m])), mv);
}

/* Gathers for the front w lanes. With w <= 4, the other lanes are 0. */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_lin_pair(__m256i idx, int w)
{
    const int *base = (const int *)(const void *)srgb_to_linear;
    if (w <= 4)
        return _mm256_zextsi128_si256(_mm_i32gather_epi32(base, _mm256_castsi256_si128(idx), 2));
    return _mm256_i32gather_epi32(base, idx, 2);
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_lin(__m256i idx, int w)
{
    return _mm256_and_si256(render_avx2_lin_pair(idx, w), RENDER_V8(0xFFFF));
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_enc(__m256i idx, int w)
{
    const int *base = (const int *)(const void *)srgb_from_linear;
    __m256i v = w <= 4
        ? _mm256_zextsi128_si256(_mm_i32gather_epi32(base, _mm256_castsi256_si128(idx), 1))
        : _mm256_i32gather_epi32(base, idx, 1);
    return _mm256_and_si256(v, RENDER_V8(0xFF));
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_div255_wide(__m256i t)
{
    __m256i h = _mm256_srli_epi32(t, 8);
    __m256i z = _mm256_add_epi32(_mm256_add_epi32(h, _mm256_and_si256(t, RENDER_V8(255))),
                                 RENDER_V8(1));
    return _mm256_add_epi32(h, _mm256_srli_epi32(_mm256_add_epi32(z, _mm256_slli_epi32(z, 8)),
                                                 16));
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_div255_narrow(__m256i x)
{
    return _mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(x, RENDER_V8(1)),
                                              _mm256_srli_epi32(x, 8)), 8);
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_lum16(__m256i lr, __m256i lg, __m256i lb)
{
    __m256i n = _mm256_add_epi32(
        _mm256_add_epi32(_mm256_mullo_epi32(lr, RENDER_V8(2126)),
                         _mm256_mullo_epi32(lg, RENDER_V8(7152))),
        _mm256_add_epi32(_mm256_mullo_epi32(lb, RENDER_V8(722)), RENDER_V8(5000)));
    const __m256i m = _mm256_set1_epi64x(3518437209);
    __m256i even = _mm256_srli_epi64(_mm256_mul_epu32(n, m), 45);
    __m256i odd = _mm256_srli_epi64(_mm256_mul_epu32(_mm256_srli_epi64(n, 32), m), 13);
    return _mm256_blend_epi32(even, odd, 0xAA);
}

/* blend_channel() from linear fg and bg: lfg * a + lbg * ia, rounded, encoded. */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_channel(__m256i lfg, __m256i lbg,
                                                               __m256i a, __m256i ia, int w)
{
    __m256i t = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(lfg, a),
                                                  _mm256_mullo_epi32(lbg, ia)),
                                 RENDER_V8(127));
    return render_avx2_enc(render_avx2_div255_wide(t), w);
}

/* render_srgb_coverage_lanes() per lane. lfg_s and lbg_s are linear fg_s and bg_s. */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_coverage(__m256i fg_s, __m256i lfg_s,
                                                                __m256i bg_s, __m256i lbg_s,
                                                                __m256i a, int w)
{
    __m256i ia = _mm256_sub_epi32(RENDER_V8(255), a);
    __m256i mix = _mm256_madd_epi16(_mm256_or_si256(fg_s, _mm256_slli_epi32(bg_s, 16)),
                                    _mm256_or_si256(a, _mm256_slli_epi32(ia, 16)));
    __m256i i = render_avx2_div255_narrow(mix);
    __m256i f = _mm256_sub_epi32(mix, _mm256_sub_epi32(_mm256_slli_epi32(i, 8), i));
    __m256i lohi = render_avx2_lin_pair(i, w);
    __m256i lo = _mm256_and_si256(lohi, RENDER_V8(0xFFFF));
    __m256i hi = _mm256_srli_epi32(lohi, 16);
    __m256i blend_l = _mm256_add_epi32(
        lo, render_avx2_div255_wide(_mm256_mullo_epi32(_mm256_sub_epi32(hi, lo), f)));
    __m256i diff = _mm256_sub_epi32(lfg_s, lbg_s);
    __m256i e = _mm256_sub_epi32(blend_l, lbg_s);
    __m256i half = _mm256_srai_epi32(_mm256_add_epi32(diff, _mm256_srli_epi32(diff, 31)), 1);
    __m256i num = _mm256_add_epi32(_mm256_sub_epi32(_mm256_slli_epi32(e, 8), e), half);
    __m256i flat = _mm256_cmpeq_epi32(diff, _mm256_setzero_si256());
    __m256i den = _mm256_or_si256(diff, _mm256_and_si256(flat, RENDER_V8(1)));
    __m128i q0 = _mm256_cvttpd_epi32(_mm256_div_pd(
        _mm256_cvtepi32_pd(_mm256_castsi256_si128(num)),
        _mm256_cvtepi32_pd(_mm256_castsi256_si128(den))));
    __m256i c;
    if (w <= 4) {
        c = _mm256_zextsi128_si256(q0);
    } else {
        __m128i q1 = _mm256_cvttpd_epi32(_mm256_div_pd(
            _mm256_cvtepi32_pd(_mm256_extracti128_si256(num, 1)),
            _mm256_cvtepi32_pd(_mm256_extracti128_si256(den, 1))));
        c = _mm256_set_m128i(q1, q0);
    }
    c = _mm256_min_epi32(_mm256_max_epi32(c, _mm256_setzero_si256()), RENDER_V8(255));
    return _mm256_blendv_epi8(c, a, flat);
}

/* The fg terms of blend_px(), and the bg terms of the last uniform bg. */
struct render_avx2_blend {
    __m256i fg, lfg_r, lfg_g, lfg_b, fg_l, fg_s, lfg_s;
    __m256i lr, lg, lb, bg_s, lbg_s;
    uint32_t bg;
    int fg_lum;
    bool have_bg, bg_far;
};

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE void render_avx2_blend_init(struct render_avx2_blend *b,
                                                               uint32_t fg, int fg_l)
{
    int fg_s = srgb_from_linear[fg_l];
    b->fg = RENDER_V8(fg);
    b->lfg_r = RENDER_V8(srgb_to_linear[(fg >> 16) & 255]);
    b->lfg_g = RENDER_V8(srgb_to_linear[(fg >> 8) & 255]);
    b->lfg_b = RENDER_V8(srgb_to_linear[fg & 255]);
    b->fg_l = RENDER_V8(fg_l);
    b->fg_s = RENDER_V8(fg_s);
    b->lfg_s = RENDER_V8(srgb_to_linear[fg_s]);
    b->fg_lum = fg_l;
    b->have_bg = false;
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE void render_avx2_blend_set_bg(struct render_avx2_blend *b,
                                                                 uint32_t bg)
{
    int bg_l = (int)blend_lum16(bg);
    int bg_s = srgb_from_linear[bg_l];
    int diff = b->fg_lum - bg_l;
    b->lr = RENDER_V8(srgb_to_linear[(bg >> 16) & 255]);
    b->lg = RENDER_V8(srgb_to_linear[(bg >> 8) & 255]);
    b->lb = RENDER_V8(srgb_to_linear[bg & 255]);
    b->bg_s = RENDER_V8(bg_s);
    b->lbg_s = RENDER_V8(srgb_to_linear[bg_s]);
    b->bg = bg;
    b->bg_far = diff <= -66 || diff >= 66;
    b->have_bg = true;
}

RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE void render_avx2_blend_clear(struct render_avx2_blend *b)
{
    const __m256i zero = _mm256_setzero_si256();
    b->lr = b->lg = b->lb = b->bg_s = b->lbg_s = zero;
    b->bg = 0;
    b->bg_far = false;
    b->have_bg = false;
}

/*
 * blend_px(fg, p, a) in the front w lanes. The other lanes hold p = 0 and
 * a = 0, and their output is not used. For an opaque bg,
 * a + (255 * (255 - a) + 127) / 255 is 255 for every a, so the output alpha
 * needs no opaque case.
 */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_blend_lanes(struct render_avx2_blend *b,
                                                                   __m256i p, __m256i a, int w)
{
    const __m256i c255 = RENDER_V8(255);
    const __m256i zero = _mm256_setzero_si256();
    __m256i front = render_avx2_tail_mask(w);
    __m256i back = _mm256_xor_si256(front, RENDER_V8(-1));
    bool uniform = b->have_bg &&
        render_avx2_all(_mm256_or_si256(_mm256_cmpeq_epi32(p, RENDER_V8(b->bg)), back));
    if (!uniform) {
        uint32_t p0 = (uint32_t)_mm256_cvtsi256_si32(p);
        if (render_avx2_all(_mm256_or_si256(_mm256_cmpeq_epi32(p, RENDER_V8(p0)), back))) {
            render_avx2_blend_set_bg(b, p0);
            uniform = true;
        }
    }
    __m256i edge = _mm256_or_si256(_mm256_cmpeq_epi32(a, zero), _mm256_cmpeq_epi32(a, c255));
    __m256i mid = _mm256_andnot_si256(edge, front);
    __m256i lr, lg, lb, need, bg_s = zero, lbg_s = zero;
    if (uniform) {
        lr = b->lr;
        lg = b->lg;
        lb = b->lb;
        bg_s = b->bg_s;
        lbg_s = b->lbg_s;
        need = b->bg_far ? mid : zero;
    } else {
        lr = render_avx2_lin(_mm256_and_si256(_mm256_srli_epi32(p, 16), c255), w);
        lg = render_avx2_lin(_mm256_and_si256(_mm256_srli_epi32(p, 8), c255), w);
        lb = render_avx2_lin(_mm256_and_si256(p, c255), w);
        __m256i bg_l = render_avx2_lum16(lr, lg, lb);
        need = _mm256_and_si256(mid, _mm256_cmpgt_epi32(
            _mm256_abs_epi32(_mm256_sub_epi32(b->fg_l, bg_l)), RENDER_V8(65)));
        if (_mm256_movemask_epi8(need)) {
            bg_s = render_avx2_enc(bg_l, w);
            lbg_s = render_avx2_lin(bg_s, w);
        }
    }
    if (_mm256_movemask_epi8(need))
        a = _mm256_blendv_epi8(a, render_avx2_coverage(b->fg_s, b->lfg_s, bg_s, lbg_s, a, w),
                               need);
    __m256i m0 = _mm256_cmpeq_epi32(a, zero);
    __m256i m255 = _mm256_cmpeq_epi32(a, c255);
    __m256i ia = _mm256_sub_epi32(c255, a);
    __m256i r = render_avx2_channel(b->lfg_r, lr, a, ia, w);
    __m256i g = render_avx2_channel(b->lfg_g, lg, a, ia, w);
    __m256i bl = render_avx2_channel(b->lfg_b, lb, a, ia, w);
    __m256i bg_a = _mm256_srli_epi32(p, 24);
    __m256i oa = _mm256_add_epi32(a, render_avx2_div255_narrow(
        _mm256_add_epi32(_mm256_madd_epi16(bg_a, ia), RENDER_V8(127))));
    __m256i res = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(oa, 24),
                                                  _mm256_slli_epi32(r, 16)),
                                  _mm256_or_si256(_mm256_slli_epi32(g, 8), bl));
    res = _mm256_blendv_epi8(res, p, m0);
    return _mm256_blendv_epi8(res, b->fg, m255);
}

/* One channel of render_blend_premul_px(). */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_premul_channel(
    __m256i c, __m256i bgc, __m256i half, __m256 af, __m256i cov, __m256i ia, int w)
{
    __m256i x = _mm256_add_epi32(_mm256_sub_epi32(_mm256_slli_epi32(c, 8), c), half);
    __m256i q = _mm256_cvttps_epi32(_mm256_div_ps(_mm256_cvtepi32_ps(x), af));
    q = _mm256_min_epi32(q, RENDER_V8(255));
    return render_avx2_channel(render_avx2_lin(q, w), render_avx2_lin(bgc, w), cov, ia, w);
}

/* render_blend_premul_px() in the front w lanes, for lanes with alpha above 0. */
RENDER_TARGET_AVX2
static inline RENDER_ALWAYS_INLINE __m256i render_avx2_premul_lanes(__m256i p, __m256i bgp,
                                                                    bool faint, int w)
{
    const __m256i c255 = RENDER_V8(255);
    __m256i a = _mm256_srli_epi32(p, 24);
    __m256i cov = faint ? _mm256_srli_epi32(_mm256_add_epi32(a, RENDER_V8(1)), 1) : a;
    __m256i ia = _mm256_sub_epi32(c255, cov);
    __m256 af = _mm256_cvtepi32_ps(_mm256_max_epi32(a, RENDER_V8(1)));
    __m256i half = _mm256_srli_epi32(a, 1);
    __m256i r = render_avx2_premul_channel(
        _mm256_and_si256(_mm256_srli_epi32(p, 16), c255),
        _mm256_and_si256(_mm256_srli_epi32(bgp, 16), c255), half, af, cov, ia, w);
    __m256i g = render_avx2_premul_channel(
        _mm256_and_si256(_mm256_srli_epi32(p, 8), c255),
        _mm256_and_si256(_mm256_srli_epi32(bgp, 8), c255), half, af, cov, ia, w);
    __m256i b = render_avx2_premul_channel(_mm256_and_si256(p, c255),
                                           _mm256_and_si256(bgp, c255), half, af, cov, ia, w);
    return _mm256_or_si256(_mm256_or_si256(RENDER_V8(0xFF000000u), _mm256_slli_epi32(r, 16)),
                           _mm256_or_si256(_mm256_slli_epi32(g, 8), b));
}

RENDER_TARGET_AVX2
static inline void render_fill_avx2(uint32_t *dst, int n, uint32_t color)
{
    if (n < 8) {
        if (n >= 4) {
            __m128i v = RENDER_V4(color);
            _mm_storeu_si128((__m128i *)dst, v);
            _mm_storeu_si128((__m128i *)(dst + n - 4), v);
        } else if (n >= 2) {
            uint64_t v = (uint64_t)color << 32 | color;
            memcpy(dst, &v, 8);
            memcpy(dst + n - 2, &v, 8);
        } else if (n == 1) {
            dst[0] = color;
        }
        return;
    }
    __m256i v = RENDER_V8(color);
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        _mm256_storeu_si256((__m256i *)(dst + i), v);
        _mm256_storeu_si256((__m256i *)(dst + i + 8), v);
        _mm256_storeu_si256((__m256i *)(dst + i + 16), v);
        _mm256_storeu_si256((__m256i *)(dst + i + 24), v);
    }
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_si256((__m256i *)(dst + i), v);
    if (i < n)
        _mm256_storeu_si256((__m256i *)(dst + n - 8), v);
}

RENDER_TARGET_AVX2
static inline void render_blit_tile_avx2(uint32_t *dst, size_t dst_stride_px,
                                         const uint32_t *tile, int cw, int ch)
{
    if (cw <= 0 || ch <= 0)
        return;
    if (cw == 8) {
        for (int y = 0; y < ch; y++)
            _mm256_storeu_si256((__m256i *)(dst + (size_t)y * dst_stride_px),
                                _mm256_loadu_si256((const __m256i *)(tile + (size_t)y * 8)));
        return;
    }
    if (cw > 8) {
        for (int y = 0; y < ch; y++) {
            const uint32_t *s = tile + (size_t)y * (size_t)cw;
            uint32_t *d = dst + (size_t)y * dst_stride_px;
            int x = 0;
            for (; x + 8 <= cw; x += 8)
                _mm256_storeu_si256((__m256i *)(d + x),
                                    _mm256_loadu_si256((const __m256i *)(s + x)));
            if (x < cw)
                _mm256_storeu_si256((__m256i *)(d + cw - 8),
                                    _mm256_loadu_si256((const __m256i *)(s + cw - 8)));
        }
        return;
    }
    if (cw >= 4) {
        for (int y = 0; y < ch; y++) {
            const uint32_t *s = tile + (size_t)y * (size_t)cw;
            uint32_t *d = dst + (size_t)y * dst_stride_px;
            __m128i a = _mm_loadu_si128((const __m128i *)s);
            __m128i b = _mm_loadu_si128((const __m128i *)(s + cw - 4));
            _mm_storeu_si128((__m128i *)d, a);
            _mm_storeu_si128((__m128i *)(d + cw - 4), b);
        }
        return;
    }
    render_blit_tile_scalar(dst, dst_stride_px, tile, cw, ch);
}

RENDER_TARGET_AVX2
static inline void render_compose_alpha_lut_avx2(uint32_t *dst, const uint8_t *cov, int n,
                                                 const uint32_t *lut256)
{
    render_lut_rows(dst, cov, n, lut256);
}

RENDER_TARGET_AVX2
static inline void render_compose_subpx_lut_avx2(uint32_t *dst, const uint8_t *cov, int n,
                                                 const struct subpx_lut *lut, uint32_t bg)
{
    render_compose_subpx_lut_scalar(dst, cov, n, lut, bg);
}

RENDER_TARGET_AVX2
static inline void render_compose_color_avx2(uint32_t *dst, const uint32_t *premul, int n,
                                             bool faint)
{
    if (n <= 0)
        return;
    srgb_ensure_luts();
    const __m256i alpha = RENDER_V8(0xFF000000u);
    const __m256i zero = _mm256_setzero_si256();
    for (int i = 0; i < n; i += 8) {
        int r = n - i < 8 ? n - i : 8;
        __m256i p = r == 8 ? _mm256_loadu_si256((const __m256i *)(premul + i))
                           : render_avx2_load_part(premul + i, r);
        __m256i live = _mm256_xor_si256(_mm256_cmpeq_epi32(_mm256_and_si256(p, alpha), zero),
                                        RENDER_V8(-1));
        unsigned m = render_avx2_bits(live);
        if (!m)
            continue;
        __m256i bgp = r == 8 ? _mm256_loadu_si256((const __m256i *)(dst + i))
                             : render_avx2_load_part(dst + i, r);
        int w = __builtin_popcount(m);
        __m256i front = render_avx2_tail_mask(w);
        __m256i rc = render_avx2_premul_lanes(render_avx2_pack_lanes(p, m, front),
                                              render_avx2_pack_lanes(bgp, m, front), faint, w);
        __m256i res = render_avx2_unpack_lanes(bgp, rc, m, live);
        if (r == 8)
            _mm256_storeu_si256((__m256i *)(dst + i), res);
        else
            render_avx2_store_part(dst + i, res, r);
    }
}

RENDER_TARGET_AVX2
static inline void render_blend_any_avx2(uint32_t *dst, const uint8_t *cov, int n, uint32_t fg)
{
    if (n <= 0)
        return;
    if (n <= 8) {
        render_blend_any_scalar(dst, cov, n, fg);
        return;
    }
    srgb_ensure_luts();
    int fg_l = (int)blend_lum16(fg);
    struct render_avx2_blend b;
    render_avx2_blend_clear(&b);
    bool ready = false;
    struct render_bg_lum lum = {0};
    const __m256i zero = _mm256_setzero_si256();
    const __m256i fgv = RENDER_V8(fg);
    for (int i = 0; i < n; i += 8) {
        int r = n - i < 8 ? n - i : 8;
        uint64_t bytes;
        if (r == 8)
            memcpy(&bytes, cov + i, 8);
        else
            bytes = render_load_bytes(cov + i, r);
        if (!bytes)
            continue;
        __m256i a = render_avx2_bytes(bytes);
        __m256i full = _mm256_cmpeq_epi32(a, RENDER_V8(255));
        __m256i midv = _mm256_xor_si256(_mm256_or_si256(full, _mm256_cmpeq_epi32(a, zero)),
                                        RENDER_V8(-1));
        unsigned m = render_avx2_bits(midv);
        unsigned f = render_avx2_bits(full);
        int w = __builtin_popcount(m);
        __m256i p = zero;
        bool vector = w > RENDER_SCALAR_LANES;
        if (vector) {
            /* The vector form gathers for every lane. Use it when the lanes share a bg. */
            p = r == 8 ? _mm256_loadu_si256((const __m256i *)(dst + i))
                       : render_avx2_load_part(dst + i, r);
            __m256i first = _mm256_permutevar8x32_epi32(p, RENDER_V8(__builtin_ctz(m)));
            vector = render_avx2_all(_mm256_or_si256(
                _mm256_cmpeq_epi32(p, first), _mm256_xor_si256(midv, RENDER_V8(-1))));
        }
        if (!vector) {
            for (; m; m &= m - 1) {
                int j = i + __builtin_ctz(m);
                dst[j] = render_blend_px_cached(fg, fg_l, dst[j], cov[j], &lum);
            }
            for (; f; f &= f - 1)
                dst[i + __builtin_ctz(f)] = fg;
            continue;
        }
        if (!ready) {
            render_avx2_blend_init(&b, fg, fg_l);
            ready = true;
        }
        __m256i front = render_avx2_tail_mask(w);
        __m256i rc = render_avx2_blend_lanes(&b, render_avx2_pack_lanes(p, m, front),
                                             render_avx2_pack_lanes(a, m, front), w);
        __m256i res = render_avx2_unpack_lanes(_mm256_blendv_epi8(p, fgv, full), rc, m, midv);
        if (r == 8)
            _mm256_storeu_si256((__m256i *)(dst + i), res);
        else
            render_avx2_store_part(dst + i, res, r);
    }
}

RENDER_TARGET_AVX2
static inline void render_blend_any_subpx_avx2(uint32_t *dst, const uint8_t *cov, int n,
                                               uint32_t fg)
{
    render_blend_any_subpx_scalar(dst, cov, n, fg);
}

RENDER_TARGET_AVX2
static inline void render_blend_rect_avx2(uint32_t *dst, int n, uint32_t color, uint8_t a)
{
    if (n <= 0 || !a)
        return;
    if (a == 255) {
        render_fill_avx2(dst, n, color);
        return;
    }
    srgb_ensure_luts();
    int fg_l = (int)blend_lum16(color);
    struct render_avx2_blend b;
    render_avx2_blend_clear(&b);
    bool ready = false;
    const __m256i av = RENDER_V8(a);
    uint32_t last_bg = 0, last = 0;
    bool have_last = false;
    for (int i = 0; i < n; i += 8) {
        int r = n - i < 8 ? n - i : 8;
        __m256i back = _mm256_xor_si256(render_avx2_tail_mask(r), RENDER_V8(-1));
        __m256i p = r == 8 ? _mm256_loadu_si256((const __m256i *)(dst + i))
                           : render_avx2_load_part(dst + i, r);
        uint32_t p0 = (uint32_t)_mm256_cvtsi256_si32(p);
        __m256i res;
        if (render_avx2_all(_mm256_or_si256(_mm256_cmpeq_epi32(p, RENDER_V8(p0)), back))) {
            if (!have_last || p0 != last_bg) {
                last_bg = p0;
                last = blend_px_lum(color, p0, fg_l, (int)blend_lum16(p0), a);
                have_last = true;
            }
            res = RENDER_V8(last);
        } else {
            if (!ready) {
                render_avx2_blend_init(&b, color, fg_l);
                ready = true;
            }
            res = render_avx2_blend_lanes(&b, p, _mm256_andnot_si256(back, av), r);
        }
        if (r == 8)
            _mm256_storeu_si256((__m256i *)(dst + i), res);
        else
            render_avx2_store_part(dst + i, res, r);
    }
}

/* ---- x86-64: AVX-512 ------------------------------------------------- */

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __mmask16 render_avx512_mask(int r)
{
    return (__mmask16)_bzhi_u32(0xFFFFu, (unsigned)r);
}

/* Sixteen RGB triples as r | g << 8 | b << 16 per lane. m selects the bytes to load. */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_rgb16(const uint8_t *p, __mmask64 m)
{
    __m512i raw = _mm512_maskz_loadu_epi8(m, p);
    const __m512i perm = _mm512_setr_epi32(0, 1, 2, 0, 3, 4, 5, 0, 6, 7, 8, 0, 9, 10, 11, 0);
    const __m512i shuf = _mm512_broadcast_i32x4(_mm_setr_epi8(
        0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1));
    return _mm512_shuffle_epi8(_mm512_permutexvar_epi32(perm, raw), shuf);
}

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __mmask64 render_avx512_rgb_mask(int r)
{
    return (__mmask64)_bzhi_u64(~0ull, (unsigned)r * 3);
}

/* srgb_to_linear[0..255] as 8 registers of 32 words. */
struct render_avx512_lin {
    __m512i t[8];
};

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE void render_avx512_lin_load(struct render_avx512_lin *l)
{
    for (int k = 0; k < 8; k++)
        l->t[k] = _mm512_loadu_si512(srgb_to_linear + 32 * k);
}

/* srgb_to_linear[x & 255] for each 16-bit lane x. */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_lin16(const struct render_avx512_lin *l,
                                                               __m512i x)
{
    __m512i q0 = _mm512_permutex2var_epi16(l->t[0], x, l->t[1]);
    __m512i q1 = _mm512_permutex2var_epi16(l->t[2], x, l->t[3]);
    __m512i q2 = _mm512_permutex2var_epi16(l->t[4], x, l->t[5]);
    __m512i q3 = _mm512_permutex2var_epi16(l->t[6], x, l->t[7]);
    __mmask32 b6 = _mm512_test_epi16_mask(x, _mm512_set1_epi16(64));
    __mmask32 b7 = _mm512_test_epi16_mask(x, _mm512_set1_epi16(128));
    return _mm512_mask_blend_epi16(b7, _mm512_mask_blend_epi16(b6, q0, q1),
                                   _mm512_mask_blend_epi16(b6, q2, q3));
}

/* srgb_to_linear[x] for each 32-bit lane x < 256. */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_lin(const struct render_avx512_lin *l,
                                                             __m512i x)
{
    return _mm512_and_si512(render_avx512_lin16(l, x), RENDER_V16(0xFFFF));
}

/* srgb_from_linear[idx] for the front w lanes. With w <= 8, the other lanes are 0. */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_enc(__m512i idx, int w)
{
    const int *base = (const int *)(const void *)srgb_from_linear;
    if (w <= 4)
        return _mm512_zextsi128_si512(_mm_and_si128(
            _mm_i32gather_epi32(base, _mm512_castsi512_si128(idx), 1), RENDER_V4(255)));
    if (w <= 8)
        return _mm512_zextsi256_si512(_mm256_and_si256(
            _mm256_i32gather_epi32(base, _mm512_castsi512_si256(idx), 1), RENDER_V8(255)));
    return _mm512_and_si512(_mm512_i32gather_epi32(idx, base, 1), RENDER_V16(255));
}

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_div255_wide(__m512i t)
{
    __m512i h = _mm512_srli_epi32(t, 8);
    __m512i z = _mm512_add_epi32(_mm512_add_epi32(h, _mm512_and_si512(t, RENDER_V16(255))),
                                 RENDER_V16(1));
    return _mm512_add_epi32(h, _mm512_srli_epi32(_mm512_add_epi32(z, _mm512_slli_epi32(z, 8)),
                                                 16));
}

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_div255_narrow(__m512i x)
{
    return _mm512_srli_epi32(_mm512_add_epi32(_mm512_add_epi32(x, RENDER_V16(1)),
                                              _mm512_srli_epi32(x, 8)), 8);
}

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_lum16(__m512i lr, __m512i lg, __m512i lb)
{
    __m512i n = _mm512_add_epi32(
        _mm512_add_epi32(_mm512_mullo_epi32(lr, RENDER_V16(2126)),
                         _mm512_mullo_epi32(lg, RENDER_V16(7152))),
        _mm512_add_epi32(_mm512_mullo_epi32(lb, RENDER_V16(722)), RENDER_V16(5000)));
    const __m512i m = _mm512_set1_epi64(3518437209);
    __m512i even = _mm512_srli_epi64(_mm512_mul_epu32(n, m), 45);
    __m512i odd = _mm512_srli_epi64(_mm512_mul_epu32(_mm512_srli_epi64(n, 32), m), 13);
    return _mm512_mask_blend_epi32((__mmask16)0xAAAA, even, odd);
}

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_channel(__m512i lfg, __m512i lbg,
                                                                 __m512i a, __m512i ia, int w)
{
    __m512i t = _mm512_add_epi32(_mm512_add_epi32(_mm512_mullo_epi32(lfg, a),
                                                  _mm512_mullo_epi32(lbg, ia)),
                                 RENDER_V16(127));
    return render_avx512_enc(render_avx512_div255_wide(t), w);
}

/* render_srgb_coverage_lanes() for the front w lanes. */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_coverage(
    const struct render_avx512_lin *l, __m512i fg_s, __m512i lfg_s, __m512i bg_s, __m512i lbg_s,
    __m512i a, int w)
{
    __m512i ia = _mm512_sub_epi32(RENDER_V16(255), a);
    __m512i mix = _mm512_madd_epi16(_mm512_or_si512(fg_s, _mm512_slli_epi32(bg_s, 16)),
                                    _mm512_or_si512(a, _mm512_slli_epi32(ia, 16)));
    __m512i i = render_avx512_div255_narrow(mix);
    __m512i f = _mm512_sub_epi32(mix, _mm512_sub_epi32(_mm512_slli_epi32(i, 8), i));
    /* Index i + 1 = 256 reads entry 0. Both entries are 0, and f is 0 there. */
    __m512i lohi = render_avx512_lin16(
        l, _mm512_or_si512(i, _mm512_slli_epi32(_mm512_add_epi32(i, RENDER_V16(1)), 16)));
    __m512i lo = _mm512_and_si512(lohi, RENDER_V16(0xFFFF));
    __m512i hi = _mm512_srli_epi32(lohi, 16);
    __m512i blend_l = _mm512_add_epi32(
        lo, render_avx512_div255_wide(_mm512_mullo_epi32(_mm512_sub_epi32(hi, lo), f)));
    __m512i diff = _mm512_sub_epi32(lfg_s, lbg_s);
    __m512i e = _mm512_sub_epi32(blend_l, lbg_s);
    __m512i half = _mm512_srai_epi32(_mm512_add_epi32(diff, _mm512_srli_epi32(diff, 31)), 1);
    __m512i num = _mm512_add_epi32(_mm512_sub_epi32(_mm512_slli_epi32(e, 8), e), half);
    __mmask16 flat = _mm512_cmpeq_epi32_mask(diff, _mm512_setzero_si512());
    __m512i den = _mm512_mask_mov_epi32(diff, flat, RENDER_V16(1));
    __m256i q0 = _mm512_cvttpd_epi32(_mm512_div_pd(
        _mm512_cvtepi32_pd(_mm512_castsi512_si256(num)),
        _mm512_cvtepi32_pd(_mm512_castsi512_si256(den))));
    __m512i c;
    if (w <= 8) {
        c = _mm512_zextsi256_si512(q0);
    } else {
        __m256i q1 = _mm512_cvttpd_epi32(_mm512_div_pd(
            _mm512_cvtepi32_pd(_mm512_extracti64x4_epi64(num, 1)),
            _mm512_cvtepi32_pd(_mm512_extracti64x4_epi64(den, 1))));
        c = _mm512_inserti64x4(_mm512_castsi256_si512(q0), q1, 1);
    }
    c = _mm512_min_epi32(_mm512_max_epi32(c, _mm512_setzero_si512()), RENDER_V16(255));
    return _mm512_mask_mov_epi32(c, flat, a);
}

/* The fg terms of blend_px(), and the bg terms of the last uniform bg. */
struct render_avx512_blend {
    struct render_avx512_lin lin;
    __m512i fg, lfg_r, lfg_g, lfg_b, fg_l, fg_s, lfg_s;
    __m512i lr, lg, lb, bg_s, lbg_s;
    uint32_t bg;
    int fg_lum;
    bool have_bg, bg_far;
};

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE void render_avx512_blend_init(struct render_avx512_blend *b,
                                                                 uint32_t fg, int fg_l)
{
    int fg_s = srgb_from_linear[fg_l];
    render_avx512_lin_load(&b->lin);
    b->fg = RENDER_V16(fg);
    b->lfg_r = RENDER_V16(srgb_to_linear[(fg >> 16) & 255]);
    b->lfg_g = RENDER_V16(srgb_to_linear[(fg >> 8) & 255]);
    b->lfg_b = RENDER_V16(srgb_to_linear[fg & 255]);
    b->fg_l = RENDER_V16(fg_l);
    b->fg_s = RENDER_V16(fg_s);
    b->lfg_s = RENDER_V16(srgb_to_linear[fg_s]);
    b->fg_lum = fg_l;
    b->have_bg = false;
}

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE void render_avx512_blend_set_bg(struct render_avx512_blend *b,
                                                                   uint32_t bg)
{
    int bg_l = (int)blend_lum16(bg);
    int bg_s = srgb_from_linear[bg_l];
    int diff = b->fg_lum - bg_l;
    b->lr = RENDER_V16(srgb_to_linear[(bg >> 16) & 255]);
    b->lg = RENDER_V16(srgb_to_linear[(bg >> 8) & 255]);
    b->lb = RENDER_V16(srgb_to_linear[bg & 255]);
    b->bg_s = RENDER_V16(bg_s);
    b->lbg_s = RENDER_V16(srgb_to_linear[bg_s]);
    b->bg = bg;
    b->bg_far = diff <= -66 || diff >= 66;
    b->have_bg = true;
}

/* blend_px(fg, p, a) in the front w lanes. See render_avx2_blend_lanes(). */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_blend_lanes(
    struct render_avx512_blend *b, __m512i p, __m512i a, int w)
{
    const __m512i c255 = RENDER_V16(255);
    const __m512i zero = _mm512_setzero_si512();
    __mmask16 front = render_avx512_mask(w);
    bool uniform = b->have_bg && !_mm512_mask_cmpneq_epi32_mask(front, p, RENDER_V16(b->bg));
    if (!uniform) {
        uint32_t p0 = (uint32_t)_mm_cvtsi128_si32(_mm512_castsi512_si128(p));
        if (!_mm512_mask_cmpneq_epi32_mask(front, p, RENDER_V16(p0))) {
            render_avx512_blend_set_bg(b, p0);
            uniform = true;
        }
    }
    __mmask16 mid = (__mmask16)(front & _mm512_test_epi32_mask(a, a) &
                                ~_mm512_cmpeq_epi32_mask(a, c255));
    __m512i lr, lg, lb, bg_s = zero, lbg_s = zero;
    __mmask16 need;
    if (uniform) {
        lr = b->lr;
        lg = b->lg;
        lb = b->lb;
        bg_s = b->bg_s;
        lbg_s = b->lbg_s;
        need = b->bg_far ? mid : 0;
    } else {
        __m512i rg = render_avx512_lin16(&b->lin, _mm512_or_si512(
            _mm512_and_si512(_mm512_srli_epi32(p, 16), c255),
            _mm512_and_si512(_mm512_slli_epi32(p, 8), RENDER_V16(0xFF0000))));
        lr = _mm512_and_si512(rg, RENDER_V16(0xFFFF));
        lg = _mm512_srli_epi32(rg, 16);
        lb = render_avx512_lin(&b->lin, _mm512_and_si512(p, c255));
        __m512i bg_l = render_avx512_lum16(lr, lg, lb);
        need = (__mmask16)(mid & _mm512_cmpgt_epi32_mask(
            _mm512_abs_epi32(_mm512_sub_epi32(b->fg_l, bg_l)), RENDER_V16(65)));
        if (need) {
            bg_s = render_avx512_enc(bg_l, w);
            lbg_s = render_avx512_lin(&b->lin, bg_s);
        }
    }
    if (need)
        a = _mm512_mask_mov_epi32(a, need, render_avx512_coverage(
            &b->lin, b->fg_s, b->lfg_s, bg_s, lbg_s, a, w));
    __mmask16 m0 = _mm512_testn_epi32_mask(a, a);
    __mmask16 m255 = _mm512_cmpeq_epi32_mask(a, c255);
    __m512i ia = _mm512_sub_epi32(c255, a);
    __m512i r = render_avx512_channel(b->lfg_r, lr, a, ia, w);
    __m512i g = render_avx512_channel(b->lfg_g, lg, a, ia, w);
    __m512i bl = render_avx512_channel(b->lfg_b, lb, a, ia, w);
    __m512i bg_a = _mm512_srli_epi32(p, 24);
    __m512i oa = _mm512_add_epi32(a, render_avx512_div255_narrow(
        _mm512_add_epi32(_mm512_madd_epi16(bg_a, ia), RENDER_V16(127))));
    __m512i res = _mm512_or_si512(_mm512_or_si512(_mm512_slli_epi32(oa, 24),
                                                  _mm512_slli_epi32(r, 16)),
                                  _mm512_or_si512(_mm512_slli_epi32(g, 8), bl));
    res = _mm512_mask_mov_epi32(res, m0, p);
    return _mm512_mask_mov_epi32(res, m255, b->fg);
}

RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_subpx_channel(
    const struct render_avx512_lin *l, __m512i fgc, __m512i lfg, __m512i bgc, __m512i lbg,
    __m512i cov, int w)
{
    const __m512i c255 = RENDER_V16(255);
    __mmask16 mid = (__mmask16)(_mm512_test_epi32_mask(cov, cov) &
                                ~_mm512_cmpeq_epi32_mask(cov, c255));
    __mmask16 need = (__mmask16)(mid & _mm512_cmpgt_epi32_mask(
        _mm512_abs_epi32(_mm512_sub_epi32(lfg, lbg)), RENDER_V16(65)));
    __m512i a = cov;
    if (need)
        a = _mm512_mask_mov_epi32(cov, need, render_avx512_coverage(l, fgc, lfg, bgc, lbg, cov, w));
    return render_avx512_channel(lfg, lbg, a, _mm512_sub_epi32(c255, a), w);
}

struct render_avx512_subpx_ink {
    struct render_avx512_lin lin;
    __m512i fg_r, fg_g, fg_b, lfg_r, lfg_g, lfg_b;
};

/*
 * blend_subpx(fg, p, r, g, b) in the front w lanes, for lanes with some
 * coverage. v holds r | g << 8 | b << 16. See render_avx2_blend_lanes() for
 * the output alpha.
 */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_subpx_lanes(
    const struct render_avx512_subpx_ink *k, __m512i p, __m512i v, int w)
{
    const __m512i c255 = RENDER_V16(255);
    __m512i cr = _mm512_and_si512(v, c255);
    __m512i cg = _mm512_and_si512(_mm512_srli_epi32(v, 8), c255);
    __m512i cb = _mm512_srli_epi32(v, 16);
    __m512i br = _mm512_and_si512(_mm512_srli_epi32(p, 16), c255);
    __m512i bgg = _mm512_and_si512(_mm512_srli_epi32(p, 8), c255);
    __m512i bb = _mm512_and_si512(p, c255);
    __m512i lrg = render_avx512_lin16(&k->lin, _mm512_or_si512(br, _mm512_slli_epi32(bgg, 16)));
    __m512i r = render_avx512_subpx_channel(&k->lin, k->fg_r, k->lfg_r, br,
        _mm512_and_si512(lrg, RENDER_V16(0xFFFF)), cr, w);
    __m512i g = render_avx512_subpx_channel(&k->lin, k->fg_g, k->lfg_g, bgg,
        _mm512_srli_epi32(lrg, 16), cg, w);
    __m512i b = render_avx512_subpx_channel(&k->lin, k->fg_b, k->lfg_b, bb,
        render_avx512_lin(&k->lin, bb), cb, w);
    __m512i max_a = _mm512_max_epi32(cr, _mm512_max_epi32(cg, cb));
    __m512i bg_a = _mm512_srli_epi32(p, 24);
    __m512i oa = _mm512_add_epi32(max_a, render_avx512_div255_narrow(_mm512_add_epi32(
        _mm512_madd_epi16(bg_a, _mm512_sub_epi32(c255, max_a)), RENDER_V16(127))));
    return _mm512_or_si512(_mm512_or_si512(_mm512_slli_epi32(oa, 24), _mm512_slli_epi32(r, 16)),
                           _mm512_or_si512(_mm512_slli_epi32(g, 8), b));
}

/* render_blend_premul_px() in the front w lanes, for lanes with alpha above 0. */
RENDER_TARGET_AVX512
static inline RENDER_ALWAYS_INLINE __m512i render_avx512_premul_lanes(
    const struct render_avx512_lin *l, __m512i p, __m512i bgp, bool faint, int w)
{
    const __m512i c255 = RENDER_V16(255);
    __m512i a = _mm512_srli_epi32(p, 24);
    __m512i cov = faint ? _mm512_srli_epi32(_mm512_add_epi32(a, RENDER_V16(1)), 1) : a;
    __m512i ia = _mm512_sub_epi32(c255, cov);
    __m512 af = _mm512_cvtepi32_ps(_mm512_max_epi32(a, RENDER_V16(1)));
    __m512i half = _mm512_srli_epi32(a, 1);
    __m512i q[3];
    for (int k = 0; k < 3; k++) {
        __m512i c = _mm512_and_si512(_mm512_srlv_epi32(p, RENDER_V16(16 - 8 * k)), c255);
        __m512i x = _mm512_add_epi32(_mm512_sub_epi32(_mm512_slli_epi32(c, 8), c), half);
        q[k] = _mm512_min_epi32(_mm512_cvttps_epi32(_mm512_div_ps(_mm512_cvtepi32_ps(x), af)),
                                c255);
    }
    __m512i br = _mm512_and_si512(_mm512_srli_epi32(bgp, 16), c255);
    __m512i bgg = _mm512_and_si512(_mm512_srli_epi32(bgp, 8), c255);
    __m512i bb = _mm512_and_si512(bgp, c255);
    __m512i lq = render_avx512_lin16(l, _mm512_or_si512(q[0], _mm512_slli_epi32(q[1], 16)));
    __m512i lx = render_avx512_lin16(l, _mm512_or_si512(q[2], _mm512_slli_epi32(br, 16)));
    __m512i lgb = render_avx512_lin16(l, _mm512_or_si512(bgg, _mm512_slli_epi32(bb, 16)));
    const __m512i lo16 = RENDER_V16(0xFFFF);
    __m512i r = render_avx512_channel(_mm512_and_si512(lq, lo16), _mm512_srli_epi32(lx, 16),
                                      cov, ia, w);
    __m512i g = render_avx512_channel(_mm512_srli_epi32(lq, 16), _mm512_and_si512(lgb, lo16),
                                      cov, ia, w);
    __m512i b = render_avx512_channel(_mm512_and_si512(lx, lo16), _mm512_srli_epi32(lgb, 16),
                                      cov, ia, w);
    return _mm512_or_si512(_mm512_or_si512(RENDER_V16(0xFF000000u), _mm512_slli_epi32(r, 16)),
                           _mm512_or_si512(_mm512_slli_epi32(g, 8), b));
}

RENDER_TARGET_AVX512
static inline void render_fill_avx512(uint32_t *dst, int n, uint32_t color)
{
    if (n <= 0)
        return;
    __m512i v = RENDER_V16(color);
    int i = 0;
    for (; i + 64 <= n; i += 64) {
        _mm512_storeu_si512(dst + i, v);
        _mm512_storeu_si512(dst + i + 16, v);
        _mm512_storeu_si512(dst + i + 32, v);
        _mm512_storeu_si512(dst + i + 48, v);
    }
    for (; i + 16 <= n; i += 16)
        _mm512_storeu_si512(dst + i, v);
    if (i < n)
        _mm512_mask_storeu_epi32(dst + i, render_avx512_mask(n - i), v);
}

RENDER_TARGET_AVX512
static inline void render_blit_tile_avx512(uint32_t *dst, size_t dst_stride_px,
                                           const uint32_t *tile, int cw, int ch)
{
    if (cw <= 0 || ch <= 0)
        return;
    if (cw <= 8) {
        __mmask8 m = (__mmask8)_bzhi_u32(0xFFu, (unsigned)cw);
        for (int y = 0; y < ch; y++)
            _mm256_mask_storeu_epi32(dst + (size_t)y * dst_stride_px, m,
                _mm256_maskz_loadu_epi32(m, tile + (size_t)y * (size_t)cw));
        return;
    }
    if (cw <= 16) {
        __mmask16 m = render_avx512_mask(cw);
        for (int y = 0; y < ch; y++)
            _mm512_mask_storeu_epi32(dst + (size_t)y * dst_stride_px, m,
                _mm512_maskz_loadu_epi32(m, tile + (size_t)y * (size_t)cw));
        return;
    }
    __mmask16 m = render_avx512_mask(cw & 15);
    for (int y = 0; y < ch; y++) {
        const uint32_t *s = tile + (size_t)y * (size_t)cw;
        uint32_t *d = dst + (size_t)y * dst_stride_px;
        int x = 0;
        for (; x + 16 <= cw; x += 16)
            _mm512_storeu_si512(d + x, _mm512_loadu_si512(s + x));
        if (x < cw)
            _mm512_mask_storeu_epi32(d + x, m, _mm512_maskz_loadu_epi32(m, s + x));
    }
}

RENDER_TARGET_AVX512
static inline void render_compose_alpha_lut_avx512(uint32_t *dst, const uint8_t *cov, int n,
                                                   const uint32_t *lut256)
{
    render_lut_rows(dst, cov, n, lut256);
}

RENDER_TARGET_AVX512
static inline void render_compose_subpx_lut_avx512(uint32_t *dst, const uint8_t *cov, int n,
                                                   const struct subpx_lut *lut, uint32_t bg)
{
    render_compose_subpx_lut_scalar(dst, cov, n, lut, bg);
}

RENDER_TARGET_AVX512
static inline void render_compose_color_avx512(uint32_t *dst, const uint32_t *premul, int n,
                                               bool faint)
{
    if (n <= 0)
        return;
    srgb_ensure_luts();
    struct render_avx512_lin l;
    render_avx512_lin_load(&l);
    const __m512i alpha = RENDER_V16(0xFF000000u);
    for (int i = 0; i < n; i += 16) {
        __mmask16 act = render_avx512_mask(n - i < 16 ? n - i : 16);
        __m512i p = _mm512_maskz_loadu_epi32(act, premul + i);
        __mmask16 live = _mm512_test_epi32_mask(p, alpha);
        if (!live)
            continue;
        __m512i bgp = _mm512_maskz_loadu_epi32(act, dst + i);
        int w = __builtin_popcount(live);
        __m512i rc = render_avx512_premul_lanes(&l, _mm512_maskz_compress_epi32(live, p),
                                                _mm512_maskz_compress_epi32(live, bgp), faint, w);
        _mm512_mask_storeu_epi32(dst + i, act, _mm512_mask_expand_epi32(bgp, live, rc));
    }
}

RENDER_TARGET_AVX512
static inline void render_blend_any_avx512(uint32_t *dst, const uint8_t *cov, int n,
                                           uint32_t fg)
{
    if (n <= 0)
        return;
    if (n <= 8) {
        render_blend_any_scalar(dst, cov, n, fg);
        return;
    }
    srgb_ensure_luts();
    int fg_l = (int)blend_lum16(fg);
    struct render_avx512_blend b;
    b.have_bg = false;
    bool ready = false;
    struct render_bg_lum lum = {0};
    const __m512i c255 = RENDER_V16(255);
    const __m512i fgv = RENDER_V16(fg);
    for (int i = 0; i < n; i += 16) {
        __mmask16 act = render_avx512_mask(n - i < 16 ? n - i : 16);
        __m128i c = _mm_maskz_loadu_epi8(act, cov + i);
        if (_mm_testz_si128(c, c))
            continue;
        __m512i a = _mm512_cvtepu8_epi32(c);
        __mmask16 full = _mm512_cmpeq_epi32_mask(a, c255);
        unsigned mid = (unsigned)(_mm512_test_epi32_mask(a, a) & ~full);
        int w = __builtin_popcount(mid);
        if (w <= RENDER_SCALAR_LANES) {
            /* Few pixels need the formula: the scalar form is faster. */
            for (; mid; mid &= mid - 1) {
                int j = i + __builtin_ctz(mid);
                dst[j] = render_blend_px_cached(fg, fg_l, dst[j], cov[j], &lum);
            }
            if (full)
                _mm512_mask_storeu_epi32(dst + i, full, fgv);
            continue;
        }
        if (!ready) {
            render_avx512_blend_init(&b, fg, fg_l);
            ready = true;
        }
        __m512i p = _mm512_maskz_loadu_epi32(act, dst + i);
        __m512i rc = render_avx512_blend_lanes(&b, _mm512_maskz_compress_epi32((__mmask16)mid, p),
                                               _mm512_maskz_compress_epi32((__mmask16)mid, a), w);
        __m512i res = _mm512_mask_expand_epi32(_mm512_mask_mov_epi32(p, full, fgv),
                                               (__mmask16)mid, rc);
        _mm512_mask_storeu_epi32(dst + i, act, res);
    }
}

RENDER_TARGET_AVX512
static inline void render_blend_any_subpx_avx512(uint32_t *dst, const uint8_t *cov, int n,
                                                 uint32_t fg)
{
    if (n <= 0)
        return;
    if (n <= 8) {
        render_blend_any_subpx_scalar(dst, cov, n, fg);
        return;
    }
    srgb_ensure_luts();
    unsigned fr = (fg >> 16) & 255, fgg = (fg >> 8) & 255, fb = fg & 255;
    struct render_avx512_subpx_ink k = {
        .fg_r = RENDER_V16(fr), .fg_g = RENDER_V16(fgg), .fg_b = RENDER_V16(fb),
        .lfg_r = RENDER_V16(srgb_to_linear[fr]),
        .lfg_g = RENDER_V16(srgb_to_linear[fgg]),
        .lfg_b = RENDER_V16(srgb_to_linear[fb]),
    };
    render_avx512_lin_load(&k.lin);
    for (int i = 0; i < n; i += 16) {
        int r = n - i < 16 ? n - i : 16;
        __mmask16 act = render_avx512_mask(r);
        __m512i v = render_avx512_rgb16(cov + 3 * i, render_avx512_rgb_mask(r));
        __mmask16 live = _mm512_test_epi32_mask(v, v);
        if (!live)
            continue;
        __m512i p = _mm512_maskz_loadu_epi32(act, dst + i);
        int w = __builtin_popcount(live);
        __m512i rc = render_avx512_subpx_lanes(&k, _mm512_maskz_compress_epi32(live, p),
                                               _mm512_maskz_compress_epi32(live, v), w);
        _mm512_mask_storeu_epi32(dst + i, act, _mm512_mask_expand_epi32(p, live, rc));
    }
}

RENDER_TARGET_AVX512
static inline void render_blend_rect_avx512(uint32_t *dst, int n, uint32_t color, uint8_t a)
{
    if (n <= 8) {
        /* A short row is faster in 8 lanes. */
        render_blend_rect_avx2(dst, n, color, a);
        return;
    }
    if (!a)
        return;
    if (a == 255) {
        render_fill_avx512(dst, n, color);
        return;
    }
    srgb_ensure_luts();
    int fg_l = (int)blend_lum16(color);
    struct render_avx512_blend b;
    b.have_bg = false;
    bool ready = false;
    uint32_t last_bg = 0, last = 0;
    bool have_last = false;
    for (int i = 0; i < n; i += 16) {
        int r = n - i < 16 ? n - i : 16;
        __mmask16 act = render_avx512_mask(r);
        __m512i p = _mm512_maskz_loadu_epi32(act, dst + i);
        uint32_t p0 = (uint32_t)_mm_cvtsi128_si32(_mm512_castsi512_si128(p));
        __m512i res;
        if (!_mm512_mask_cmpneq_epi32_mask(act, p, RENDER_V16(p0))) {
            if (!have_last || p0 != last_bg) {
                last_bg = p0;
                last = blend_px_lum(color, p0, fg_l, (int)blend_lum16(p0), a);
                have_last = true;
            }
            res = RENDER_V16(last);
        } else {
            if (!ready) {
                render_avx512_blend_init(&b, color, fg_l);
                ready = true;
            }
            res = render_avx512_blend_lanes(&b, p, _mm512_maskz_mov_epi32(act, RENDER_V16(a)), r);
        }
        _mm512_mask_storeu_epi32(dst + i, act, res);
    }
}

#endif /* RENDER_KERNEL_X86 */

/* ---- aarch64: NEON --------------------------------------------------- */

#if defined(RENDER_KERNEL_NEON)

static inline void render_fill_neon(uint32_t *dst, int n, uint32_t color)
{
    if (n < 4) {
        for (int i = 0; i < n; i++)
            dst[i] = color;
        return;
    }
    uint32x4_t v = vdupq_n_u32(color);
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        vst1q_u32(dst + i, v);
        vst1q_u32(dst + i + 4, v);
        vst1q_u32(dst + i + 8, v);
        vst1q_u32(dst + i + 12, v);
    }
    for (; i + 4 <= n; i += 4)
        vst1q_u32(dst + i, v);
    if (i < n)
        vst1q_u32(dst + n - 4, v);
}

static inline void render_blit_tile_neon(uint32_t *dst, size_t dst_stride_px,
                                         const uint32_t *tile, int cw, int ch)
{
    if (cw <= 0 || ch <= 0)
        return;
    if (cw < 4) {
        render_blit_tile_scalar(dst, dst_stride_px, tile, cw, ch);
        return;
    }
    for (int y = 0; y < ch; y++) {
        const uint32_t *s = tile + (size_t)y * (size_t)cw;
        uint32_t *d = dst + (size_t)y * dst_stride_px;
        int x = 0;
        for (; x + 8 <= cw; x += 8) {
            vst1q_u32(d + x, vld1q_u32(s + x));
            vst1q_u32(d + x + 4, vld1q_u32(s + x + 4));
        }
        for (; x + 4 <= cw; x += 4)
            vst1q_u32(d + x, vld1q_u32(s + x));
        if (x < cw)
            vst1q_u32(d + cw - 4, vld1q_u32(s + cw - 4));
    }
}

/* The blend kernels have no NEON math yet. They run the scalar reference. */
static inline void render_compose_alpha_lut_neon(uint32_t *dst, const uint8_t *cov, int n,
                                                 const uint32_t *lut256)
{
    render_compose_alpha_lut_scalar(dst, cov, n, lut256);
}

static inline void render_compose_subpx_lut_neon(uint32_t *dst, const uint8_t *cov, int n,
                                                 const struct subpx_lut *lut, uint32_t bg)
{
    render_compose_subpx_lut_scalar(dst, cov, n, lut, bg);
}

static inline void render_compose_color_neon(uint32_t *dst, const uint32_t *premul, int n,
                                             bool faint)
{
    render_compose_color_scalar(dst, premul, n, faint);
}

static inline void render_blend_any_neon(uint32_t *dst, const uint8_t *cov, int n, uint32_t fg)
{
    render_blend_any_scalar(dst, cov, n, fg);
}

static inline void render_blend_any_subpx_neon(uint32_t *dst, const uint8_t *cov, int n,
                                               uint32_t fg)
{
    render_blend_any_subpx_scalar(dst, cov, n, fg);
}

static inline void render_blend_rect_neon(uint32_t *dst, int n, uint32_t color, uint8_t a)
{
    render_blend_rect_scalar(dst, n, color, a);
}

#endif /* RENDER_KERNEL_NEON */

/* ---- Dispatchers ----------------------------------------------------- */

#if defined(RENDER_KERNEL_X86)
#define RENDER_KERNEL_DISPATCH(isa, name, ...)                       \
    do {                                                             \
        if ((isa) == RENDER_ISA_AVX512) {                            \
            render_##name##_avx512(__VA_ARGS__);                     \
            return;                                                  \
        }                                                            \
        if ((isa) == RENDER_ISA_AVX2) {                              \
            render_##name##_avx2(__VA_ARGS__);                       \
            return;                                                  \
        }                                                            \
        render_##name##_scalar(__VA_ARGS__);                         \
    } while (0)
#elif defined(RENDER_KERNEL_NEON)
#define RENDER_KERNEL_DISPATCH(isa, name, ...)                       \
    do {                                                             \
        if ((isa) == RENDER_ISA_NEON) {                              \
            render_##name##_neon(__VA_ARGS__);                       \
            return;                                                  \
        }                                                            \
        render_##name##_scalar(__VA_ARGS__);                         \
    } while (0)
#else
#define RENDER_KERNEL_DISPATCH(isa, name, ...)                       \
    do {                                                             \
        (void)(isa);                                                 \
        render_##name##_scalar(__VA_ARGS__);                         \
    } while (0)
#endif

static inline RENDER_ALWAYS_INLINE void
render_kernel_fill(int isa, uint32_t *dst, int n, uint32_t color)
{
    RENDER_KERNEL_DISPATCH(isa, fill, dst, n, color);
}

static inline RENDER_ALWAYS_INLINE void
render_kernel_blit_tile(int isa, uint32_t *dst, size_t dst_stride_px,
                        const uint32_t *tile, int cw, int ch)
{
    RENDER_KERNEL_DISPATCH(isa, blit_tile, dst, dst_stride_px, tile, cw, ch);
}

static inline RENDER_ALWAYS_INLINE void
render_kernel_compose_alpha_lut(int isa, uint32_t *dst, const uint8_t *cov, int n,
                                const uint32_t *lut256)
{
    RENDER_KERNEL_DISPATCH(isa, compose_alpha_lut, dst, cov, n, lut256);
}

static inline RENDER_ALWAYS_INLINE void
render_kernel_compose_subpx_lut(int isa, uint32_t *dst, const uint8_t *cov, int n,
                                const struct subpx_lut *lut, uint32_t bg)
{
    RENDER_KERNEL_DISPATCH(isa, compose_subpx_lut, dst, cov, n, lut, bg);
}

static inline RENDER_ALWAYS_INLINE void
render_kernel_compose_color(int isa, uint32_t *dst, const uint32_t *premul, int n, bool faint)
{
    RENDER_KERNEL_DISPATCH(isa, compose_color, dst, premul, n, faint);
}

static inline RENDER_ALWAYS_INLINE void
render_kernel_blend_any(int isa, uint32_t *dst, const uint8_t *cov, int n, uint32_t fg)
{
    RENDER_KERNEL_DISPATCH(isa, blend_any, dst, cov, n, fg);
}

static inline RENDER_ALWAYS_INLINE void
render_kernel_blend_any_subpx(int isa, uint32_t *dst, const uint8_t *cov, int n, uint32_t fg)
{
    RENDER_KERNEL_DISPATCH(isa, blend_any_subpx, dst, cov, n, fg);
}

static inline RENDER_ALWAYS_INLINE void
render_kernel_blend_rect(int isa, uint32_t *dst, int n, uint32_t color, uint8_t a)
{
    RENDER_KERNEL_DISPATCH(isa, blend_rect, dst, n, color, a);
}

/* ---- Out-of-line table (kernels.c) ----------------------------------- */

struct render_kernels {
    int isa;
    void (*fill)(uint32_t *dst, int n, uint32_t color);
    void (*blit_tile)(uint32_t *dst, size_t dst_stride_px, const uint32_t *tile,
                      int cw, int ch);
    void (*compose_alpha_lut)(uint32_t *dst, const uint8_t *cov, int n,
                              const uint32_t *lut256);
    void (*compose_subpx_lut)(uint32_t *dst, const uint8_t *cov, int n,
                              const struct subpx_lut *lut, uint32_t bg);
    void (*compose_color)(uint32_t *dst, const uint32_t *premul, int n, bool faint);
    void (*blend_any)(uint32_t *dst, const uint8_t *cov, int n, uint32_t fg);
    void (*blend_any_subpx)(uint32_t *dst, const uint8_t *cov, int n, uint32_t fg);
    void (*blend_rect)(uint32_t *dst, int n, uint32_t color, uint8_t a);
};

/*
 * The kernel table for a level from render_isa_resolve(). A level that this
 * build has no variant for gives the SCALAR table.
 */
const struct render_kernels *render_kernels_for(int isa);

#endif
