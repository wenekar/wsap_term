#define _GNU_SOURCE
#include "render/render.h"
#include "render/internal.h"
#include "render/util.h"
#include "render/cell.h"
#include "paint/blend.h"
#include "paint/framebuffer.h"
#include "render/term_frame.h"
#include "render/backend.h"
#include "render/scene_paint.h"
#include "font/glyph_cache.h"
#include "font/cell_draw.h"
#include "paint/scene.h"
#include "paint/shadow.h"
#include "font/font.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>

static inline void futex_wait_val(_Atomic int *addr, int val) {
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}
static inline void futex_wake_val(_Atomic int *addr, int count) {
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}
#endif

#define FAINT_ALPHA 128

static __thread uint32_t *run_codepoints;
static __thread int run_capacity;

static inline uint32_t blend_glyph_px(uint32_t fg, uint32_t bg, uint8_t a);
static const uint32_t *blend_pair_lut(uint32_t fg, uint32_t bg);

#define BLEND_PAIR_SLOTS 16
static __thread struct blend_pair { uint64_t tag, pending; uint32_t lut[256]; }
    blend_pairs[BLEND_PAIR_SLOTS];

static void run_buffers_free(void)
{
    free(run_codepoints);
    run_codepoints = NULL;
    run_capacity = 0;
}

static bool ensure_run_capacity(int count)
{
    if (count <= run_capacity) return true;
    uint32_t *cps = realloc(run_codepoints, (size_t)count * sizeof(*run_codepoints));
    if (!cps) return false;
    run_codepoints = cps;
    run_capacity = count;
    return true;
}

#if defined(__aarch64__)
#include <arm_neon.h>
static inline void fill_pixels32(uint32_t *dst, uint32_t color, int count)
{
    if (count <= 0) return;
    uint32x4_t v = vdupq_n_u32(color);
    while (count >= 16) {
        vst1q_u32(dst, v);
        vst1q_u32(dst + 4, v);
        vst1q_u32(dst + 8, v);
        vst1q_u32(dst + 12, v);
        dst += 16;
        count -= 16;
    }
    while (count >= 4) {
        vst1q_u32(dst, v);
        dst += 4;
        count -= 4;
    }
    while (count > 0) {
        *dst++ = color;
        count--;
    }
}

static inline void copy_pixels32(uint32_t *dst, const uint32_t *src, int count)
{
    while (count >= 4) {
        vst1q_u32(dst, vld1q_u32(src));
        dst += 4; src += 4; count -= 4;
    }
    while (count-- > 0) *dst++ = *src++;
}

static inline void blend_pixels8(uint32_t *dst, const uint8_t *alpha, uint32_t fg,
                                 const uint32_t *lut, uint32_t bg)
{
    for (int i = 0; i < 8; i++)
        dst[i] = (lut && dst[i] == bg) ? lut[alpha[i]]
                                     : render_blend_px(fg, dst[i], alpha[i]);
}
#elif defined(__AVX2__)
#include <immintrin.h>
static inline __m256i tail_mask32(int count)
{
    return _mm256_cmpgt_epi32(_mm256_set1_epi32(count),
                              _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
}

static inline void fill_pixels32(uint32_t *dst, uint32_t color, int count)
{
    if (count <= 0) return;
    __m256i v = _mm256_set1_epi32((int)color);
    while (count >= 32) {
        _mm256_storeu_si256((__m256i *)dst, v);
        _mm256_storeu_si256((__m256i *)(dst + 8), v);
        _mm256_storeu_si256((__m256i *)(dst + 16), v);
        _mm256_storeu_si256((__m256i *)(dst + 24), v);
        dst += 32;
        count -= 32;
    }
    while (count >= 8) {
        _mm256_storeu_si256((__m256i *)dst, v);
        dst += 8;
        count -= 8;
    }
    if (count > 0)
        _mm256_maskstore_epi32((int *)dst, tail_mask32(count), v);
}

static inline __m256i blend8_chan(__m256i idx, __m256i a, __m256i ia, unsigned fgc)
{
    __m256i lb = _mm256_and_si256(_mm256_i32gather_epi32(
        (const int *)render_srgb_to_linear, idx, 2),
        _mm256_set1_epi32(0xFFFF));
    __m256i s = _mm256_add_epi32(
        _mm256_mullo_epi32(a, _mm256_set1_epi32(render_srgb_to_linear[fgc])),
        _mm256_mullo_epi32(ia, lb));
    __m256i t = _mm256_add_epi32(s, _mm256_set1_epi32(127));
    __m256i h = _mm256_srli_epi32(t, 8);
    __m256i z = _mm256_add_epi32(h, _mm256_and_si256(t, _mm256_set1_epi32(255)));
    __m256i zp1 = _mm256_add_epi32(z, _mm256_set1_epi32(1));
    return _mm256_add_epi32(h, _mm256_srli_epi32(
        _mm256_add_epi32(zp1, _mm256_slli_epi32(zp1, 8)), 16));
}

static inline __m256i blend8_px(__m256i p, __m256i a, uint32_t fg)
{
    render_ensure_srgb_luts();
    const __m256i m8 = _mm256_set1_epi32(0xFF);
    __m256i ia = _mm256_sub_epi32(_mm256_set1_epi32(255), a);
    __m256i lin_b = blend8_chan(_mm256_and_si256(p, m8), a, ia, fg & 0xFF);
    __m256i lin_g = blend8_chan(_mm256_and_si256(_mm256_srli_epi32(p, 8), m8),
                                a, ia, (fg >> 8) & 0xFF);
    __m256i lin_r = blend8_chan(_mm256_and_si256(_mm256_srli_epi32(p, 16), m8),
                                a, ia, (fg >> 16) & 0xFF);
    const int *enc = (const int *)render_linear_to_srgb;
    __m256i b = _mm256_and_si256(_mm256_i32gather_epi32(enc, lin_b, 1), m8);
    __m256i g = _mm256_and_si256(_mm256_i32gather_epi32(enc, lin_g, 1), m8);
    __m256i r = _mm256_and_si256(_mm256_i32gather_epi32(enc, lin_r, 1), m8);
    __m256i res = _mm256_or_si256(_mm256_or_si256(b, _mm256_slli_epi32(g, 8)),
        _mm256_or_si256(_mm256_slli_epi32(r, 16), _mm256_set1_epi32(0xFF000000)));
    res = _mm256_blendv_epi8(res, p, _mm256_cmpeq_epi32(a, _mm256_setzero_si256()));
    return _mm256_blendv_epi8(res, _mm256_set1_epi32((int)fg),
                              _mm256_cmpeq_epi32(a, _mm256_set1_epi32(255)));
}

#define CORRECT_PAIR_BITS 6
#define CORRECT_PAIR_SLOTS (1 << CORRECT_PAIR_BITS)

static __thread struct correct_pair { uint32_t tag, pending; uint8_t lut[256]; }
    correct_pairs[CORRECT_PAIR_SLOTS];

static const uint8_t *correct_pair_lut(int fg_l, int bg_l)
{
    if (fg_l - bg_l > -66 && fg_l - bg_l < 66)
        return NULL;
    uint32_t tag = ((uint32_t)fg_l << 16) | (uint32_t)bg_l;
    unsigned i = (tag * UINT32_C(2654435761)) >> (32 - CORRECT_PAIR_BITS) & (CORRECT_PAIR_SLOTS - 1);
    struct correct_pair *cp = &correct_pairs[i];
    if (cp->tag == tag)
        return cp->lut;
    if (cp->pending != tag) {
        cp->pending = tag;
        return NULL;
    }
    for (int a = 0; a < 256; a++)
        cp->lut[a] = render_correct_coverage(fg_l, bg_l, (uint8_t)a);
    cp->tag = tag;
    cp->pending = 0;
    return cp->lut;
}

static inline __m256i correct8_coverage(__m256i p, const uint8_t *alpha,
                                        int count, uint32_t fg)
{
    alignas(32) uint32_t px[8];
    alignas(16) uint8_t ca[8] = {0};
    _mm256_storeu_si256((__m256i *)px, p);
    int fg_l = (int)render_lum16(fg);
    bool uniform = count == 8
        ? (_mm256_movemask_epi8(_mm256_cmpeq_epi32(p, _mm256_set1_epi32((int)px[0]))) == -1)
        : true;
    if (count < 8) {
        for (int i = 1; i < count; i++) {
            if (px[i] != px[0]) {
                uniform = false;
                break;
            }
        }
    }
    if (uniform) {
        int bg_l = (int)render_lum16(px[0]);
        const uint8_t *clut = correct_pair_lut(fg_l, bg_l);
        if (clut) {
            for (int i = 0; i < count; i++)
                ca[i] = clut[alpha[i]];
        } else {
            for (int i = 0; i < count; i++)
                ca[i] = render_correct_coverage(fg_l, bg_l, alpha[i]);
        }
    } else {
        for (int i = 0; i < count; i++) {
            int bg_l = (int)render_lum16(px[i]);
            const uint8_t *clut = correct_pair_lut(fg_l, bg_l);
            ca[i] = clut ? clut[alpha[i]]
                         : render_correct_coverage(fg_l, bg_l, alpha[i]);
        }
    }
    return _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)ca));
}

static inline void blend_pixels8(uint32_t *dst, const uint8_t *alpha, uint32_t fg,
                                 const uint32_t *lut, uint32_t bg)
{
    __m256i p = _mm256_loadu_si256((const __m256i *)dst);
    if (lut && _mm256_movemask_epi8(
            _mm256_cmpeq_epi32(p, _mm256_set1_epi32((int)bg))) == -1) {
        __m128i a8 = _mm_loadu_si64(alpha);
        __m256i a32 = _mm256_cvtepu8_epi32(a8);
        __m256i res = _mm256_i32gather_epi32((const int *)lut, a32, 4);
        _mm256_storeu_si256((__m256i *)dst, res);
        return;
    }
    __m256i a = correct8_coverage(p, alpha, 8, fg);
    _mm256_storeu_si256((__m256i *)dst, blend8_px(p, a, fg));
}

static inline void blend_pixels_tail(uint32_t *dst, const uint8_t *alpha,
                                     uint32_t fg, int count,
                                     const uint32_t *lut, uint32_t bg)
{
    __m256i m = tail_mask32(count);
    __m256i p = _mm256_maskload_epi32((const int *)dst, m);
    __m256i unif = _mm256_or_si256(
        _mm256_cmpeq_epi32(p, _mm256_set1_epi32((int)bg)),
        _mm256_andnot_si256(m, _mm256_set1_epi32(-1)));
    if (lut && _mm256_movemask_epi8(unif) == -1) {
        alignas(16) uint8_t a_buf[8] = {0};
        for (int i = 0; i < count; i++)
            a_buf[i] = alpha[i];
        __m128i a8 = _mm_loadl_epi64((const __m128i *)a_buf);
        __m256i a32 = _mm256_cvtepu8_epi32(a8);
        __m256i res = _mm256_i32gather_epi32((const int *)lut, a32, 4);
        _mm256_maskstore_epi32((int *)dst, m, res);
        return;
    }
    __m256i a = correct8_coverage(p, alpha, count, fg);
    _mm256_maskstore_epi32((int *)dst, m, blend8_px(p, a, fg));
}
#else
static inline void fill_pixels32(uint32_t *dst, uint32_t color, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = color;
}
static inline void copy_pixels32(uint32_t *dst, const uint32_t *src, int count)
{
    memcpy(dst, src, (size_t)count * sizeof(*dst));
}
#endif

static inline void blit_tile(uint32_t *dst, size_t stride,
                             const uint32_t *tile, int cw, int ch)
{
#if defined(__AVX2__)
    if (__builtin_expect(cw == 8, 1)) {
        for (int y = 0; y < ch; y++) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(tile + (size_t)y * 8));
            _mm256_storeu_si256((__m256i *)(dst + (size_t)y * stride), v);
        }
        return;
    }
    const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    if (cw <= 8) {
        __m256i m = _mm256_cmpgt_epi32(_mm256_set1_epi32(cw), iota);
        for (int y = 0; y < ch; y++) {
            __m256i v = _mm256_maskload_epi32(
                (const int *)(tile + (size_t)y * cw), m);
            _mm256_maskstore_epi32((int *)(dst + (size_t)y * stride), m, v);
        }
        return;
    }
    int body = cw & ~7, rem = cw & 7;
    __m256i m = _mm256_cmpgt_epi32(_mm256_set1_epi32(rem), iota);
    for (int y = 0; y < ch; y++) {
        const uint32_t *s = tile + (size_t)y * cw;
        uint32_t *d = dst + (size_t)y * stride;
        for (int x = 0; x < body; x += 8)
            _mm256_storeu_si256((__m256i *)(d + x),
                                _mm256_loadu_si256((const __m256i *)(s + x)));
        if (rem)
            _mm256_maskstore_epi32((int *)(d + body), m,
                                   _mm256_maskload_epi32((const int *)(s + body), m));
    }
#else
    for (int y = 0; y < ch; y++)
        copy_pixels32(dst + (size_t)y * stride, tile + (size_t)y * cw, cw);
#endif
}

static const uint32_t *blend_pair_lut(uint32_t fg, uint32_t bg)
{
    uint64_t tag = (uint64_t)fg << 32 | bg;
    unsigned i = (unsigned)((tag * UINT64_C(0x9E3779B97F4A7C15)) >> 60) &
                 (BLEND_PAIR_SLOTS - 1);
    struct blend_pair *bp = &blend_pairs[i];
    if (bp->tag == tag)
        return bp->lut;
    if (bp->pending != tag) {
        bp->pending = tag;
        return NULL;
    }
    int fg_l = (int)render_lum16(fg), bg_l = (int)render_lum16(bg);
    for (int a = 0; a < 256; a++)
        bp->lut[a] = render_blend_px_lum(fg, bg, fg_l, bg_l, (uint8_t)a);
    bp->tag = tag;
    bp->pending = 0;
    return bp->lut;
}

static inline uint32_t blend_glyph_px(uint32_t fg, uint32_t bg, uint8_t a)
{
    const uint32_t *lut = blend_pair_lut(fg, bg);
    return lut ? lut[a] : render_blend_px(fg, bg, a);
}

static inline uint32_t blend_premul_glyph_px(uint32_t premul, uint32_t bg, bool faint)
{
    unsigned a = premul >> 24;
    if (!a) return bg;
    unsigned coverage = faint ? (a + 1) / 2 : a;
    uint32_t result = 0xFF000000;

    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned channel = (((premul >> shift) & 255) * 255 + a / 2) / a;
        if (channel > 255) channel = 255;
        result |= render_blend_channel(channel, (bg >> shift) & 255, coverage) << shift;
    }
    return result;
}

static struct font_instance *term_font(const struct renderer *r)
{
    return r && r->font ? r->font : font_instance_default();
}

static struct font_instance *ui_font(const struct renderer *r)
{
    return r && r->ui_font ? r->ui_font : font_instance_default();
}

static inline uint32_t resolve_cell_bg(const struct terminal_frame *frame,
                                       const struct wsap_vtcore_cell *cells, int c,
                                       int sel_c0, int sel_c1, int cursor)
{
    bool rev = frame->reverse_video;
    bool is_cur = cells ? render_cell_is_cursor(&cells[c], c, cursor)
                        : c == cursor;
    if (is_cur && frame->cursor_style == WSAP_VTCORE_CURSOR_BLOCK)
        return frame->cursor_color;
    if (render_cell_selected(c, sel_c0, sel_c1))
        return TERM_SELECTION_BG;
    if (!cells)
        return rev ? frame->default_fg : frame->default_bg;
    const struct wsap_vtcore_cell *cell = &cells[c];
    if (cell->text == 0 && cell->width != 0)
        return rev ? frame->default_fg : frame->default_bg;
    return terminal_frame_color(frame, rev ? cell->fg : cell->bg);
}

static void paint_row_backgrounds(const struct terminal_frame *frame,
                                  uint32_t *row_px, int width, int cols,
                                  const struct wsap_vtcore_cell *cells, int cursor,
                                  int cw, int ch, int sel_c0, int sel_c1)
{
    if (!frame->reverse_video && sel_c0 > sel_c1 && (cursor < 0 || cursor >= cols)) {
        bool uniform = true;
        if (cells) {
            for (int i = 0; i < cols; i++) {
                if (cells[i].bg != WSAP_VTCORE_COLOR_BG &&
                    terminal_frame_color(frame, cells[i].bg) != frame->default_bg) {
                    uniform = false;
                    break;
                }
            }
        }
        if (uniform) {
            fill_pixels32(row_px, frame->default_bg, ch * width);
            return;
        }
    }
    int c = 0;
    while (c < cols) {
        uint32_t bg = resolve_cell_bg(frame, cells, c, sel_c0, sel_c1, cursor);
        int end = c + 1;
        while (end < cols && resolve_cell_bg(frame, cells, end, sel_c0, sel_c1, cursor) == bg)
            end++;
        int x0 = c * cw;
        int span_w = (end - c) * cw;
        if (x0 == 0 && span_w == width) {
            fill_pixels32(row_px, bg, ch * width);
        } else {
            for (int y = 0; y < ch; y++) {
                uint32_t *dst = row_px + (size_t)y * width + x0;
                fill_pixels32(dst, bg, span_w);
            }
        }
        c = end;
    }
}

static void paint_glyph(uint32_t *row_px, int width, int x0, int ch,
                        const struct glyph *glyph, uint32_t fg,
                        bool faint)
{
    if (!glyph->row_masks || (glyph->has_color && !glyph->color))
        return;
    if (glyph->has_color) {
        for (int y = 0; y < ch; y++) {
            uint32_t *dst = row_px + (size_t)y * width;
            const uint64_t *mask = glyph_mask(glyph, y);
            for (int w = 0; w < glyph->mask_words; w++) {
                uint64_t ink = mask[w];
                while (ink) {
                    int x = w * 64 + __builtin_ctzll(ink);
                    ink &= ink - 1;
                    if (x >= glyph->width) break;
                    int dx = x0 + glyph->x_offset + x;
                    if (dx < 0 || dx >= width) continue;
                    uint32_t p = *glyph_color_at(glyph, y, x);
                    if (p > 0x00FFFFFF)
                        dst[dx] = blend_premul_glyph_px(p, dst[dx], faint);
                }
            }
        }
    } else if (glyph->has_subpx_ink && glyph->subpx) {
        int x0_glyph = x0 + glyph->x_offset;
        for (int y = 0; y < ch; y++) {
            if (!glyph_row_inked(glyph, y)) continue;
            uint32_t *dst = row_px + (size_t)y * width;
            const uint64_t *mask = glyph_mask(glyph, y);
            for (int w = 0; w < glyph->mask_words; w++) {
                uint64_t ink = mask[w];
                while (ink) {
                    int x = w * 64 + __builtin_ctzll(ink);
                    ink &= ink - 1;
                    if (x >= glyph->width) break;
                    int dx = x0_glyph + x;
                    if (dx < 0 || dx >= width) continue;
                    uint8_t sr = glyph->subpx[y][x * 3 + 0];
                    uint8_t sg = glyph->subpx[y][x * 3 + 1];
                    uint8_t sb = glyph->subpx[y][x * 3 + 2];
                    if (faint) {
                        sr = (uint8_t)((sr + 1) / 2);
                        sg = (uint8_t)((sg + 1) / 2);
                        sb = (uint8_t)((sb + 1) / 2);
                    }
                    dst[dx] = render_blend_subpx(fg, dst[dx], sr, sg, sb);
                }
            }
        }
    } else {
        int x0_glyph = x0 + glyph->x_offset;
        bool in_bounds = x0_glyph >= 0 && x0_glyph + glyph->width <= width;
        if (in_bounds && !faint) {
            uint32_t bg0 = row_px[x0_glyph];
            const uint32_t *lut = blend_pair_lut(fg, bg0);
            for (int y = 0; y < ch; y++) {
                if (!glyph_row_inked(glyph, y)) continue;
                uint32_t *dst = row_px + (size_t)y * width + x0_glyph;
#if defined(__aarch64__)
                int x = 0;
                for (; x + 8 <= glyph->width; x += 8)
                    blend_pixels8(dst + x, glyph->alpha[y] + x, fg, lut, bg0);
                for (; x < glyph->width; x++)
                    dst[x] = blend_glyph_px(fg, dst[x], glyph->alpha[y][x]);
#elif defined(__AVX2__)
                int x = 0;
                for (; x + 8 <= glyph->width; x += 8)
                    blend_pixels8(dst + x, glyph->alpha[y] + x, fg, lut, bg0);
                if (x < glyph->width)
                    blend_pixels_tail(dst + x, glyph->alpha[y] + x, fg,
                                      glyph->width - x, lut, bg0);
#else
                const uint64_t *mask = glyph_mask(glyph, y);
                for (int w = 0; w < glyph->mask_words; w++) {
                    uint64_t ink = mask[w];
                    while (ink) {
                        int x = w * 64 + __builtin_ctzll(ink);
                        ink &= ink - 1;
                        if (x >= glyph->width) break;
                        uint8_t a = glyph->alpha[y][x];
                        if (a == 255) {
                            dst[x] = fg;
                        } else if (a) {
                            dst[x] = (lut && dst[x] == bg0) ? lut[a]
                                : render_blend_px(fg, dst[x], a);
                        }
                    }
                }
#endif
            }
        } else {
            for (int y = 0; y < ch; y++) {
                uint32_t *dst = row_px + (size_t)y * width;
                const uint64_t *mask = glyph_mask(glyph, y);
                for (int w = 0; w < glyph->mask_words; w++) {
                    uint64_t ink = mask[w];
                    while (ink) {
                        int x = w * 64 + __builtin_ctzll(ink);
                        ink &= ink - 1;
                        if (x >= glyph->width) break;
                        int dx = x0_glyph + x;
                        if (dx < 0 || dx >= width) continue;
                        uint8_t a = glyph->alpha[y][x];
                        if (faint) a = (uint8_t)((a + 1) / 2);
                        if (a == 255) {
                            dst[dx] = fg;
                        } else if (a) {
                            dst[dx] = blend_glyph_px(fg, dst[dx], a);
                        }
                    }
                }
            }
        }
    }
}

static void draw_decorations(uint32_t *row_px, int width, int x0, int cw, int ch,
                             uint16_t attrs, uint32_t fg)
{
    if (!render_cell_decorated(attrs))
        return;
    int ys[5], n_dec = 0;
    enum wsap_vtcore_underline ul = wsap_vtcore_attr_underline(attrs);
    if ((ul != WSAP_VTCORE_UNDERLINE_NONE && ul != WSAP_VTCORE_UNDERLINE_DOUBLE) ||
        (attrs & WSAP_VTCORE_ATTR_LINK))
        ys[n_dec++] = ch - 2;
    if (ul == WSAP_VTCORE_UNDERLINE_DOUBLE) {
        ys[n_dec++] = ch - 3;
        ys[n_dec++] = ch - 1;
    }
    if (attrs & WSAP_VTCORE_ATTR_STRIKE)    ys[n_dec++] = ch / 2;
    if (attrs & WSAP_VTCORE_ATTR_OVERLINE)  ys[n_dec++] = 1;
    for (int d = 0; d < n_dec; d++) {
        if (ys[d] < 0 || ys[d] >= ch) continue;
        uint32_t *dst = row_px + (size_t)ys[d] * width + x0;
        if (!(attrs & WSAP_VTCORE_ATTR_FAINT)) {
            fill_pixels32(dst, fg, cw);
            continue;
        }
        for (int x = 0; x < cw; x++)
            dst[x] = blend_glyph_px(fg, dst[x], FAINT_ALPHA);
    }
}

struct row_paint {
    struct font_instance *fi;
    const struct terminal_frame *frame;
    uint32_t *row_px;
    const struct wsap_vtcore_cell *cells;
    int width, cols, cursor, cw, ch, sel_c0, sel_c1;
    bool plain_cells;
    int base, next;
};

static void paint_cell(const struct row_paint *p, int c)
{
    const struct terminal_frame *frame = p->frame;
    if (p->cells && p->cells[c].text <= ' ' && p->cells[c].width != 0 &&
        !render_cell_decorated(p->cells[c].attrs))
        return;
    struct wsap_vtcore_cell cell = p->plain_cells
        ? (p->cells ? render_cell_colors(frame, p->cells[c]) : render_blank_cell(frame))
        : render_resolve_cell(frame, p->cells, c, p->sel_c0, p->sel_c1, p->cursor);
    uint16_t style = render_cell_style(&cell);
    uint32_t *row_px = p->row_px;
    int cw = p->cw, ch = p->ch;
    if (cell.text > ' ') {
        bool faint = (cell.attrs & WSAP_VTCORE_ATTR_FAINT) != 0;
        uint32_t glyph_cp = terminal_frame_glyph(frame, cell.text);
        const uint32_t *tile = NULL;
        if (cell.fg == frame->default_fg && cell.bg == frame->default_bg && !faint)
            tile = glyph_cache_fetch_tile(p->fi, glyph_cp, style);
        if (tile) {
            blit_tile(row_px + (size_t)c * cw, p->width, tile, cw, ch);
        } else {
            const struct glyph *glyph = glyph_cache_fetch(p->fi, glyph_cp, style);
            if (glyph && glyph->width > 0 && glyph->row_masks)
                paint_glyph(row_px, p->width, c * cw, ch, glyph, cell.fg, faint);
        }
    }
    draw_decorations(row_px, p->width, c * cw, cw, ch, cell.attrs, cell.fg);
}

static void paint_span(void *ctx, const struct glyph_span *span)
{
    struct row_paint *p = ctx;
    int start = p->base + span->start;
    for (; p->next < start; p->next++)
        paint_cell(p, p->next);
    int cw = p->cw, ch = p->ch;
    for (int i = 0; i < span->count; i++) {
        int c = start + i;
        struct wsap_vtcore_cell ci = render_resolve_cell(p->frame, p->cells, c, p->sel_c0,
                                                          p->sel_c1, p->cursor);
        bool faint = (ci.attrs & WSAP_VTCORE_ATTR_FAINT) != 0;
        if (span->tiles && ci.fg == p->frame->default_fg &&
            ci.bg == p->frame->default_bg && !faint)
            blit_tile(p->row_px + (size_t)c * cw, p->width,
                      span->tiles + (size_t)i * cw * ch, cw, ch);
        else
            paint_glyph(p->row_px, p->width, c * cw, ch, &span->cells[i], ci.fg, faint);
        draw_decorations(p->row_px, p->width, c * cw, cw, ch, ci.attrs, ci.fg);
    }
    p->next = start + span->count;
}

static void paint_row_foreground(struct font_instance *fi,
                             const struct terminal_frame *frame,
                             uint32_t *row_px, int width, int cols,
                             const struct wsap_vtcore_cell *cells, int cursor,
                             int cw, int ch, int sel_c0, int sel_c1)
{
    struct row_paint p = {
        .fi = fi, .frame = frame, .row_px = row_px, .cells = cells,
        .width = width, .cols = cols, .cursor = cursor, .cw = cw, .ch = ch,
        .sel_c0 = sel_c0, .sel_c1 = sel_c1,
        .plain_cells = !frame->reverse_video && sel_c0 > sel_c1 &&
                       (cursor < 0 || cursor >= cols),
    };
    for (int c = 0; c < cols;) {
        int end = cells ? render_segment_end(cells, c, cols, is_cell_aligned_symbol) : c + 1;
        if (end - c > 1 && ensure_run_capacity(end - c)) {
            for (int i = c; i < end; i++)
                run_codepoints[i - c] = cells[i].text;
            p.base = p.next = c;
            glyph_cache_shape_segment(fi, run_codepoints, end - c, render_cell_style(&cells[c]),
                                      paint_span, &p);
            for (; p.next < end; p.next++)
                paint_cell(&p, p.next);
        } else {
            for (int i = c; i < end; i++)
                paint_cell(&p, i);
        }
        c = end;
    }
}

static void draw_nonblock_cursor(const struct terminal_frame *frame, uint32_t *row_px,
                        int width, int cursor, int cols, int cw, int ch)
{
    if (cursor < 0 || cursor >= cols || frame->cursor_style == WSAP_VTCORE_CURSOR_BLOCK)
        return;
    uint32_t *dst = row_px + cursor * cw;
    for (int y = 0; y < ch; y++, dst += width) {
        if (frame->cursor_style == WSAP_VTCORE_CURSOR_BAR) {
            dst[0] = frame->cursor_color;
            if (cw > 1) dst[1] = frame->cursor_color;
        } else if (frame->cursor_style == WSAP_VTCORE_CURSOR_UNDERLINE && y >= ch - 2) {
            fill_pixels32(dst, frame->cursor_color, cw);
        }
    }
}

static void paint_row(struct font_instance *fi, const struct terminal_frame *frame,
                      uint32_t *px, int width, int row)
{
    const struct font_metrics *m = font_instance_metrics(fi);
    int cw = m->cell_w, ch = m->cell_h;
    int cols = width / cw;
    if (cols > frame->cols) cols = frame->cols;
    const struct wsap_vtcore_cell *cells = terminal_frame_row(frame, row);
    uint32_t *row_px = px + (size_t)row * ch * width;
    int sel_c0 = -1, sel_c1 = -2;
    terminal_frame_row_selection(frame, row, cols, &sel_c0, &sel_c1);
    int cursor = row == frame->cursor_y ? frame->cursor_x : -1;

    paint_row_backgrounds(frame, row_px, width, cols, cells, cursor, cw, ch, sel_c0, sel_c1);
    paint_row_foreground(fi, frame, row_px, width, cols, cells, cursor, cw, ch, sel_c0, sel_c1);
    draw_nonblock_cursor(frame, row_px, width, cursor, cols, cw, ch);
}

static void fill_px_range(uint32_t *px, size_t count, uint32_t color)
{
    for (size_t i = 0; i < count;) {
        int chunk = count - i > INT32_MAX ? INT32_MAX : (int)(count - i);
        fill_pixels32(px + i, color, chunk);
        i += (size_t)chunk;
    }
}

static int paint_right_margin_x(const struct font_metrics *m,
                                const struct terminal_frame *frame, int width)
{
    int cols = width / m->cell_w;
    if (cols > frame->cols) cols = frame->cols;
    return cols * m->cell_w;
}

static void fill_row_right_margin(const struct font_metrics *m,
                                  const struct terminal_frame *frame,
                                  uint32_t *px, int width, int row, int right)
{
    if (right >= width)
        return;
    uint32_t *row_px = px + (size_t)row * m->cell_h * width;
    for (int y = 0; y < m->cell_h; y++)
        fill_pixels32(row_px + (size_t)y * width + right, frame->default_bg,
                      width - right);
}

static void paint_grid_margins(const struct font_metrics *m,
                               const struct terminal_frame *frame,
                               uint32_t *px, int width, int height)
{
    int cols = width / m->cell_w;
    if (cols > frame->cols) cols = frame->cols;
    int rows = height / m->cell_h;
    if (rows > frame->rows) rows = frame->rows;
    int right = cols * m->cell_w;
    int bottom = rows * m->cell_h;
    for (int y = 0; y < bottom; y++)
        fill_pixels32(px + (size_t)y * width + right, frame->default_bg,
                      width - right);
    fill_px_range(px + (size_t)bottom * width,
                  (size_t)(height - bottom) * width, frame->default_bg);
}

static bool renderer_reserve_rows(struct renderer_history *h, int rows)
{
    if (rows <= h->rows_cap)
        return true;
    int cap = h->rows_cap ? h->rows_cap : 256;
    while (cap < rows)
        cap *= 2;
    struct render_row **tables[FRAMEBUFFER_BUFFERS + 1];
    for (int i = 0; i < FRAMEBUFFER_BUFFERS; i++)
        tables[i] = &h->buf_rows[i];
    tables[FRAMEBUFFER_BUFFERS] = &h->front_rows;
    for (int i = 0; i <= FRAMEBUFFER_BUFFERS; i++) {
        struct render_row *p = realloc(*tables[i], (size_t)cap * sizeof(*p));
        if (!p)
            return false;
        memset(p + h->rows_cap, 0, (size_t)(cap - h->rows_cap) * sizeof(*p));
        *tables[i] = p;
    }
    h->rows_cap = cap;
    return true;
}

static void renderer_history_init(struct renderer_history *h)
{
    *h = (struct renderer_history){0};
    for (int i = 0; i < FRAMEBUFFER_BUFFERS; i++) {
        h->buf_cur_y[i] = -1;
        h->buf_cur_style[i] = -1;
    }
    h->ui_buf = -1;
}

static void renderer_history_clear(struct renderer_history *h)
{
    for (int i = 0; i < FRAMEBUFFER_BUFFERS; i++)
        free(h->buf_rows[i]);
    free(h->front_rows);
    renderer_history_init(h);
}

static void renderer_history_destroy(void *data)
{
    struct renderer_history *h = data;
    renderer_history_clear(h);
    free(h);
}

static struct renderer_history *renderer_history_get(
        struct framebuffer_history *owner, struct renderer_history *temporary)
{
    if (!owner)
        return temporary;
    if (!owner->data) {
        struct renderer_history *h = malloc(sizeof(*h));
        if (!h)
            return temporary;
        renderer_history_init(h);
        owner->data = h;
        owner->destroy = renderer_history_destroy;
    }
    return owner->data;
}

void renderer_init(struct renderer *r)
{
    memset(r, 0, sizeof(*r));
    render_ensure_srgb_luts();
    r->gpu = render_gpu_create();
}

struct renderer *renderer_create(void)
{
    struct renderer *r = malloc(sizeof(*r));
    if (r)
        renderer_init(r);
    return r;
}

void renderer_destroy(struct renderer *r)
{
    if (!r)
        return;
    render_pool_destroy();
    render_gpu_destroy(r->gpu);
    font_instance_unref(r->font);
    font_instance_unref(r->ui_font);
    free(r);
}

void *renderer_device(struct renderer *r)
{
    return r ? render_gpu_device(r->gpu) : NULL;
}

static void hold(struct font_instance **slot, struct font_instance *fi)
{
    if (*slot == fi)
        return;
    font_instance_unref(*slot);
    *slot = font_instance_ref(fi);
}

static void renderer_repaint_all(struct renderer_history *h)
{
    if (!h)
        return;
    h->front_valid = false;
    for (int i = 0; i < FRAMEBUFFER_BUFFERS; i++) {
        h->buf_w[i] = 0;
        h->buf_ui_frame[i] = 0;
    }
}

#define PAINT_POOL_MAX 15
static pthread_t pool_threads[PAINT_POOL_MAX];
static int pool_count = -1;
static _Atomic int pool_seq;
static _Atomic int pool_pending;
static _Atomic bool pool_stop;
#ifndef __linux__
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pool_done_cv = PTHREAD_COND_INITIALIZER;
#endif
static struct {
    uint32_t *px;
    int width, rows;
    int workers;
    struct font_instance *font;
    struct terminal_frame frame;
} pool_job;

static void pool_wake_all(void)
{
#ifdef __linux__
    atomic_fetch_add_explicit(&pool_seq, 1, memory_order_release);
    futex_wake_val(&pool_seq, INT_MAX);
#else
    pthread_mutex_lock(&pool_mu);
    atomic_fetch_add_explicit(&pool_seq, 1, memory_order_release);
    pthread_cond_broadcast(&pool_cv);
    pthread_mutex_unlock(&pool_mu);
#endif
}

static void *paint_worker(void *arg)
{
    int id = (int)(uintptr_t)arg;
    (void)font_thread_attach();
    int seen = 0;
    for (;;) {
#ifdef __linux__
        while (atomic_load_explicit(&pool_seq, memory_order_acquire) == seen)
            futex_wait_val(&pool_seq, seen);
        seen = atomic_load_explicit(&pool_seq, memory_order_acquire);
#else
        pthread_mutex_lock(&pool_mu);
        while (atomic_load_explicit(&pool_seq, memory_order_acquire) == seen)
            pthread_cond_wait(&pool_cv, &pool_mu);
        seen = atomic_load_explicit(&pool_seq, memory_order_acquire);
        pthread_mutex_unlock(&pool_mu);
#endif
        if (atomic_load_explicit(&pool_stop, memory_order_acquire))
            break;

        glyph_cache_flush_deferred();
        (void)font_thread_attach();

        int total = pool_job.workers + 1;
        int r0 = id * pool_job.rows / total;
        int r1 = (id + 1) * pool_job.rows / total;
        for (int row = r0; row < r1; row++)
            paint_row(pool_job.font, &pool_job.frame, pool_job.px, pool_job.width, row);

        if (atomic_fetch_sub_explicit(&pool_pending, 1,
                memory_order_acq_rel) == 1) {
#ifdef __linux__
            futex_wake_val(&pool_pending, 1);
#else
            pthread_mutex_lock(&pool_mu);
            pthread_cond_signal(&pool_done_cv);
            pthread_mutex_unlock(&pool_mu);
#endif
        }
    }
    glyph_cache_thread_detach();
    font_thread_detach();
    run_buffers_free();
    return NULL;
}

static void pool_spawn(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN) - 1;
    if (n > 3) n = 3;
    const char *env = getenv("WSAP_POOL");
    if (env) {
        char *end;
        errno = 0;
        long requested = strtol(env, &end, 10);
        if (!errno && end != env && !*end && requested >= 0)
            n = requested;
        else
            fprintf(stderr, "ignoring invalid WSAP_POOL value: %s\n", env);
    }
    if (n < 0) n = 0;
    if (n > PAINT_POOL_MAX) n = PAINT_POOL_MAX;
    atomic_store_explicit(&pool_stop, false, memory_order_relaxed);

    atomic_store_explicit(&pool_seq, 0, memory_order_relaxed);
    atomic_store_explicit(&pool_pending, 0, memory_order_relaxed);
    pool_job = (typeof(pool_job)){0};

    pool_count = 0;
    sigset_t set, old;
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    for (long i = 0; i < n; i++)
        if (pthread_create(&pool_threads[pool_count], NULL, paint_worker, (void *)(uintptr_t)i))
            break;
        else
            pool_count++;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

void render_pool_destroy(void)
{
    if (pool_count <= 0) {
        pool_count = -1;
        run_buffers_free();
        return;
    }
    atomic_store_explicit(&pool_stop, true, memory_order_release);
    pool_wake_all();
    for (int i = 0; i < pool_count; i++)
        pthread_join(pool_threads[i], NULL);
    pool_count = -1;
    atomic_store_explicit(&pool_stop, false, memory_order_relaxed);
    run_buffers_free();
}

static void paint_rows_parallel(struct font_instance *fi,
                                const struct terminal_frame *frame,
                                uint32_t *px, int width, int rows)
{
    if (pool_count < 0)
        pool_spawn();
    if (!pool_count) {
        for (int row = 0; row < rows; row++)
            paint_row(fi, frame, px, width, row);
        glyph_cache_flush_frame();
        return;
    }
    glyph_cache_set_parallel(true);
    pool_job = (typeof(pool_job)){ px, width, rows, pool_count, fi, *frame };
    atomic_store_explicit(&pool_pending, pool_count, memory_order_relaxed);
    pool_wake_all();

    int total = pool_job.workers + 1;
    int r0 = pool_job.workers * rows / total;
    int r1 = rows;
    for (int row = r0; row < r1; row++)
        paint_row(fi, frame, px, width, row);

#ifdef __linux__
    int p;
    while ((p = atomic_load_explicit(&pool_pending, memory_order_acquire)) > 0)
        futex_wait_val(&pool_pending, p);
#else
    pthread_mutex_lock(&pool_mu);
    while (atomic_load_explicit(&pool_pending, memory_order_acquire) > 0)
        pthread_cond_wait(&pool_done_cv, &pool_mu);
    pthread_mutex_unlock(&pool_mu);
#endif
    glyph_cache_set_parallel(false);
    glyph_cache_flush_frame();
}

static int render_users;

bool render_init(void)
{
    if (render_users > 0) {
        render_users++;
        return true;
    }
    render_build_srgb_luts();
    if (!font_system_init())
        return false;
    render_users = 1;
    return true;
}

void render_shutdown(void)
{
    if (render_users == 0 || --render_users > 0)
        return;
    render_pool_destroy();
    font_system_shutdown();
    ui_shadow_cache_destroy();
}

void render_warm_caches(void)
{
    for (uint32_t c = 32; c < 127; c++)
        glyph_cache_fetch(font_instance_default(), c, 0);
}

void render_paint_full_terminal_frame(struct font_instance *fi, uint32_t *px,
                                      int width, int height,
                                      const struct terminal_frame *frame)
{
    if (!px || width <= 0 || height <= 0) return;
    if (!fi)
        fi = font_instance_default();
    const struct font_metrics *m = font_instance_metrics(fi);
    if (!frame) {
        fill_px_range(px, (size_t)width * height, 0);
        return;
    }
    glyph_cache_begin_frame(fi, frame->default_fg, frame->default_bg);
    int rows = height / m->cell_h;
    if (rows > frame->rows) rows = frame->rows;
    paint_grid_margins(m, frame, px, width, height);
    if (terminal_frame_valid(frame) && rows > 3 && !getenv("WSAP_NO_PARALLEL")) {
        paint_rows_parallel(fi, frame, px, width, rows);
        return;
    }
    for (int row = 0; row < rows; row++)
        paint_row(fi, frame, px, width, row);
}

static bool grid_bands_touch(struct rect2i a, struct rect2i b)
{
    return a.x <= b.x + b.width && b.x <= a.x + a.width &&
           a.y <= b.y + b.height && b.y <= a.y + a.height;
}

static void grid_band_add(struct renderer_history *h, int buf, struct rect2i band)
{
    if (rect2i_is_empty(band))
        return;
    struct rect2i *bands = h->buf_grid_bands[buf];
    int n = h->buf_grid_bands_n[buf];
    bool merged;
    do {
        merged = false;
        for (int i = 0; i < n;) {
            if (grid_bands_touch(bands[i], band)) {
                band = rect2i_union(bands[i], band);
                bands[i] = bands[--n];
                merged = true;
                continue;
            }
            i++;
        }
    } while (merged);
    if (n < RENDER_GRID_BANDS_MAX) {
        bands[n] = band;
        h->buf_grid_bands_n[buf] = n + 1;
        return;
    }
    for (int i = 0; i < n; i++)
        band = rect2i_union(band, bands[i]);
    bands[0] = band;
    h->buf_grid_bands_n[buf] = 1;
}

static int rect2i_minus(struct rect2i a, struct rect2i b,
                        struct rect2i out[4])
{
    struct rect2i i = rect2i_intersect(a, b);
    if (rect2i_is_empty(i)) {
        out[0] = a;
        return 1;
    }
    struct vector2i ae = rect2i_end(a), ie = rect2i_end(i);
    int n = 0;
    if (a.y < i.y)
        out[n++] = (struct rect2i){
            .pos = a.pos,
            .size = { .x = a.width, .y = i.y - a.y } };
    if (ie.y < ae.y)
        out[n++] = (struct rect2i){
            .pos = { .x = a.x, .y = ie.y },
            .size = { .x = a.width, .y = ae.y - ie.y } };
    if (a.x < i.x)
        out[n++] = (struct rect2i){
            .pos = { .x = a.x, .y = i.y },
            .size = { .x = i.x - a.x, .y = ie.y - i.y } };
    if (ie.x < ae.x)
        out[n++] = (struct rect2i){
            .pos = { .x = ie.x, .y = i.y },
            .size = { .x = ae.x - ie.x, .y = ie.y - i.y } };
    return n;
}

static struct render_damage renderer_paint_frame(struct renderer_history *h,
                                                 struct font_instance *fi, uint32_t *px,
                                                 int width, int height, int buf,
                                                 int origin,
                                                 const struct terminal_frame *frame,
                                                 void (*emit_run)(void *ctx, int y, int h),
                                                 void *emit_ctx)
{
    if (!h || !px || width <= 0 || height <= 0 ||
        (unsigned)buf >= FRAMEBUFFER_BUFFERS)
        return (struct render_damage){0, 0};
    if (!fi)
        fi = font_instance_default();
    const struct font_metrics *m = font_instance_metrics(fi);
    uint64_t font_gen = font_instance_version(fi);
    if (!frame) {
        h->front_valid = false;
        fill_px_range(px, (size_t)width * height, 0);
        grid_band_add(h, buf, (struct rect2i){
            .pos = { .x = 0, .y = origin },
            .size = { .x = width, .y = height } });
        return (struct render_damage){0, height};
    }
    glyph_cache_begin_frame(fi, frame->default_fg, frame->default_bg);
    int ch = m->cell_h;
    int rows = height / ch;
    if (rows > frame->rows) rows = frame->rows;
    if (!renderer_reserve_rows(h, rows))
        return (struct render_damage){0, 0};
    int first = -1, last = -1;
    int cx = frame->cursor_x, cy = frame->cursor_y;
    int style = (int)frame->cursor_style;
    bool full = !terminal_frame_valid(frame) || h->buf_w[buf] != width || h->buf_h[buf] != height ||
                h->buf_term_size[buf].x != frame->cols || h->buf_term_size[buf].y != frame->rows ||
                h->buf_font_gen[buf] != font_gen ||
                h->buf_fg[buf] != frame->default_fg || h->buf_bg[buf] != frame->default_bg ||
                h->buf_cursor_color[buf] != frame->cursor_color ||
                h->buf_reverse_video[buf] != frame->reverse_video ||
                h->buf_full_gen[buf] != frame->full_gen;
    bool front_full = !h->front_valid || h->front_w != width ||
                      h->front_h != height || h->front_origin != origin ||
                      h->front_term_size.x != frame->cols || h->front_term_size.y != frame->rows ||
                      h->front_font_gen != font_gen ||
                      h->front_fg != frame->default_fg ||
                      h->front_bg != frame->default_bg ||
                      h->front_cursor_color != frame->cursor_color ||
                      h->front_reverse_video != frame->reverse_video ||
                      h->front_full_gen != frame->full_gen;
    int k = frame->scrolled;
    int matches = 0;
    if (!full && k > 0 && k < rows)
        for (int row = 0; row < rows - k; row++)
            matches += h->buf_rows[buf][row + k].gen == terminal_frame_row_key(frame, row);
    if (!full && k > 0 && k < rows && matches * 2 > rows - k) {
        size_t shift_px = (size_t)k * ch * width;
        size_t keep_px = (size_t)(rows - k) * ch * width;
        memmove(px, px + shift_px, keep_px * sizeof(uint32_t));
        if (h->buf_cur_y[buf] >= 0)
            h->buf_cur_y[buf] = h->buf_cur_y[buf] >= k ? h->buf_cur_y[buf] - k : -1;
        memmove(&h->buf_rows[buf][0], &h->buf_rows[buf][k],
                (size_t)(rows - k) * sizeof(struct render_row));
        for (int row = rows - k; row < rows; row++) {
            h->buf_rows[buf][row].gen = 0;
            h->buf_rows[buf][row].sel_c0 = frame->cols;
            h->buf_rows[buf][row].sel_c1 = -1;
            h->buf_rows[buf][row].ui = true;
        }
        grid_band_add(h, buf, (struct rect2i){
            .pos = { .x = 0, .y = origin },
            .size = { .x = width, .y = rows * ch } });
        if (first < 0)
            first = 0;
        last = rows - 1;
        if (emit_run) {
            emit_run(emit_ctx, 0, rows * ch);
            emit_run = NULL;
        }
    }
    if (!full && rows > 3) {
        int changed = 0;
        for (int row = 0; row < rows; row++)
            changed += h->buf_rows[buf][row].gen != terminal_frame_row_key(frame, row);
        full = changed * 2 > rows;
    }
    if (full) {
        paint_grid_margins(m, frame, px, width, height);
        if (terminal_frame_valid(frame) && rows > 3 && !getenv("WSAP_NO_PARALLEL"))
            paint_rows_parallel(fi, frame, px, width, rows);
        else
            for (int row = 0; row < rows; row++)
                paint_row(fi, frame, px, width, row);
        for (int row = 0; row < rows; row++) {
            h->buf_rows[buf][row].gen = terminal_frame_row_key(frame, row);
            terminal_frame_row_selection(frame, row, frame->cols,
                                         &h->buf_rows[buf][row].sel_c0,
                                         &h->buf_rows[buf][row].sel_c1);
        }
        grid_band_add(h, buf, (struct rect2i){
            .pos = { .x = 0, .y = origin },
            .size = { .x = width, .y = height } });
    }
    bool moved = h->buf_cur_x[buf] != cx || h->buf_cur_y[buf] != cy || h->buf_cur_style[buf] != style;
    bool front_moved = h->front_cur_x != cx || h->front_cur_y != cy ||
                       h->front_cur_style != style;
    int right = paint_right_margin_x(m, frame, width);
    int paint_run = -1, emit_first = -1;
    for (int row = 0; !full && row <= rows; row++) {
        bool content = false, repaint = false;
        if (row < rows) {
            int sel_c0, sel_c1;
            terminal_frame_row_selection(frame, row, frame->cols,
                                         &sel_c0, &sel_c1);
            bool dirty_cursor = moved && (row == cy || row == h->buf_cur_y[buf]);
            bool buffer_changed = dirty_cursor ||
                h->buf_rows[buf][row].sel_c0 != sel_c0 ||
                h->buf_rows[buf][row].sel_c1 != sel_c1 ||
                h->buf_rows[buf][row].gen != terminal_frame_row_key(frame, row);
            content = !front_full &&
                ((front_moved && (row == cy || row == h->front_cur_y)) ||
                 h->front_rows[row].sel_c0 != sel_c0 ||
                 h->front_rows[row].sel_c1 != sel_c1 ||
                 h->front_rows[row].gen != terminal_frame_row_key(frame, row));
            repaint = buffer_changed || h->buf_rows[buf][row].ui;
        }
        if (repaint) {
            paint_row(fi, frame, px, width, row);
            fill_row_right_margin(m, frame, px, width, row, right);
            h->buf_rows[buf][row].gen = terminal_frame_row_key(frame, row);
            terminal_frame_row_selection(frame, row, frame->cols,
                                         &h->buf_rows[buf][row].sel_c0,
                                         &h->buf_rows[buf][row].sel_c1);
            if (paint_run < 0)
                paint_run = row;
        } else if (paint_run >= 0) {
            grid_band_add(h, buf, (struct rect2i){
                .pos = { .x = 0, .y = origin + paint_run * ch },
                .size = { .x = width, .y = (row - paint_run) * ch } });
            paint_run = -1;
        }
        if (content) {
            if (emit_first < 0)
                emit_first = row;
            if (first < 0)
                first = row;
            last = row;
        } else if (emit_first >= 0) {
            if (emit_run)
                emit_run(emit_ctx, emit_first * ch, (row - emit_first) * ch);
            emit_first = -1;
        }
    }
    if (!full && h->buf_bottom_ui[buf] && rows * ch < height) {
        fill_px_range(px + (size_t)rows * ch * width,
                      (size_t)(height - rows * ch) * width, frame->default_bg);
        grid_band_add(h, buf, (struct rect2i){
            .pos = { .x = 0, .y = origin + rows * ch },
            .size = { .x = width, .y = height - rows * ch } });
    }
    for (int row = 0; row < h->rows_cap; row++)
        h->buf_rows[buf][row].ui = false;
    h->buf_bottom_ui[buf] = false;
    h->buf_cur_x[buf] = cx; h->buf_cur_y[buf] = cy;
    h->buf_cur_style[buf] = style;
    h->buf_w[buf] = width; h->buf_h[buf] = height;
    h->buf_term_size[buf] = (struct vector2i){ .x = frame->cols, .y = frame->rows };
    h->buf_font_gen[buf] = font_gen;
    h->buf_fg[buf] = frame->default_fg; h->buf_bg[buf] = frame->default_bg;
    h->buf_cursor_color[buf] = frame->cursor_color;
    h->buf_reverse_video[buf] = frame->reverse_video;
    h->buf_full_gen[buf] = frame->full_gen;
    for (int row = 0; row < rows; row++) {
        h->front_rows[row].gen = terminal_frame_row_key(frame, row);
        terminal_frame_row_selection(frame, row, frame->cols,
                                     &h->front_rows[row].sel_c0,
                                     &h->front_rows[row].sel_c1);
    }
    h->front_w = width; h->front_h = height; h->front_origin = origin;
    h->front_term_size = (struct vector2i){ .x = frame->cols, .y = frame->rows };
    h->front_font_gen = font_gen;
    h->front_cur_x = cx; h->front_cur_y = cy;
    h->front_cur_style = style;
    h->front_fg = frame->default_fg; h->front_bg = frame->default_bg;
    h->front_cursor_color = frame->cursor_color;
    h->front_reverse_video = frame->reverse_video;
    h->front_full_gen = frame->full_gen;
    h->front_valid = true;
    if (front_full && !full && emit_run)
        emit_run(emit_ctx, 0, height);
    if (full || front_full) return (struct render_damage){0, height};
    return first < 0 ? (struct render_damage){0, 0} :
                      (struct render_damage){first * ch, (last - first + 1) * ch};
}

struct dmg_emit_ctx { struct framebuffer *t; int origin, width, n; };

static void emit_damage_run(void *ctx, int y, int h)
{
    struct dmg_emit_ctx *c = ctx;
    framebuffer_damage(c->t, (struct rect2i){
        .pos = { .x = 0, .y = c->origin + y },
        .size = { .x = c->width, .y = h },
    });
    c->n++;
}

static void draw_terminal_frame(struct renderer *r, struct renderer_history *h,
                                struct framebuffer *t,
                                const struct terminal_frame *frame)
{
    int width = t->size.x, height = t->size.y;
    int origin = frame && frame->origin_y > 0 ? frame->origin_y : 0;
    if (origin >= height)
        return;
    struct font_instance *fi = term_font(r);
    glyph_cache_refresh(fi);
    const struct font_metrics *m = font_instance_metrics(fi);
    const struct render_backend_view view = {
        .font = fi,
        .cols = frame ? frame->cols : 0,
        .rows = frame ? frame->rows : 0,
        .cell_w = (float)m->cell_w,
        .cell_h = (float)m->cell_h,
        .cell_size = sizeof(struct wsap_vtcore_cell),
        .row = terminal_frame_row,
        .row_selection = terminal_frame_row_selection,
        .resolve_cell = render_resolve_cell,
        .row_gen = terminal_frame_row_key,
        .font_version = font_instance_version,
        .fetch_glyph = glyph_cache_fetch,
        .shape_segment = glyph_cache_shape_segment,
        .cell_aligned_symbol = is_cell_aligned_symbol,
    };
    switch (t->gpu ? render_gpu_terminal(t->gpu, frame, &view) : RENDER_BACKEND_SKIP) {
    case RENDER_BACKEND_DREW:
        h->front_valid = false;
        framebuffer_damage_all(t);
        return;
    case RENDER_BACKEND_FAILED:
        h->front_valid = false;
        return;
    case RENDER_BACKEND_SKIP:
        break;
    }
    int buf = t->buffer;
    if (t->stale && (unsigned)buf < FRAMEBUFFER_BUFFERS)
        h->buf_w[buf] = 0;
    uint32_t *px = t->pixels;
    if (px)
        px += (size_t)origin * (size_t)width;
    struct dmg_emit_ctx ctx = { t, origin, width, 0 };
    struct render_damage d = renderer_paint_frame(h, fi, px,
                                                width, height - origin,
                                                buf, origin, frame,
                                                emit_damage_run, &ctx);
    if (!ctx.n)
        framebuffer_damage(t, (struct rect2i){
            .pos = { .x = 0, .y = d.y + origin },
            .size = { .x = d.height ? width : 0, .y = d.height },
        });
}

static void invalidate_terminal(struct renderer_history *h, struct renderer *r,
                                int buf,
                                struct rect2i terminal, struct rect2i in)
{
    int ch = font_instance_metrics(term_font(r))->cell_h;
    int y0 = in.y - terminal.y;
    int y1 = y0 + in.height;
    int grid_h = terminal.height / ch * ch;
    if (y1 > grid_h) {
        h->buf_bottom_ui[buf] = true;
        y1 = grid_h;
    }
    int row1 = (y1 + ch - 1) / ch;
    if (!renderer_reserve_rows(h, row1))
        return;
    for (int row = y0 / ch; row < row1; row++)
        h->buf_rows[buf][row].ui = true;
}

static void begin_ui_frame(struct renderer_history *h, struct renderer *r,
                           struct framebuffer *t,
                           const struct ui_damage *damage,
                           struct rect2i terminal, uint32_t base)
{
    if (!r || !t)
        return;
    h->ui_buf = -1;
    int buf = t->buffer;
    struct vector2i size = {
        .x = t->size.x, .y = t->size.y,
    };
    if ((unsigned)buf >= FRAMEBUFFER_BUFFERS || size.x <= 0 || size.y <= 0 ||
        t->cleared)
        return;

    struct ui_damage frame = damage ? *damage : (struct ui_damage){ .all = true };
    uint64_t f = ++h->ui_frame;
    h->ui_hist[f % FRAMEBUFFER_BUFFERS] = frame;
    uint64_t last = h->buf_ui_frame[buf];
    struct ui_damage region = {0};
    if (!last || f - last > FRAMEBUFFER_BUFFERS ||
        !vector2i_eq(h->buf_ui_size[buf], size) || t->stale)
        region.all = true;
    else
        for (uint64_t g = last + 1; g <= f; g++)
            ui_damage_merge(&region, &h->ui_hist[g % FRAMEBUFFER_BUFFERS]);
    h->buf_ui_frame[buf] = f;
    h->buf_ui_size[buf] = size;

    struct rect2i full = { .size = size };
    if (region.all) {
        h->buf_w[buf] = 0;
        region = (struct ui_damage){0};
        ui_damage_add(&region, full);
    }
    terminal = rect2i_intersect(terminal, full);
    struct ui_damage outside = {0};
    for (int i = 0; i < region.n; i++) {
        struct rect2i rr = rect2i_intersect(region.rects[i], full);
        if (rect2i_is_empty(rr))
            continue;
        struct rect2i in = rect2i_intersect(rr, terminal);
        if (!rect2i_is_empty(in))
            invalidate_terminal(h, r, buf, terminal, in);
        struct rect2i rest[4];
        int n = rect2i_minus(rr, terminal, rest);
        for (int k = 0; k < n; k++)
            ui_damage_add(&outside, rest[k]);
    }
    h->ui_repaint = outside;
    h->ui_frame_damage = frame;
    h->ui_base = base;
    h->ui_buf = buf;
}

static void fill_rect(uint32_t *px, int width, struct rect2i rr, uint32_t c)
{
    for (int y = rr.y; y < rr.y + rr.height; y++)
        fill_pixels32(px + (size_t)y * width + rr.x, c, rr.width);
}

static void render_ui_scene(struct renderer_history *h, struct renderer *r,
                            struct framebuffer *t,
                            const struct ui_scene *scene)
{
    if (!t)
        return;
    int buf = t->buffer;
    bool prepared = r && h->ui_buf >= 0 && h->ui_buf == buf;
    if (r)
        h->ui_buf = -1;
    if (!ui_scene_is_valid(scene)) {
        framebuffer_damage_all(t);
        return;
    }
    int width = t->size.x, height = t->size.y;
    uint32_t *px = t->pixels;
    glyph_cache_refresh(ui_font(r));
    if (t->gpu && width > 0 && height > 0 && render_gpu_ui_scene(t->gpu, scene, ui_font(r)))
        return;
    if (!px || width <= 0 || height <= 0) {
        framebuffer_damage_all(t);
        return;
    }
    struct vector2i limit = { .x = width, .y = height };
    if (!prepared) {
        ui_scene_paint_clipped(ui_font(r), px, width, height, scene,
                               (struct rect2i){ .size = limit });
        framebuffer_damage_all(t);
        return;
    }

    struct rect2i bounds = ui_scene_bounds(scene);
    for (int i = 0; i < h->buf_grid_bands_n[buf]; i++) {
        struct rect2i b = rect2i_intersect(h->buf_grid_bands[buf][i], bounds);
        if (rect2i_clip(&b, limit))
            ui_scene_paint_clipped(ui_font(r), px, width, height, scene, b);
    }
    h->buf_grid_bands_n[buf] = 0;
    for (int i = 0; i < h->ui_repaint.n; i++) {
        struct rect2i rr = h->ui_repaint.rects[i];
        if (!rect2i_clip(&rr, limit))
            continue;
        fill_rect(px, width, rr, h->ui_base);
        ui_scene_paint_clipped(ui_font(r), px, width, height, scene, rr);
    }

    framebuffer_damage(t, (struct rect2i){0});
    if (h->ui_frame_damage.all) {
        framebuffer_damage_all(t);
        return;
    }
    for (int i = 0; i < h->ui_frame_damage.n; i++) {
        struct rect2i dr = h->ui_frame_damage.rects[i];
        if (rect2i_clip(&dr, limit))
            framebuffer_damage(t, dr);
    }
}

static void draw_frame_layers(struct renderer *r, struct renderer_history *h,
                              struct framebuffer *fb,
                              const struct render_frame *frame)
{
    if (!frame->paint && !frame->has_grid) {
        if (fb->pixels && fb->size.x > 0 && fb->size.y > 0)
            fill_px_range(fb->pixels, (size_t)fb->size.x * fb->size.y,
                          frame->base);
        framebuffer_damage_all(fb);
        if ((unsigned)fb->buffer < FRAMEBUFFER_BUFFERS) {
            h->buf_w[fb->buffer] = 0;
            h->buf_ui_frame[fb->buffer] = 0;
        }
        h->front_valid = false;
        return;
    }
    if (frame->paint)
        begin_ui_frame(h, r, fb, frame->damage, frame->terminal, frame->base);
    if (frame->has_grid)
        draw_terminal_frame(r, h, fb, &frame->grid);
    if (frame->paint)
        render_ui_scene(h, r, fb, frame->paint);
}

struct damage_list render_draw(struct renderer *r, struct framebuffer *target,
                               const struct render_frame *frame)
{
    if (!target || !frame)
        return (struct damage_list){0};

    target->presented = false;
    struct framebuffer fb = *target;
    struct damage_list recorded = {0};
    if (!fb.damage)
        fb.damage = &recorded;
    struct renderer_history temporary;
    renderer_history_init(&temporary);
    struct renderer_history *h = renderer_history_get(target->history, &temporary);
    bool gpu_frame = false;
    if (fb.drawable && r && r->gpu) {
        gpu_frame = render_gpu_begin(r->gpu, &fb, frame->base);
        fb.gpu = gpu_frame && !frame->gpu_off ? r->gpu : NULL;
    } else if (frame->gpu_off) {
        fb.gpu = NULL;
    }
    if (r) {
        hold(&r->font, frame->font);
        hold(&r->ui_font, frame->ui_font);
        if (frame->repaint)
            renderer_repaint_all(h);
    }

    draw_frame_layers(r, h, &fb, frame);

    if (gpu_frame && render_gpu_redraw(r->gpu)) {
        fb.gpu = NULL;
        fb.stale = true;
        *fb.damage = (struct damage_list){0};
        renderer_repaint_all(h);
        draw_frame_layers(r, h, &fb, frame);
    }
    if (gpu_frame)
        target->presented = render_gpu_submit(r->gpu, &fb);

    struct damage_list damage = *fb.damage;
    if (h == &temporary)
        renderer_history_clear(h);
    return damage;
}
