#include "render/scene_paint.h"

#include "font/glyph_cache.h"
#include "paint/blend.h"
#include "paint/scene.h"
#include "paint/shadow.h"
#include "paint/shape_math.h"
#include "paint/text_run.h"
#include "render/cpu/isa.h"
#include "render/cpu/kernels.h"
#include "render/cpu/pair_lut.h"
#include "util/geom.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * The scene painter of the new renderer.
 *
 * Rows go through the pixel kernels at the level that
 * render_isa_resolve(RENDER_ISA_AUTO) gives. The painter has no mutable
 * state except two caches: the kernel table (set once, the same value in
 * every thread) and a per-thread pair LUT cache.
 *
 * Link contract: this file and the files it uses may reference only
 * glyph_cache_fetch, glyph_cache_refresh, and the sRGB tables of the
 * outside world (tests/ui/stubs/scene_paint_deps.c).
 */

#define SCENE_CHUNK 256

_Static_assert((unsigned)UI_TEXT_BOLD == (unsigned)FONT_STYLE_BOLD &&
                   (unsigned)UI_TEXT_ITALIC == (unsigned)FONT_STYLE_ITALIC,
               "UI text bits must match font style bits");

/* The font style of UI text: its bold and italic bits. */
static inline uint16_t text_style(uint32_t ui_flags)
{
    return (uint16_t)(ui_flags & (UI_TEXT_BOLD | UI_TEXT_ITALIC));
}

static _Atomic(const struct render_kernels *) scene_kernels_cache;

static const struct render_kernels *scene_kernels(void)
{
    const struct render_kernels *k =
        atomic_load_explicit(&scene_kernels_cache, memory_order_acquire);
    if (!k) {
        /* Every thread computes the same table, so a race is harmless. */
        k = render_kernels_for(render_isa_resolve(RENDER_ISA_AUTO));
        atomic_store_explicit(&scene_kernels_cache, k, memory_order_release);
    }
    return k;
}

static _Thread_local struct pair_lut *scene_luts_tls;
static pthread_key_t scene_luts_key;
static pthread_once_t scene_luts_once = PTHREAD_ONCE_INIT;
static bool scene_luts_key_ok;

static void scene_luts_destroy(void *p)
{
    pair_lut_destroy(p);
    free(p);
}

static void scene_luts_key_init(void)
{
    scene_luts_key_ok = pthread_key_create(&scene_luts_key, scene_luts_destroy) == 0;
}

/* Allocates one pair LUT table per thread on first glyph span blend. */
static struct pair_lut *scene_luts_get(void)
{
    if (scene_luts_tls)
        return scene_luts_tls;
    pthread_once(&scene_luts_once, scene_luts_key_init);
    if (!scene_luts_key_ok)
        return NULL;
    struct pair_lut *luts = aligned_alloc(64, (sizeof(*luts) + 63) & ~(size_t)63);
    if (!luts)
        return NULL;
    memset(luts, 0, sizeof(*luts));
    if (pthread_setspecific(scene_luts_key, luts) != 0) {
        free(luts);
        return NULL;
    }
    scene_luts_tls = luts;
    return luts;
}

/* ---- Rects ----------------------------------------------------------- */

/* One run of one color with alpha a over the row. */
static void blend_run(const struct render_kernels *k, uint32_t *dst, int n,
                      uint32_t color, uint8_t a)
{
    if (n <= 0 || !a)
        return;
    if (a == 255)
        k->fill(dst, n, color | 0xFF000000u);
    else
        k->blend_rect(dst, n, color, a);
}

static void paint_rect(const struct render_kernels *k, uint32_t *px,
                       int width, int height, struct rect2i r,
                       struct rect2i clip, uint32_t color, int alpha)
{
    r = rect2i_intersect(r, clip);
    if (!rect2i_clip(&r, (struct vector2i){ .x = width, .y = height }))
        return;
    uint8_t a = alpha >= 0 ? (uint8_t)alpha : (uint8_t)(color >> 24);
    /* A blend rect has its alpha apart from its color. The color is opaque. */
    if (alpha >= 0)
        color |= 0xFF000000u;
    struct vector2i end = rect2i_end(r);
    for (int y = r.y; y < end.y; y++)
        blend_run(k, px + (size_t)y * width + r.x, end.x - r.x, color, a);
}

/* Scale a shape coverage by the alpha of the color. */
static inline uint8_t scale_cov(unsigned alpha, int cov)
{
    return cov == 255 ? (uint8_t)alpha
                      : (uint8_t)((alpha * (unsigned)cov + 127) / 255);
}

static void paint_rounded_rect(const struct render_kernels *k, uint32_t *px,
                               int width, int height, struct rect2i r,
                               int radius, struct rect2i clip, uint32_t color)
{
    if (radius <= 0) {
        paint_rect(k, px, width, height, r, clip, color, -1);
        return;
    }
    struct rect2i visible = rect2i_intersect(r, clip);
    if (!rect2i_clip(&visible, (struct vector2i){ .x = width, .y = height }))
        return;
    unsigned alpha = color >> 24;
    if (!alpha)
        return;
    if (radius > r.width / 2) radius = r.width / 2;
    if (radius > r.height / 2) radius = r.height / 2;

    int64_t ctl_x = (int64_t)r.x + radius, ctl_y = (int64_t)r.y + radius;
    int64_t ctr_x = (int64_t)r.x + r.width - radius - 1;
    int64_t cbl_y = (int64_t)r.y + r.height - radius - 1;
    struct vector2i end = rect2i_end(visible);
    int x0 = visible.x, x1 = end.x;
    /* The corner spans: [x0, left_end) and [right_start, x1). */
    int64_t left_end64 = ctl_x < x1 ? ctl_x : x1;
    int64_t band_start64 = ctl_x > x0 ? ctl_x : x0;
    int64_t band_end64 = ctr_x + 1 < x1 ? ctr_x + 1 : x1;
    int64_t right_start64 = ctl_x > ctr_x + 1 ? ctl_x : ctr_x + 1;
    if (right_start64 < x0) right_start64 = x0;
    int left_end = (int)left_end64, band_start = (int)band_start64;
    int band_end = (int)band_end64, right_start = (int)right_start64;
    uint32_t opaque = color | 0xFF000000u;
    uint8_t cov[SCENE_CHUNK];

    for (int y = visible.y; y < end.y; y++) {
        uint32_t *row = px + (size_t)y * width;
        if (y >= ctl_y && y <= cbl_y) {
            blend_run(k, row + x0, x1 - x0, color, (uint8_t)alpha);
            continue;
        }
        for (int i = 0; i < 2; i++) {
            int s = i ? right_start : x0;
            int e = i ? x1 : left_end;
            while (s < e) {
                int n = e - s < SCENE_CHUNK ? e - s : SCENE_CHUNK;
                for (int j = 0; j < n; j++)
                    cov[j] = scale_cov(alpha, shape_rrect_cov(
                        (float)(s + j), (float)y, (float)r.x, (float)r.y,
                        (float)r.width, (float)r.height, (float)radius));
                k->blend_any(row + s, cov, n, opaque);
                s += n;
            }
        }
        if (band_end > band_start)
            blend_run(k, row + band_start, band_end - band_start, color,
                      (uint8_t)alpha);
    }
}

static void paint_circle(const struct render_kernels *k, uint32_t *px,
                         int width, int height, struct rect2i bounds,
                         int radius, struct rect2i clip, uint32_t color,
                         uint8_t alpha)
{
    if (radius <= 0 || !alpha)
        return;
    struct rect2i visible = rect2i_intersect(bounds, clip);
    if (!rect2i_clip(&visible, (struct vector2i){ .x = width, .y = height }))
        return;
    float cx = (float)bounds.x + (float)radius - 0.5f;
    float cy = (float)bounds.y + (float)radius - 0.5f;
    struct vector2i end = rect2i_end(visible);
    uint32_t opaque = color | 0xFF000000u;
    uint8_t cov[SCENE_CHUNK];

    for (int y = visible.y; y < end.y; y++) {
        uint32_t *row = px + (size_t)y * width;
        for (int s = visible.x; s < end.x;) {
            int n = end.x - s < SCENE_CHUNK ? end.x - s : SCENE_CHUNK;
            for (int j = 0; j < n; j++)
                cov[j] = scale_cov(alpha, shape_circle_cov(
                    (float)(s + j), (float)y, cx, cy, (float)radius));
            k->blend_any(row + s, cov, n, opaque);
            s += n;
        }
    }
}

static void paint_shadow(const struct render_kernels *k, uint32_t *px,
                         int width, int height, struct rect2i painted,
                         int radius, int blur, struct rect2i clip,
                         uint32_t color)
{
    uint8_t base = (uint8_t)(color >> 24);
    const struct ui_shadow_mask *mask = ui_shadow_mask_shape(radius, blur);
    if (!base || !mask)
        return;
    struct rect2i box = rect2i_inset(painted, mask->extent);
    if (rect2i_is_empty(box))
        return;
    struct rect2i visible = rect2i_intersect(painted, clip);
    if (!rect2i_clip(&visible, (struct vector2i){ .x = width, .y = height }))
        return;

    struct vector2i end = rect2i_end(visible);
    uint32_t opaque = color | 0xFF000000u;
    uint8_t cov[SCENE_CHUNK];
    for (int y = visible.y; y < end.y; y++) {
        uint32_t *row = px + (size_t)y * width;
        for (int s = visible.x; s < end.x;) {
            int n = end.x - s < SCENE_CHUNK ? end.x - s : SCENE_CHUNK;
            for (int j = 0; j < n; j++) {
                uint8_t c = ui_shadow_coverage(
                    mask, box, (struct vector2i){ .x = s + j, .y = y });
                cov[j] = base == 255 ? c : scale_cov(base, c);
            }
            k->blend_any(row + s, cov, n, opaque);
            s += n;
        }
    }
}

/* ---- Text ------------------------------------------------------------ */

/* True if all n pixels are one opaque color. */
static bool uniform_opaque(const uint32_t *dst, int n)
{
    uint32_t c = dst[0];
    if ((c >> 24) != 255)
        return false;
    for (int i = 1; i < n; i++)
        if (dst[i] != c)
            return false;
    return true;
}

/* Alpha coverage over n pixels. cov is already scaled by the opacity. */
static void blend_alpha_span(const struct render_kernels *k, uint32_t *dst,
                             const uint8_t *cov, int n, uint32_t fg)
{
    if (uniform_opaque(dst, n)) {
        struct pair_lut *luts = scene_luts_get();
        const uint32_t *lut = luts ? pair_lut_alpha(luts, fg, dst[0], false) : NULL;
        if (lut) {
            k->compose_alpha_lut(dst, cov, n, lut);
            return;
        }
    }
    k->blend_any(dst, cov, n, fg);
}

static void blend_subpx_span(const struct render_kernels *k, uint32_t *dst,
                             const uint8_t *cov, int n, uint32_t fg)
{
    if (uniform_opaque(dst, n)) {
        uint32_t bg = dst[0];
        struct pair_lut *luts = scene_luts_get();
        const struct subpx_lut *lut = luts ? pair_lut_subpx(luts, fg, bg, false) : NULL;
        if (lut) {
            k->compose_subpx_lut(dst, cov, n, lut, bg);
            return;
        }
    }
    k->blend_any_subpx(dst, cov, n, fg);
}

static inline uint8_t scale_opacity(uint8_t c, unsigned opacity)
{
    return (uint8_t)(((unsigned)c * opacity + 127) / 255);
}

static void paint_glyph(const struct render_kernels *k, uint32_t *px,
                        int width, int height, int x0, int y0,
                        const struct glyph *g, uint32_t fg, int cell_h,
                        int glyph_rows, struct rect2i clip)
{
    if (!g || !g->width || cell_h <= 0)
        return;
    bool subpx = g->has_subpx_ink && g->subpx;
    if (!subpx && !g->alpha)
        return;
    int64_t gx = (int64_t)x0 + g->x_offset;
    int64_t sx = gx > clip.x ? gx : clip.x;
    int64_t ex = gx + g->width;
    if (ex > (int64_t)clip.x + clip.width) ex = (int64_t)clip.x + clip.width;
    if (ex > width) ex = width;
    if (sx < 0) sx = 0;
    if (sx >= ex)
        return;
    int n = (int)(ex - sx);
    int off = (int)(sx - gx);
    unsigned opacity = fg >> 24;
    int stride = subpx ? 3 : 1;
    int max_y = cell_h < glyph_rows ? cell_h : glyph_rows;
    uint8_t buf[SCENE_CHUNK * 3];

    for (int y = 0; y < max_y; y++) {
        int64_t py = (int64_t)y0 + y;
        if (py < clip.y || py >= (int64_t)clip.y + clip.height || py >= height)
            continue;
        uint32_t *row = px + (size_t)py * width + sx;
        const uint8_t *src = (subpx ? g->subpx[y] : g->alpha[y]) + off * stride;
        if (opacity == 255) {
            if (subpx)
                blend_subpx_span(k, row, src, n, fg);
            else
                blend_alpha_span(k, row, src, n, fg);
            continue;
        }
        if (!opacity)
            continue;
        for (int s = 0; s < n; s += SCENE_CHUNK) {
            int m = n - s < SCENE_CHUNK ? n - s : SCENE_CHUNK;
            for (int j = 0; j < m * stride; j++)
                buf[j] = scale_opacity(src[s * stride + j], opacity);
            if (subpx)
                blend_subpx_span(k, row + s, buf, m, fg);
            else
                blend_alpha_span(k, row + s, buf, m, fg);
        }
    }
}

static void paint_text(const struct render_kernels *k, struct font_instance *fi,
                       uint32_t *px, int width, int height,
                       const struct ui_scene *s, const struct ui_scene_op *op,
                       struct rect2i clip)
{
    int glyph_rows = font_instance_metrics(fi)->cell_h;
    const char *text = ui_scene_op_text(s, op);
    if (!text || op->style.advance <= 0 || op->style.cell_h <= 0)
        return;
    clip = rect2i_intersect(clip, ui_scene_text_ink(s, op));
    if (rect2i_is_empty(clip))
        return;
    uint16_t flags = text_style(op->style.flags);
    int64_t tx = op->rect.x;
    for (uint32_t i = 0; i < op->text_len;) {
        uint32_t cp;
        int cells;
        i += (uint32_t)ui_text_run_next(text + i, op->text_len - i, &cp, &cells);
        const struct glyph *g =
            tx + 382 <= clip.x ? NULL : glyph_cache_fetch(fi, cp, flags);
        int gx = tx > INT_MAX ? INT_MAX : tx < INT_MIN ? INT_MIN : (int)tx;
        paint_glyph(k, px, width, height, gx, op->rect.y, g, op->color,
                    op->style.cell_h, glyph_rows, clip);
        tx += (int64_t)cells * op->style.advance;
        if (tx >= (int64_t)clip.x + clip.width + 128)
            break;
    }
}

/* ---- Images ---------------------------------------------------------- */

/*
 * One axis of the sample rule of an image op. `pos` is the texel that holds
 * the sample point, and `frac` is the rest of the way to the next texel in
 * 1/65536 steps.
 */
struct image_tap {
    int32_t pos;
    int32_t frac;
};

/* The sample point of destination step `i` of `n` over `src` texels: 16.16. */
static int64_t image_coord(int i, int n, int src)
{
    return ((int64_t)(2 * i + 1) * src << 15) / n;
}

/* The texel that holds the sample point. */
static struct image_tap image_tap_nearest(int i, int n, int src)
{
    return (struct image_tap){ .pos = (int32_t)(image_coord(i, n, src) >> 16), .frac = 0 };
}

/*
 * The two texels of a linear sample. The centers of the texels sit at
 * half-texel steps, so the mix runs from the center to the next center.
 */
static struct image_tap image_tap_linear(int i, int n, int src)
{
    int64_t u = image_coord(i, n, src) - 32768;
    int64_t last = (int64_t)(src - 1) << 16;
    if (u < 0)
        u = 0;
    if (u > last)
        u = last;
    return (struct image_tap){ .pos = (int32_t)(u >> 16), .frac = (int32_t)(u & 0xffff) };
}

/* The 16.16 scale of a premultiplied channel: the tint and the opacity. */
static int32_t image_scale(uint32_t value, uint32_t alpha)
{
    /* (value / 255) * (alpha / 255) * 65536 */
    return (int32_t)((uint64_t)value * alpha * 65536 / (255u * 255u * 255u));
}

/* Scales a premultiplied channel of 0 to 255 by a 16.16 scale. */
static inline uint32_t image_scaled(uint32_t value, int32_t scale)
{
    return (value * (uint32_t)scale + 32768u) >> 16;
}

/* One channel of the premultiplied source over a straight backdrop. */
static inline uint32_t image_over_channel(uint32_t src, uint32_t bg, uint32_t inv)
{
    uint32_t v = src + (bg * inv + 127u) / 255u;
    return v > 255u ? 255u : v;
}

/* The premultiplied source over the straight color of the canvas. */
static inline uint32_t image_over(uint32_t src, uint32_t bg)
{
    uint32_t a = src >> 24;
    if (!a)
        return bg;
    uint32_t inv = 255u - a;
    uint32_t r = image_over_channel((src >> 16) & 0xff, (bg >> 16) & 0xff, inv);
    uint32_t g = image_over_channel((src >> 8) & 0xff, (bg >> 8) & 0xff, inv);
    uint32_t b = image_over_channel(src & 0xff, bg & 0xff, inv);
    uint32_t out_a = image_over_channel(a, bg >> 24, inv);
    return (out_a << 24) | (r << 16) | (g << 8) | b;
}

/* Mixes four premultiplied texels by their 16.16 weights. */
static inline uint32_t image_mix4(uint32_t t00, uint32_t t10, uint32_t t01, uint32_t t11,
                                  int32_t wx, int32_t wy)
{
    uint64_t sx = (uint64_t)(uint32_t)(65536 - wx), sy = (uint64_t)(uint32_t)(65536 - wy);
    uint64_t w00 = sx * sy, w10 = (uint64_t)(uint32_t)wx * sy;
    uint64_t w01 = sx * (uint64_t)(uint32_t)wy, w11 = (uint64_t)(uint32_t)wx * (uint64_t)(uint32_t)wy;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint64_t v = (uint64_t)((t00 >> shift) & 0xff) * w00 +
                     (uint64_t)((t10 >> shift) & 0xff) * w10 +
                     (uint64_t)((t01 >> shift) & 0xff) * w01 +
                     (uint64_t)((t11 >> shift) & 0xff) * w11;
        out |= (uint32_t)((v + (1ull << 31)) >> 32) << shift;
    }
    return out;
}

/*
 * Draws the texels of the image of `op` into its rect, over the canvas.
 * The op scales, tints, and clips the copy.
 */
static bool paint_image(uint32_t *px, int width, int height, const struct ui_scene_op *op,
                        struct rect2i clip)
{
    const struct ui_image *image = op->image.src;
    if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
        return true;
    struct rect2i dst = op->rect, uv = op->image.uv;
    struct rect2i r = rect2i_intersect(dst, clip);
    if (!rect2i_clip(&r, (struct vector2i){ .x = width, .y = height }))
        return true;
    if (dst.width <= 0 || dst.height <= 0)
        return true;

    /* A zero tint is white, and an opacity of 0 or 255 keeps the image. */
    uint32_t tint = op->color ? op->color : 0xFFFFFFFFu;
    bool plain = op->alpha >= 255 && tint == 0xFFFFFFFFu;
    uint32_t tint_alpha = (tint >> 24) * op->alpha;
    int32_t scale_r = image_scale((tint >> 16) & 0xff, tint_alpha);
    int32_t scale_g = image_scale((tint >> 8) & 0xff, tint_alpha);
    int32_t scale_b = image_scale(tint & 0xff, tint_alpha);
    int32_t scale_a = (int32_t)((uint64_t)tint_alpha * 65536 / (255u * 255u));
    bool linear = (op->flags & UI_IMAGE_LINEAR) != 0;

    struct image_tap inline_taps[SCENE_CHUNK];
    struct image_tap *taps = inline_taps;
    if (r.width > SCENE_CHUNK) {
        taps = malloc((size_t)r.width * sizeof(*taps));
        if (!taps)
            return false;
    }
    for (int i = 0; i < r.width; i++) {
        int step = r.x - dst.x + i;
        taps[i] = linear ? image_tap_linear(step, dst.width, uv.width)
                         : image_tap_nearest(step, dst.width, uv.width);
    }

    int last_x = uv.x + uv.width - 1, last_y = uv.y + uv.height - 1;
    for (int j = 0; j < r.height; j++) {
        int step = r.y - dst.y + j;
        struct image_tap ty = linear ? image_tap_linear(step, dst.height, uv.height)
                                     : image_tap_nearest(step, dst.height, uv.height);
        int sy0 = uv.y + ty.pos, sy1 = sy0 < last_y ? sy0 + 1 : sy0;
        const uint32_t *row0 = image->pixels + (size_t)sy0 * image->size.x;
        const uint32_t *row1 = image->pixels + (size_t)sy1 * image->size.x;
        uint32_t *out = px + (size_t)(r.y + j) * width + r.x;
        for (int i = 0; i < r.width; i++) {
            int tx0 = uv.x + taps[i].pos, tx1 = tx0 < last_x ? tx0 + 1 : tx0;
            uint32_t texel = linear
                ? image_mix4(row0[tx0], row0[tx1], row1[tx0], row1[tx1],
                             taps[i].frac, ty.frac)
                : row0[tx0];
            if (!plain)
                texel = (image_scaled(texel >> 24, scale_a) << 24) |
                        (image_scaled((texel >> 16) & 0xff, scale_r) << 16) |
                        (image_scaled((texel >> 8) & 0xff, scale_g) << 8) |
                        image_scaled(texel & 0xff, scale_b);
            out[i] = image_over(texel, out[i]);
        }
    }
    if (taps != inline_taps)
        free(taps);
    return true;
}

/* ---- Walk ------------------------------------------------------------ */

static bool op_outside_clip(const struct ui_scene *s,
                            const struct ui_scene_op *op, struct rect2i clip)
{
    struct rect2i ink = op->kind == UI_SCENE_OP_TEXT
        ? ui_scene_text_ink(s, op) : op->rect;
    return rect2i_is_empty(rect2i_intersect(ink, clip));
}

bool ui_scene_paint_clipped(struct font_instance *fi, uint32_t *px, int width, int height,
                            const struct ui_scene *scene,
                            struct rect2i paint_clip)
{
    if (!px || !ui_scene_is_valid(scene) || width <= 0 || height <= 0)
        return false;

    if (!rect2i_clip(&paint_clip,
                     (struct vector2i){ .x = width, .y = height }))
        return true;

    const struct render_kernels *k = scene_kernels();
    struct rect2i inline_stack[16];
    struct rect2i *clip_stack = inline_stack;
    int clip_cap = (int)(sizeof(inline_stack) / sizeof(inline_stack[0]));
    if (scene->clips_cap > clip_cap) {
        clip_stack = malloc((size_t)scene->clips_cap * sizeof(*clip_stack));
        if (!clip_stack)
            return false;
        clip_cap = scene->clips_cap;
    }
    int clip_n = 0;
    struct rect2i clip = paint_clip;
    bool painted = true;

    for (int i = 0; i < scene->n_ops && painted; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        switch (op->kind) {
        case UI_SCENE_OP_PUSH_CLIP:
            if (clip_n < clip_cap)
                clip_stack[clip_n++] = clip;
            clip = rect2i_intersect(clip, op->rect);
            break;
        case UI_SCENE_OP_POP_CLIP:
            if (clip_n > 0)
                clip = clip_stack[--clip_n];
            break;
        case UI_SCENE_OP_RECT:
        case UI_SCENE_OP_BLEND_RECT:
            if (op_outside_clip(scene, op, clip)) break;
            paint_rect(k, px, width, height, op->rect, clip, op->color,
                       op->kind == UI_SCENE_OP_RECT ? -1 : op->alpha);
            break;
        case UI_SCENE_OP_ROUNDED_RECT:
            if (op_outside_clip(scene, op, clip)) break;
            paint_rounded_rect(k, px, width, height, op->rect, op->radius,
                               clip, op->color);
            break;
        case UI_SCENE_OP_CIRCLE:
            if (op_outside_clip(scene, op, clip)) break;
            paint_circle(k, px, width, height, op->rect, op->radius, clip,
                         op->color, op->alpha);
            break;
        case UI_SCENE_OP_SHADOW:
            if (op_outside_clip(scene, op, clip)) break;
            paint_shadow(k, px, width, height, op->rect, op->radius,
                         op->blur, clip, op->color);
            break;
        case UI_SCENE_OP_TEXT:
            if (op_outside_clip(scene, op, clip)) break;
            paint_text(k, fi, px, width, height, scene, op, clip);
            break;
        case UI_SCENE_OP_IMAGE:
            if (op_outside_clip(scene, op, clip)) break;
            painted = paint_image(px, width, height, op, clip);
            break;
        }
    }
    if (clip_stack != inline_stack)
        free(clip_stack);
    return painted;
}

bool ui_scene_paint(struct font_instance *fi, uint32_t *px, int width, int height,
                    const struct ui_scene *scene)
{
    glyph_cache_refresh(fi);
    return ui_scene_paint_clipped(fi, px, width, height, scene,
        (struct rect2i){ .size = { .x = width, .y = height } });
}
