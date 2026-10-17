#ifndef WSAP_PAINT_BLEND_H
#define WSAP_PAINT_BLEND_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

extern uint16_t render_srgb_to_linear[258];
extern uint8_t render_linear_to_srgb[65540];
extern _Atomic bool render_srgb_luts_ready;
void render_build_srgb_luts(void);

static inline void render_ensure_srgb_luts(void)
{
    if (!atomic_load_explicit(&render_srgb_luts_ready, memory_order_acquire))
        render_build_srgb_luts();
}

static inline unsigned render_blend_channel(unsigned fg, unsigned bg, unsigned a)
{
    fg &= 0xff;
    bg &= 0xff;
    render_ensure_srgb_luts();
    unsigned t = render_srgb_to_linear[fg] * a +
                 render_srgb_to_linear[bg] * (255 - a) + 127;
    unsigned d = (unsigned)(((uint64_t)t * 0x808081ull) >> 31);
    return render_linear_to_srgb[d];
}

static inline unsigned render_lum16(uint32_t rgb)
{
    render_ensure_srgb_luts();
    return (2126u * render_srgb_to_linear[(rgb >> 16) & 255] +
            7152u * render_srgb_to_linear[(rgb >> 8) & 255] +
             722u * render_srgb_to_linear[rgb & 255] + 5000) / 10000;
}

static inline uint8_t render_srgb_coverage(int fg_s, int bg_s, uint8_t a)
{
    int mix = fg_s * a + bg_s * (255 - a);
    int i = (mix + 1 + (mix >> 8)) >> 8;
    int f = mix - i * 255;
    int lo = render_srgb_to_linear[i], hi = render_srgb_to_linear[i + 1];
    int blend_l = lo + (hi - lo) * f / 255;
    int bg_l = render_srgb_to_linear[bg_s];
    int diff = render_srgb_to_linear[fg_s] - bg_l;
    if (!diff)
        return a;
    int c = ((blend_l - bg_l) * 255 + diff / 2) / diff;
    if (c < 0) return 0;
    if (c > 255) return 255;
    return (uint8_t)c;
}

static inline uint8_t render_correct_coverage(int fg_l, int bg_l, uint8_t a)
{
    int diff = fg_l - bg_l;
    if (diff > -66 && diff < 66)
        return a;
    render_ensure_srgb_luts();
    return render_srgb_coverage(render_linear_to_srgb[fg_l],
                                render_linear_to_srgb[bg_l], a);
}

static inline uint8_t render_correct_channel(int fg_c, int bg_c, uint8_t cov)
{
    if (!cov || cov == 255)
        return cov;
    if (fg_c == bg_c)
        return cov;
    render_ensure_srgb_luts();
    int fg_l = render_srgb_to_linear[fg_c & 0xff];
    int bg_l = render_srgb_to_linear[bg_c & 0xff];
    int diff = fg_l - bg_l;
    if (diff > -66 && diff < 66)
        return cov;
    return render_srgb_coverage(fg_c & 0xff, bg_c & 0xff, cov);
}

static inline uint32_t render_blend_subpx(uint32_t fg, uint32_t bg,
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

    uint8_t ar = render_correct_channel((int)fg_r, (int)bg_r, subpx_r);
    uint8_t ag = render_correct_channel((int)fg_g, (int)bg_g, subpx_g);
    uint8_t ab = render_correct_channel((int)fg_b, (int)bg_b, subpx_b);

    unsigned r = render_blend_channel(fg_r, bg_r, ar);
    unsigned g = render_blend_channel(fg_g, bg_g, ag);
    unsigned b = render_blend_channel(fg_b, bg_b, ab);

    unsigned bg_a = bg >> 24;
    unsigned max_a = subpx_r > subpx_g ? (subpx_r > subpx_b ? subpx_r : subpx_b)
                                       : (subpx_g > subpx_b ? subpx_g : subpx_b);
    unsigned out_a = bg_a == 255 ? 255u
        : max_a + (bg_a * (255u - max_a) + 127u) / 255u;
    return (out_a << 24) | (r << 16) | (g << 8) | b;
}

struct render_subpx_lut {
    uint32_t r[256];
    uint32_t g[256];
    uint32_t b[256];
};

static inline void render_build_subpx_lut(struct render_subpx_lut *lut,
                                          uint32_t fg, uint32_t bg)
{
    unsigned fg_r = (fg >> 16) & 0xff;
    unsigned fg_g = (fg >> 8) & 0xff;
    unsigned fg_b = fg & 0xff;
    unsigned bg_r = (bg >> 16) & 0xff;
    unsigned bg_g = (bg >> 8) & 0xff;
    unsigned bg_b = bg & 0xff;

    for (int c = 0; c < 256; c++) {
        uint8_t ar = render_correct_channel((int)fg_r, (int)bg_r, (uint8_t)c);
        uint8_t ag = render_correct_channel((int)fg_g, (int)bg_g, (uint8_t)c);
        uint8_t ab = render_correct_channel((int)fg_b, (int)bg_b, (uint8_t)c);
        lut->r[c] = (uint32_t)render_blend_channel(fg_r, bg_r, ar) << 16;
        lut->g[c] = (uint32_t)render_blend_channel(fg_g, bg_g, ag) << 8;
        lut->b[c] = (uint32_t)render_blend_channel(fg_b, bg_b, ab);
    }
}

static inline uint32_t render_blend_subpx_lut(const struct render_subpx_lut *lut,
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

static inline uint32_t render_blend_px_lum(uint32_t fg, uint32_t bg,
                                           int fg_l, int bg_l, uint8_t a)
{
    if (!a) return bg;
    if (a == 255) return fg;
    a = render_correct_coverage(fg_l, bg_l, a);
    if (!a) return bg;
    if (a == 255) return fg;
    unsigned r = render_blend_channel((fg >> 16) & 0xff, (bg >> 16) & 0xff, a);
    unsigned g = render_blend_channel((fg >> 8) & 0xff, (bg >> 8) & 0xff, a);
    unsigned b = render_blend_channel(fg & 0xff, bg & 0xff, a);
    unsigned bg_a = bg >> 24;
    unsigned out_a = bg_a == 255 ? 255u
        : a + (bg_a * (255u - a) + 127u) / 255u;
    return (out_a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t render_blend_px(uint32_t fg, uint32_t bg, uint8_t a)
{
    return render_blend_px_lum(fg, bg, (int)render_lum16(fg),
                               (int)render_lum16(bg), a);
}

#endif
