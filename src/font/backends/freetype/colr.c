#define _GNU_SOURCE

#include "font/backends/freetype/internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_COLOR_H
#include FT_OUTLINE_H

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 13)

#define COLR_MAX_DEPTH 64
#define COLR_NODE_BUDGET 2048
#define COLR_CANVAS_MAX 1024
#define COLR_PIXEL_BUDGET ((size_t)8 * 1024 * 1024)

#define COLR_TRANSFORM_CASES \
    case FT_COLR_PAINTFORMAT_TRANSFORM: \
    case FT_COLR_PAINTFORMAT_TRANSLATE: \
    case FT_COLR_PAINTFORMAT_SCALE: \
    case FT_COLR_PAINTFORMAT_ROTATE: \
    case FT_COLR_PAINTFORMAT_SKEW

struct colr_mat {
    double xx, xy, yx, yy, dx, dy;
};

struct colr_ctx {
    FT_Face face;
    FT_Color *palette;
    uint32_t *img;
    double s26;
    FT_Pos ox26, oy26;
    unsigned palette_len;
    int w, h;
    unsigned budget;
    size_t px_budget;
};

static unsigned colr_muldiv255(unsigned v, unsigned f)
{
    return (v * f + 127) / 255;
}

static uint32_t colr_over(uint32_t d, uint32_t s)
{
    unsigned sa = s >> 24;
    if (!sa)
        return d;
    unsigned inv = 255 - sa;
    return ((sa + colr_muldiv255(d >> 24, inv)) << 24) |
           ((((s >> 16) & 255) + colr_muldiv255((d >> 16) & 255, inv)) << 16) |
           ((((s >> 8) & 255) + colr_muldiv255((d >> 8) & 255, inv)) << 8) |
           ((s & 255) + colr_muldiv255(d & 255, inv));
}

static uint32_t colr_scale_px(uint32_t px, unsigned a)
{
    return (colr_muldiv255(px >> 24, a) << 24) |
           (colr_muldiv255((px >> 16) & 255, a) << 16) |
           (colr_muldiv255((px >> 8) & 255, a) << 8) |
           colr_muldiv255(px & 255, a);
}

static uint32_t colr_resolve_color(const struct colr_ctx *ctx, FT_ColorIndex ci)
{
    FT_Color c = { 255, 255, 255, 255 };
    if (ci.palette_index != 0xFFFF) {
        if (!ctx->palette || ci.palette_index >= ctx->palette_len)
            return 0;
        c = ctx->palette[ci.palette_index];
    }
    unsigned a = (unsigned)c.alpha * (unsigned)ci.alpha / 0x4000u;
    return colr_scale_px(UINT32_C(0xFF000000) | ((uint32_t)c.red << 16) |
                         ((uint32_t)c.green << 8) | c.blue, a);
}

static uint32_t colr_gradient_avg_color(struct colr_ctx *ctx,
                                    const FT_ColorLine *line)
{
    FT_ColorStopIterator it = line->color_stop_iterator;
    FT_ColorStop stop;
    uint64_t a = 0, r = 0, g = 0, b = 0, n = 0;
    while (FT_Get_Colorline_Stops(ctx->face, &stop, &it)) {
        uint32_t c = colr_resolve_color(ctx, stop.color);
        a += c >> 24;
        r += (c >> 16) & 255;
        g += (c >> 8) & 255;
        b += c & 255;
        n++;
    }
    if (!n)
        return 0;
    return (uint32_t)(((a / n) << 24) | ((r / n) << 16) |
                      ((g / n) << 8) | (b / n));
}

static double colr_blend_ch(FT_Composite_Mode mode, double cb, double cs)
{
    switch (mode) {
    case FT_COLR_COMPOSITE_SCREEN:
        return cb + cs - cb * cs;
    case FT_COLR_COMPOSITE_OVERLAY:
        return cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    case FT_COLR_COMPOSITE_DARKEN:
        return cb < cs ? cb : cs;
    case FT_COLR_COMPOSITE_LIGHTEN:
        return cb > cs ? cb : cs;
    case FT_COLR_COMPOSITE_COLOR_DODGE:
        return cb >= 1 ? 1 : cs >= 1 ? 1 : fmin(1, cs / (1 - cb));
    case FT_COLR_COMPOSITE_COLOR_BURN:
        return cb <= 0 ? 0 : cs <= 0 ? 0 : 1 - fmin(1, (1 - cb) / cs);
    case FT_COLR_COMPOSITE_HARD_LIGHT:
        return cs <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    case FT_COLR_COMPOSITE_SOFT_LIGHT: {
        double d = cb <= 0.25 ? ((16 * cb - 12) * cb + 4) * cb : sqrt(cb);
        return cs <= 0.5 ? cb - (1 - 2 * cs) * cb * (1 - cb)
                         : cb + (2 * cs - 1) * (d - cb);
    }
    case FT_COLR_COMPOSITE_DIFFERENCE:
        return fabs(cb - cs);
    case FT_COLR_COMPOSITE_EXCLUSION:
        return cb + cs - 2 * cb * cs;
    case FT_COLR_COMPOSITE_MULTIPLY:
        return cb * cs;
    default:
        return cs;
    }
}

static double colr_min3(const double c[3])
{
    return fmin(c[0], fmin(c[1], c[2]));
}

static double colr_max3(const double c[3])
{
    return fmax(c[0], fmax(c[1], c[2]));
}

static double colr_sat(const double c[3])
{
    return colr_max3(c) - colr_min3(c);
}

static double colr_lum(const double c[3])
{
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
}

static void colr_set_lum(const double c[3], double l, double out[3])
{
    double d = l - colr_lum(c);
    for (int i = 0; i < 3; i++)
        out[i] = c[i] + d;
    double n = colr_min3(out), x = colr_max3(out);
    if (n < 0)
        for (int i = 0; i < 3; i++)
            out[i] = l + (out[i] - l) * l / (l - n);
    if (x > 1)
        for (int i = 0; i < 3; i++)
            out[i] = l + (out[i] - l) * (1 - l) / (x - l);
}

static void colr_set_sat(const double c[3], double s, double out[3])
{
    double n = colr_min3(c), x = colr_max3(c);
    for (int i = 0; i < 3; i++)
        out[i] = x > n ? (c[i] == x ? s :
                          c[i] == n ? 0 : (c[i] - n) * s / (x - n)) : 0;
}

static void colr_blend(FT_Composite_Mode mode, const double cb[3],
                       const double cs[3], double out[3])
{
    switch (mode) {
    case FT_COLR_COMPOSITE_HSL_HUE: {
        double t[3];
        colr_set_sat(cs, colr_sat(cb), t);
        colr_set_lum(t, colr_lum(cb), out);
        return;
    }
    case FT_COLR_COMPOSITE_HSL_SATURATION: {
        double t[3];
        colr_set_sat(cb, colr_sat(cs), t);
        colr_set_lum(t, colr_lum(cb), out);
        return;
    }
    case FT_COLR_COMPOSITE_HSL_COLOR:
        colr_set_lum(cs, colr_lum(cb), out);
        return;
    case FT_COLR_COMPOSITE_HSL_LUMINOSITY:
        colr_set_lum(cb, colr_lum(cs), out);
        return;
    default:
        for (int i = 0; i < 3; i++)
            out[i] = colr_blend_ch(mode, cb[i], cs[i]);
    }
}

static uint32_t colr_compose(uint32_t b, uint32_t s, FT_Composite_Mode mode)
{
    if (mode == FT_COLR_COMPOSITE_CLEAR)
        return 0;
    if (mode == FT_COLR_COMPOSITE_SRC)
        return s;
    if (mode == FT_COLR_COMPOSITE_DEST)
        return b;

    double sa = (s >> 24) / 255.0, ba = (b >> 24) / 255.0;

    if (mode == FT_COLR_COMPOSITE_PLUS) {
        unsigned ch[4] = {
            s & 255, (s >> 8) & 255, (s >> 16) & 255, s >> 24
        };
        const unsigned db[4] = {
            b & 255, (b >> 8) & 255, (b >> 16) & 255, b >> 24
        };
        for (int i = 0; i < 4; i++)
            ch[i] = ch[i] + db[i] > 255 ? 255 : ch[i] + db[i];
        return ((uint32_t)ch[3] << 24) | (ch[2] << 16) |
               (ch[1] << 8) | ch[0];
    }

    if (mode <= FT_COLR_COMPOSITE_XOR) {
        double fs, fb;
        switch (mode) {
        case FT_COLR_COMPOSITE_SRC_OVER:  fs = 1;      fb = 1 - sa; break;
        case FT_COLR_COMPOSITE_DEST_OVER: fs = 1 - ba; fb = 1;      break;
        case FT_COLR_COMPOSITE_SRC_IN:    fs = ba;     fb = 0;      break;
        case FT_COLR_COMPOSITE_DEST_IN:   fs = 0;      fb = sa;     break;
        case FT_COLR_COMPOSITE_SRC_OUT:   fs = 1 - ba; fb = 0;      break;
        case FT_COLR_COMPOSITE_DEST_OUT:  fs = 0;      fb = 1 - sa; break;
        case FT_COLR_COMPOSITE_SRC_ATOP:  fs = ba;     fb = 1 - sa; break;
        case FT_COLR_COMPOSITE_DEST_ATOP: fs = 1 - ba; fb = sa;     break;
        default:                          fs = 1 - ba; fb = 1 - sa; break;
        }
        unsigned a = (unsigned)lround((sa * fs + ba * fb) * 255);
        unsigned r = (unsigned)lround(((s >> 16) & 255) * fs +
                                    ((b >> 16) & 255) * fb);
        unsigned g = (unsigned)lround(((s >> 8) & 255) * fs +
                                    ((b >> 8) & 255) * fb);
        unsigned bl = (unsigned)lround((s & 255) * fs + (b & 255) * fb);
        return ((uint32_t)a << 24) | (r << 16) | (g << 8) | bl;
    }

    double ao = sa + ba * (1 - sa);
    if (ao <= 0)
        return 0;
    double cb[3] = {0}, cs[3] = {0}, bl[3];
    if (ba > 0) {
        cb[0] = ((b >> 16) & 255) / (255.0 * ba);
        cb[1] = ((b >> 8) & 255) / (255.0 * ba);
        cb[2] = (b & 255) / (255.0 * ba);
    }
    if (sa > 0) {
        cs[0] = ((s >> 16) & 255) / (255.0 * sa);
        cs[1] = ((s >> 8) & 255) / (255.0 * sa);
        cs[2] = (s & 255) / (255.0 * sa);
    }
    colr_blend(mode, cb, cs, bl);
    const double sp[3] = { ((s >> 16) & 255) / 255.0,
                     ((s >> 8) & 255) / 255.0, (s & 255) / 255.0 };
    const double bp[3] = { ((b >> 16) & 255) / 255.0,
                     ((b >> 8) & 255) / 255.0, (b & 255) / 255.0 };
    unsigned ch[3];
    for (int i = 0; i < 3; i++) {
        double co = (1 - ba) * sp[i] + (1 - sa) * bp[i] + sa * ba * bl[i];
        ch[i] = (unsigned)lround(fmin(1, fmax(0, co)) * 255);
    }
    return ((uint32_t)lround(ao * 255) << 24) |
           (ch[0] << 16) | (ch[1] << 8) | ch[2];
}

static struct colr_mat colr_mat_mul(struct colr_mat m, struct colr_mat t)
{
    struct colr_mat r;
    r.xx = m.xx * t.xx + m.xy * t.yx;
    r.xy = m.xx * t.xy + m.xy * t.yy;
    r.yx = m.yx * t.xx + m.yy * t.yx;
    r.yy = m.yx * t.xy + m.yy * t.yy;
    r.dx = m.xx * t.dx + m.xy * t.dy + m.dx;
    r.dy = m.yx * t.dx + m.yy * t.dy + m.dy;
    return r;
}

static double colr_fixed_to_double(FT_Fixed v)
{
    return v / 65536.0;
}

struct colr_mask {
    uint8_t *buf;
    int x, y, w, h;
};

static bool colr_rasterize_glyph_mask(struct colr_ctx *ctx, FT_UInt gid,
                            const struct colr_mat *m, struct colr_mask *mk)
{
    if (FT_Load_Glyph(ctx->face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING |
                      FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM))
        return false;
    FT_GlyphSlot slot = ctx->face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE || !slot->outline.n_points)
        return false;
    double mx[4] = { m->xx * ctx->s26 * 65536.0, m->xy * ctx->s26 * 65536.0,
                     m->yx * ctx->s26 * 65536.0, m->yy * ctx->s26 * 65536.0 };
    double mv[2] = { m->dx * ctx->s26 - ctx->ox26,
                     m->dy * ctx->s26 - ctx->oy26 };
    for (int i = 0; i < 4; i++)
        if (!(mx[i] > -2147483000.0 && mx[i] < 2147483000.0))
            return false;
    for (int i = 0; i < 2; i++)
        if (!(mv[i] > -2147483000.0 && mv[i] < 2147483000.0))
            return false;
    FT_Matrix t = {
        (FT_Fixed)lround(mx[0]), (FT_Fixed)lround(mx[1]),
        (FT_Fixed)lround(mx[2]), (FT_Fixed)lround(mx[3]),
    };
    FT_Outline_Transform(&slot->outline, &t);
    FT_Outline_Translate(&slot->outline,
                         (FT_Pos)lround(mv[0]), (FT_Pos)lround(mv[1]));
    FT_BBox cb;
    FT_Outline_Get_CBox(&slot->outline, &cb);
    int x0 = (int)(cb.xMin >> 6), y0 = (int)(cb.yMin >> 6);
    int x1 = (int)((cb.xMax + 63) >> 6), y1 = (int)((cb.yMax + 63) >> 6);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ctx->w) x1 = ctx->w;
    if (y1 > ctx->h) y1 = ctx->h;
    if (x1 <= x0 || y1 <= y0)
        return false;
    FT_Outline_Translate(&slot->outline, -(x0 << 6), -(y0 << 6));
    mk->w = x1 - x0;
    mk->h = y1 - y0;
    if ((size_t)mk->w * (size_t)mk->h > ctx->px_budget)
        return false;
    ctx->px_budget -= (size_t)mk->w * (size_t)mk->h;
    mk->buf = calloc((size_t)mk->w, (size_t)mk->h);
    if (!mk->buf)
        return false;
    FT_Bitmap bm = { (unsigned)mk->h, (unsigned)mk->w, mk->w, mk->buf,
                     256, FT_PIXEL_MODE_GRAY, 0, NULL };
    if (FT_Outline_Get_Bitmap(ft_lib, &slot->outline, &bm)) {
        free(mk->buf);
        mk->buf = NULL;
        return false;
    }
    mk->x = x0;
    mk->y = ctx->h - y1;
    return true;
}

static void colr_mask_blit(struct colr_ctx *ctx, const struct colr_mask *mk,
                           const uint32_t *src, uint32_t solid)
{
    for (int r = 0; r < mk->h; r++) {
        const uint8_t *mrow = mk->buf + (size_t)r * mk->w;
        const uint32_t *srow = src ? src + (size_t)(mk->y + r) * ctx->w + mk->x
                                   : NULL;
        uint32_t *drow = ctx->img + (size_t)(mk->y + r) * ctx->w + mk->x;
        for (int c = 0; c < mk->w; c++) {
            unsigned a = mrow[c];
            uint32_t sp = src ? srow[c] : solid;
            if (!a || !(sp >> 24))
                continue;
            drow[c] = colr_over(drow[c], colr_scale_px(sp, a));
        }
    }
}

static void colr_fill_all(struct colr_ctx *ctx, uint32_t color)
{
    size_t n = (size_t)ctx->w * ctx->h;
    for (size_t i = 0; i < n; i++)
        ctx->img[i] = colr_over(ctx->img[i], color);
}

static void colr_render_paint(struct colr_ctx *ctx, FT_OpaquePaint opaque,
                      struct colr_mat m, int depth);
static bool colr_flatten_paint_color(struct colr_ctx *ctx, FT_OpaquePaint opaque,
                             uint32_t *out, int depth);

static FT_OpaquePaint colr_sub_paint(const FT_COLR_Paint *p)
{
    switch (p->format) {
    case FT_COLR_PAINTFORMAT_TRANSFORM: return p->u.transform.paint;
    case FT_COLR_PAINTFORMAT_TRANSLATE: return p->u.translate.paint;
    case FT_COLR_PAINTFORMAT_SCALE:     return p->u.scale.paint;
    case FT_COLR_PAINTFORMAT_ROTATE:    return p->u.rotate.paint;
    case FT_COLR_PAINTFORMAT_SKEW:      return p->u.skew.paint;
    default: {
        FT_OpaquePaint none = { NULL, 0 };
        return none;
    }
    }
}

static bool colr_paint_get_matrix(const FT_COLR_Paint *p, struct colr_mat *t)
{
    switch (p->format) {
    case FT_COLR_PAINTFORMAT_TRANSFORM: {
        const FT_Affine23 *a = &p->u.transform.affine;
        *t = (struct colr_mat){ colr_fixed_to_double(a->xx), colr_fixed_to_double(a->xy),
                                colr_fixed_to_double(a->yx), colr_fixed_to_double(a->yy),
                                colr_fixed_to_double(a->dx), colr_fixed_to_double(a->dy) };
        return true;
    }
    case FT_COLR_PAINTFORMAT_TRANSLATE:
        *t = (struct colr_mat){ 1, 0, 0, 1, colr_fixed_to_double(p->u.translate.dx),
                                colr_fixed_to_double(p->u.translate.dy) };
        return true;
    case FT_COLR_PAINTFORMAT_SCALE: {
        double sx = colr_fixed_to_double(p->u.scale.scale_x);
        double sy = colr_fixed_to_double(p->u.scale.scale_y);
        double cx = colr_fixed_to_double(p->u.scale.center_x);
        double cy = colr_fixed_to_double(p->u.scale.center_y);
        *t = (struct colr_mat){ sx, 0, 0, sy, cx - sx * cx, cy - sy * cy };
        return true;
    }
    case FT_COLR_PAINTFORMAT_ROTATE: {
        double a = colr_fixed_to_double(p->u.rotate.angle) * M_PI;
        double cx = colr_fixed_to_double(p->u.rotate.center_x);
        double cy = colr_fixed_to_double(p->u.rotate.center_y);
        double c = cos(a), s = sin(a);
        *t = (struct colr_mat){ c, -s, s, c, cx - c * cx + s * cy,
                                cy - s * cx - c * cy };
        return true;
    }
    case FT_COLR_PAINTFORMAT_SKEW: {
        double tx = -tan(colr_fixed_to_double(p->u.skew.x_skew_angle) * M_PI);
        double ty = tan(colr_fixed_to_double(p->u.skew.y_skew_angle) * M_PI);
        double cx = colr_fixed_to_double(p->u.skew.center_x);
        double cy = colr_fixed_to_double(p->u.skew.center_y);
        *t = (struct colr_mat){ 1, tx, ty, 1, -tx * cy, -ty * cx };
        return true;
    }
    default:
        return false;
    }
}

static uint32_t *colr_sub_image(struct colr_ctx *ctx, FT_OpaquePaint opaque,
                                struct colr_mat m, int depth)
{
    size_t px = (size_t)ctx->w * ctx->h;
    if (px > ctx->px_budget)
        return NULL;
    uint32_t *img = calloc(px, sizeof(*img));
    if (!img)
        return NULL;
    ctx->px_budget -= px;
    struct colr_ctx sub = *ctx;
    sub.img = img;
    colr_render_paint(&sub, opaque, m, depth - 1);
    ctx->budget = sub.budget;
    ctx->px_budget = sub.px_budget;
    return img;
}

static void colr_fill_glyph(struct colr_ctx *ctx, FT_UInt gid,
                            FT_OpaquePaint fill, struct colr_mat m, int depth)
{
    uint32_t color;
    struct colr_mask mk;
    uint32_t *img = NULL;
    if (!colr_flatten_paint_color(ctx, fill, &color, depth)) {
        color = 0;
        img = colr_sub_image(ctx, fill, m, depth);
        if (!img)
            return;
    }
    if ((img || (color >> 24)) && colr_rasterize_glyph_mask(ctx, gid, &m, &mk)) {
        if (img)
            colr_mask_blit(ctx, &mk, img, 0);
        else
            colr_mask_blit(ctx, &mk, NULL, color);
        free(mk.buf);
    }
    free(img);
}

static void colr_render_paint(struct colr_ctx *ctx, FT_OpaquePaint opaque,
                      struct colr_mat m, int depth)
{
    if (depth <= 0 || !ctx->budget || !ctx->budget--)
        return;
    FT_COLR_Paint p;
    if (!FT_Get_Paint(ctx->face, opaque, &p))
        return;
    switch (p.format) {
    case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
        FT_LayerIterator it = p.u.colr_layers.layer_iterator;
        FT_OpaquePaint layer;
        while (ctx->budget && FT_Get_Paint_Layers(ctx->face, &it, &layer))
            colr_render_paint(ctx, layer, m, depth - 1);
        break;
    }
    case FT_COLR_PAINTFORMAT_GLYPH:
        colr_fill_glyph(ctx, p.u.glyph.glyphID, p.u.glyph.paint, m, depth - 1);
        break;
    case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
        FT_OpaquePaint sub = { NULL, 0 };
        if (FT_Get_Color_Glyph_Paint(ctx->face, p.u.colr_glyph.glyphID,
                                     FT_COLOR_NO_ROOT_TRANSFORM, &sub))
            colr_render_paint(ctx, sub, m, depth - 1);
        break;
    }
    COLR_TRANSFORM_CASES: {
        struct colr_mat t;
        if (colr_paint_get_matrix(&p, &t))
            colr_render_paint(ctx, colr_sub_paint(&p), colr_mat_mul(m, t), depth - 1);
        break;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE: {
        uint32_t *bd = colr_sub_image(ctx, p.u.composite.backdrop_paint,
                                      m, depth - 1);
        uint32_t *src = colr_sub_image(ctx, p.u.composite.source_paint,
                                     m, depth - 1);
        if (bd && src) {
            size_t n = (size_t)ctx->w * ctx->h;
            for (size_t i = 0; i < n; i++)
                ctx->img[i] = colr_over(ctx->img[i],
                    colr_compose(bd[i], src[i],
                                 p.u.composite.composite_mode));
        }
        free(bd);
        free(src);
        break;
    }
    default: {

        uint32_t c;
        if (colr_flatten_paint_color(ctx, opaque, &c, depth - 1) && (c >> 24))
            colr_fill_all(ctx, c);
        break;
    }
    }
}

static bool colr_flatten_paint_color(struct colr_ctx *ctx, FT_OpaquePaint opaque,
                             uint32_t *out, int depth)
{
    if (depth <= 0 || !ctx->budget || !ctx->budget--)
        return false;
    FT_COLR_Paint p;
    if (!FT_Get_Paint(ctx->face, opaque, &p))
        return false;
    switch (p.format) {
    case FT_COLR_PAINTFORMAT_SOLID:
        *out = colr_resolve_color(ctx, p.u.solid.color);
        return true;
    case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
        *out = colr_gradient_avg_color(ctx, &p.u.linear_gradient.colorline);
        return true;
    case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
        *out = colr_gradient_avg_color(ctx, &p.u.radial_gradient.colorline);
        return true;
    case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
        *out = colr_gradient_avg_color(ctx, &p.u.sweep_gradient.colorline);
        return true;
    COLR_TRANSFORM_CASES:
        return colr_flatten_paint_color(ctx, colr_sub_paint(&p), out, depth - 1);
    case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
        FT_LayerIterator it = p.u.colr_layers.layer_iterator;
        FT_OpaquePaint layer;
        uint32_t acc = 0;
        while (FT_Get_Paint_Layers(ctx->face, &it, &layer)) {
            uint32_t c;
            if (!colr_flatten_paint_color(ctx, layer, &c, depth - 1))
                return false;
            acc = colr_over(acc, c);
        }
        *out = acc;
        return true;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE: {
        uint32_t b, s;
        if (!colr_flatten_paint_color(ctx, p.u.composite.backdrop_paint, &b,
                              depth - 1) ||
            !colr_flatten_paint_color(ctx, p.u.composite.source_paint, &s,
                              depth - 1))
            return false;
        *out = colr_compose(b, s, p.u.composite.composite_mode);
        return true;
    }
    default:
        return false;
    }
}

bool colr_rasterize(FT_Face face, uint32_t gid, FT_BitmapGlyph bg)
{
    FT_OpaquePaint root = { NULL, 0 };
    if (!FT_Get_Color_Glyph_Paint(face, gid, FT_COLOR_NO_ROOT_TRANSFORM,
                                  &root))
        return false;

    struct colr_ctx ctx = { .face = face };
    FT_Palette_Data pd;
    if (FT_Palette_Data_Get(face, &pd) == 0) {
        ctx.palette_len = pd.num_palette_entries;
        FT_Palette_Select(face, 0, &ctx.palette);
    }

    int ppem = (int)face->size->metrics.y_ppem;
    int upem = (int)face->units_per_EM;
    if (ppem < 1 || upem < 1)
        return false;
    int pem = ppem * 8;
    if (pem < 64)
        pem = 64;
    if (pem > 256)
        pem = 256;

    double rx0 = 0, ry0 = 0, rx1 = pem, ry1 = pem;
    FT_ClipBox cb;
    if (FT_Get_Color_Glyph_ClipBox(face, gid, &cb)) {
        double k = (double)pem / (ppem * 64.0);
        const double xs[4] = { cb.bottom_left.x * k, cb.top_left.x * k,
                         cb.top_right.x * k, cb.bottom_right.x * k };
        const double ys[4] = { cb.bottom_left.y * k, cb.top_left.y * k,
                         cb.top_right.y * k, cb.bottom_right.y * k };
        rx0 = rx1 = xs[0];
        ry0 = ry1 = ys[0];
        for (int i = 1; i < 4; i++) {
            rx0 = fmin(rx0, xs[i]);
            rx1 = fmax(rx1, xs[i]);
            ry0 = fmin(ry0, ys[i]);
            ry1 = fmax(ry1, ys[i]);
        }
    }
    if (!(rx0 > -32768.0 && rx1 < 32768.0 && ry0 > -32768.0 &&
          ry1 < 32768.0))
        return false;
    int x0 = (int)floor(rx0), y0 = (int)floor(ry0);
    int x1 = (int)ceil(rx1), y1 = (int)ceil(ry1);
    ctx.w = x1 - x0;
    ctx.h = y1 - y0;
    if (ctx.w < 1 || ctx.h < 1 || ctx.w > COLR_CANVAS_MAX ||
        ctx.h > COLR_CANVAS_MAX)
        return false;
    ctx.s26 = 64.0 * pem / upem;
    ctx.ox26 = (FT_Pos)x0 * 64;
    ctx.oy26 = (FT_Pos)y0 * 64;
    ctx.img = calloc((size_t)ctx.w * ctx.h, sizeof(*ctx.img));
    if (!ctx.img)
        return false;
    ctx.budget = COLR_NODE_BUDGET;
    ctx.px_budget = COLR_PIXEL_BUDGET;
    colr_render_paint(&ctx, root, (struct colr_mat){ 1, 0, 0, 1, 0, 0 },
              COLR_MAX_DEPTH);

    bool ink = false;
    for (size_t i = 0, n = (size_t)ctx.w * ctx.h; i < n && !ink; i++)
        ink = (ctx.img[i] >> 24) != 0;
    if (!ink) {
        free(ctx.img);
        return false;
    }

    free(bg->bitmap.buffer);
    bg->bitmap.rows = (unsigned)ctx.h;
    bg->bitmap.width = (unsigned)ctx.w;
    bg->bitmap.pitch = ctx.w * 4;
    bg->bitmap.buffer = (unsigned char *)ctx.img;
    bg->bitmap.num_grays = 0;
    bg->bitmap.pixel_mode = FT_PIXEL_MODE_BGRA;
    bg->bitmap.palette_mode = 0;
    bg->bitmap.palette = NULL;
    bg->left = x0;
    bg->top = y1;
    return true;
}

#else

bool colr_rasterize(FT_Face face, uint32_t gid, FT_BitmapGlyph bg)
{
    (void)face; (void)gid; (void)bg;
    return false;
}

#endif
