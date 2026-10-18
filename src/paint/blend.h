#ifndef WSAP_PAINT_BLEND_H
#define WSAP_PAINT_BLEND_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

extern uint16_t srgb_to_linear[258];
extern uint8_t srgb_from_linear[65540];
extern _Atomic bool srgb_luts_ready;
void srgb_build_luts(void);

static inline void srgb_ensure_luts(void)
{
    if (!atomic_load_explicit(&srgb_luts_ready, memory_order_acquire))
        srgb_build_luts();
}

static inline unsigned blend_channel(unsigned fg, unsigned bg, unsigned a)
{
    fg &= 0xff;
    bg &= 0xff;
    srgb_ensure_luts();
    unsigned t = srgb_to_linear[fg] * a +
                 srgb_to_linear[bg] * (255 - a) + 127;
    unsigned d = (unsigned)(((uint64_t)t * 0x808081ull) >> 31);
    return srgb_from_linear[d];
}

static inline unsigned blend_lum16(uint32_t rgb)
{
    srgb_ensure_luts();
    return (2126u * srgb_to_linear[(rgb >> 16) & 255] +
            7152u * srgb_to_linear[(rgb >> 8) & 255] +
             722u * srgb_to_linear[rgb & 255] + 5000) / 10000;
}

static inline uint8_t srgb_coverage(int fg_s, int bg_s, uint8_t a)
{
    int mix = fg_s * a + bg_s * (255 - a);
    int i = (mix + 1 + (mix >> 8)) >> 8;
    int f = mix - i * 255;
    int lo = srgb_to_linear[i], hi = srgb_to_linear[i + 1];
    int blend_l = lo + (hi - lo) * f / 255;
    int bg_l = srgb_to_linear[bg_s];
    int diff = srgb_to_linear[fg_s] - bg_l;
    if (!diff)
        return a;
    int c = ((blend_l - bg_l) * 255 + diff / 2) / diff;
    if (c < 0) return 0;
    if (c > 255) return 255;
    return (uint8_t)c;
}

static inline uint8_t blend_correct_coverage(int fg_l, int bg_l, uint8_t a)
{
    int diff = fg_l - bg_l;
    if (diff > -66 && diff < 66)
        return a;
    srgb_ensure_luts();
    return srgb_coverage(srgb_from_linear[fg_l],
                         srgb_from_linear[bg_l], a);
}

static inline uint8_t blend_correct_channel(int fg_c, int bg_c, uint8_t cov)
{
    if (!cov || cov == 255)
        return cov;
    if (fg_c == bg_c)
        return cov;
    srgb_ensure_luts();
    int fg_l = srgb_to_linear[fg_c & 0xff];
    int bg_l = srgb_to_linear[bg_c & 0xff];
    int diff = fg_l - bg_l;
    if (diff > -66 && diff < 66)
        return cov;
    return srgb_coverage(fg_c & 0xff, bg_c & 0xff, cov);
}

static inline uint32_t blend_subpx(uint32_t fg, uint32_t bg,
                                   uint8_t subpx_r, uint8_t subpx_g, uint8_t subpx_b)
{
    if (!subpx_r && !subpx_g && !subpx_b)
        return bg;
    unsigned fg_r = (fg >> 16) & 0xff;
    unsigned fg_g = (fg >> 8) & 0xff;
    unsigned fg_b = fg & 0xff;
    unsigned bg_r = (bg >> 16) & 0xff;
    unsigned bg_g = (bg >> 8) & 0xff;
    unsigned bg_b = bg & 0xff;

    uint8_t ar = blend_correct_channel((int)fg_r, (int)bg_r, subpx_r);
    uint8_t ag = blend_correct_channel((int)fg_g, (int)bg_g, subpx_g);
    uint8_t ab = blend_correct_channel((int)fg_b, (int)bg_b, subpx_b);

    unsigned r = blend_channel(fg_r, bg_r, ar);
    unsigned g = blend_channel(fg_g, bg_g, ag);
    unsigned b = blend_channel(fg_b, bg_b, ab);

    unsigned bg_a = bg >> 24;
    unsigned max_a = subpx_r > subpx_g ? (subpx_r > subpx_b ? subpx_r : subpx_b)
                                       : (subpx_g > subpx_b ? subpx_g : subpx_b);
    unsigned out_a = bg_a == 255 ? 255u
        : max_a + (bg_a * (255u - max_a) + 127u) / 255u;
    return (out_a << 24) | (r << 16) | (g << 8) | b;
}

struct subpx_lut {
    uint32_t r[256];
    uint32_t g[256];
    uint32_t b[256];
};

static inline void blend_build_subpx_channel(uint32_t *out, int shift,
                                             unsigned fg_c, unsigned bg_c)
{
    uint32_t bg_s = (uint32_t)bg_c << shift;
    if (fg_c == bg_c) {
        for (int c = 0; c < 256; c++)
            out[c] = bg_s;
        return;
    }
    out[0] = bg_s;
    for (int c = 1; c < 255; c++) {
        uint8_t a = blend_correct_channel((int)fg_c, (int)bg_c, (uint8_t)c);
        out[c] = (uint32_t)blend_channel(fg_c, bg_c, a) << shift;
    }
    out[255] = (uint32_t)fg_c << shift;
}

static inline void blend_build_subpx_lut(struct subpx_lut *lut,
                                         uint32_t fg, uint32_t bg)
{
    blend_build_subpx_channel(lut->r, 16, (fg >> 16) & 0xff, (bg >> 16) & 0xff);
    blend_build_subpx_channel(lut->g, 8, (fg >> 8) & 0xff, (bg >> 8) & 0xff);
    blend_build_subpx_channel(lut->b, 0, fg & 0xff, bg & 0xff);
}

static inline uint32_t blend_subpx_lut(const struct subpx_lut *lut,
                                       uint32_t bg,
                                       uint8_t subpx_r, uint8_t subpx_g, uint8_t subpx_b)
{
    if (!subpx_r && !subpx_g && !subpx_b)
        return bg;
    unsigned bg_a = bg >> 24;
    unsigned rgb = lut->r[subpx_r] | lut->g[subpx_g] | lut->b[subpx_b];
    if (bg_a == 255)
        return 0xFF000000u | rgb;
    unsigned max_a = subpx_r > subpx_g ? (subpx_r > subpx_b ? subpx_r : subpx_b)
                                       : (subpx_g > subpx_b ? subpx_g : subpx_b);
    unsigned out_a = max_a + (bg_a * (255u - max_a) + 127u) / 255u;
    return (out_a << 24) | rgb;
}

static inline uint32_t blend_px_lum(uint32_t fg, uint32_t bg,
                                    int fg_l, int bg_l, uint8_t a)
{
    if (!a) return bg;
    if (a == 255) return fg;
    a = blend_correct_coverage(fg_l, bg_l, a);
    if (!a) return bg;
    if (a == 255) return fg;
    unsigned r = blend_channel((fg >> 16) & 0xff, (bg >> 16) & 0xff, a);
    unsigned g = blend_channel((fg >> 8) & 0xff, (bg >> 8) & 0xff, a);
    unsigned b = blend_channel(fg & 0xff, bg & 0xff, a);
    unsigned bg_a = bg >> 24;
    unsigned out_a = bg_a == 255 ? 255u
        : a + (bg_a * (255u - a) + 127u) / 255u;
    return (out_a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t blend_px(uint32_t fg, uint32_t bg, uint8_t a)
{
    return blend_px_lum(fg, bg, (int)blend_lum16(fg),
                        (int)blend_lum16(bg), a);
}

#endif
