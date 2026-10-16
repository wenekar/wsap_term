#ifndef WSAP_UI_UTIL_H
#define WSAP_UI_UTIL_H

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "anim.h"
#include "css.h"
#include "paint/scene.h"
#include "util/geom.h"
#include "metrics.h"
#include "paint/text_run.h"

#include <string.h>

static inline struct css_scale_context ui_scale_ctx_default(void)
{
    return (struct css_scale_context){
        .dpi_scale = 1.0f,
        .cell_w = (float)UI_DEFAULT_CELL_W,
        .cell_h = (float)UI_DEFAULT_CELL_H,
    };
}

static inline float ui_positive_or(float v, float fallback)
{
    return isfinite(v) && v > 0.0f ? v : fallback;
}

static inline float ui_dpi_scale(float dpi_scale)
{
    return ui_positive_or(dpi_scale, 1.0f);
}

static inline int ui_px(float dpi_scale, int px)
{
    float v = (float)px * ui_dpi_scale(dpi_scale);
    return v > 0.0f && v < (float)INT_MAX ? (int)lroundf(v) : px;
}

static inline int ui_ctx_px(const struct css_scale_context *ctx, int px)
{
    return ui_px(ctx ? ctx->dpi_scale : 0.0f, px);
}

static inline bool ui_set_if_positive(float *dst, float v)
{
    if (!dst || !isfinite(v) || v <= 0.0f)
        return false;
    *dst = v;
    return true;
}

static inline void ui_scale_ctx_set(struct css_scale_context *ctx,
                                    float dpi_scale, float cell_w, float cell_h)
{
    if (!ctx)
        return;
    ui_set_if_positive(&ctx->dpi_scale, dpi_scale);
    ui_set_if_positive(&ctx->cell_w, cell_w);
    ui_set_if_positive(&ctx->cell_h, cell_h);
}

static inline uint32_t ui_color_opacity(uint32_t color, float opacity)
{
    if (!isfinite(opacity) || opacity <= 0.0f)
        return color & 0x00FFFFFFu;
    if (opacity >= 1.0f)
        return color;
    uint32_t a = (uint32_t)(color >> 24);
    a = (uint32_t)((float)a * opacity + 0.5f);
    return (color & 0x00FFFFFFu) | (a << 24);
}

static inline int ui_float_to_int(float v, bool nonnegative)
{
    if (!isfinite(v))
        return 0;
    if (nonnegative && v < 0.0f)
        return 0;
    if (v >= (float)INT_MAX)
        return INT_MAX;
    if (v <= (float)INT_MIN)
        return nonnegative ? 0 : INT_MIN;
    return (int)v;
}

static inline int ui_css_resolve_int(struct css_value v, float parent_dim,
                                     float auto_val,
                                     const struct css_scale_context *ctx,
                                     bool nonnegative)
{
    return ui_float_to_int(css_value_resolve(v, parent_dim, auto_val, ctx),
                           nonnegative);
}

static inline struct insets2i ui_css_resolve_insets(struct css_box box,
                                                    struct vector2i basis,
                                                    const struct css_scale_context *ctx,
                                                    bool nonnegative)
{
    struct insets2i out = {0};
    for (int i = 0; i < 4; i++)
        out.v[i] = ui_css_resolve_int(box.v[i], (i & 1) ? (float)basis.x : (float)basis.y,
                                      0, ctx, nonnegative);
    return out;
}

static inline int ui_css_pos_int(struct css_value v, float fallback,
                                 const struct css_scale_context *ctx)
{
    float resolved = css_value_resolve(v, 0, fallback, ctx);
    if (!isfinite(resolved) || resolved <= 0.0f)
        return (int)fallback;
    return resolved < (float)INT_MAX ? (int)lroundf(resolved) : (int)fallback;
}

static inline int ui_css_nonneg(struct css_value v, float fallback,
                                const struct css_scale_context *ctx)
{
    int px = ui_css_pos_int(v, fallback, ctx);
    return px < 0 ? 0 : px;
}

static inline int ui_ctx_size(const struct css_scale_context *ctx,
                              struct css_value v, float fallback)
{
    return ui_css_pos_int(v, fallback, ctx);
}

static inline int ui_ctx_inset(const struct css_scale_context *ctx,
                               struct css_value v, float fallback)
{
    return ui_css_nonneg(v, fallback, ctx);
}

static inline struct insets2i ui_ctx_insets(const struct css_scale_context *ctx,
                                            struct css_box box, float fallback)
{
    struct insets2i out = {0};
    for (int i = 0; i < 4; i++)
        out.v[i] = ui_ctx_inset(ctx, box.v[i], fallback);
    return out;
}

static inline int ui_cell_dim(float cell, int fallback)
{
    return isfinite(cell) && cell >= 1.0f && cell < (float)INT_MAX
        ? (int)lroundf(cell) : fallback;
}

static inline bool ui_scene_ripple(struct ui_scene *scene, struct rect2i clip,
                                   const struct ui_ripple *ripple,
                                   uint64_t now_ms)
{
    int radius;
    uint8_t alpha;
    if (!now_ms || !ripple->active ||
        !ui_ripple_eval(ripple, now_ms, &radius, &alpha))
        return true;
    return ui_scene_push_clip(scene, clip) &&
           ui_scene_circle(scene, ripple->origin, radius, ripple->color,
                           alpha) &&
           ui_scene_pop_clip(scene);
}

/* Copies text into dst (cap bytes with the NUL), cut at a UTF-8 boundary.
 * False when dst already held that text. */
static inline bool ui_text_copy(char *dst, size_t cap, const char *text)
{
    if (!text)
        text = "";
    size_t len = strlen(text);
    if (len > cap - 1) {
        size_t off = 0;
        while (off < len) {
            uint32_t cp;
            int cells;
            int n = ui_text_run_next(text + off, len - off, &cp, &cells);
            if (n <= 0 || off + (size_t)n > cap - 1)
                break;
            off += (size_t)n;
        }
        len = off;
    }
    if (memcmp(dst, text, len) == 0 && dst[len] == '\0')
        return false;
    memcpy(dst, text, len);
    dst[len] = '\0';
    return true;
}

/* The cell size text lays out in, from the style scale. */
static inline struct ui_text_style ui_widget_text_style(
    const struct css_scale_context *scale)
{
    return (struct ui_text_style){
        .advance = ui_cell_dim(scale->cell_w, UI_DEFAULT_CELL_W),
        .cell_h = ui_cell_dim(scale->cell_h, UI_DEFAULT_CELL_H),
    };
}
#endif
