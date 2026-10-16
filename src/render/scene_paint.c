#include "render/scene_paint.h"

#include "paint/blend.h"
#include "font/glyph_cache.h"
#include "util/geom.h"
#include "paint/scene.h"
#include "paint/shadow.h"
#include "paint/text_run.h"
#include "render/text_style.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

static void paint_rect(uint32_t *px, int width, int height,
                       struct rect2i r, struct rect2i clip,
                       uint32_t color, int alpha)
{
    r = rect2i_intersect(r, clip);
    if (!rect2i_clip(&r, (struct vector2i){ .x = width, .y = height }))
        return;
    uint8_t a = alpha >= 0 ? (uint8_t)alpha : (uint8_t)(color >> 24);
    if (!a)
        return;
    if (alpha >= 0)
        color |= 0xFF000000u;
    struct vector2i end = rect2i_end(r);
    int x1 = end.x;
    int y1 = end.y;
    if (a == 255) {
        uint32_t opaque = color | 0xFF000000u;
        for (int y = r.y; y < y1; y++) {
            uint32_t *row = px + (size_t)y * width;
            for (int x = r.x; x < x1; x++)
                row[x] = opaque;
        }
        return;
    }

    int fg_l = (int)render_lum16(color);
    for (int y = r.y; y < y1; y++) {
        uint32_t *row = px + (size_t)y * width;
        int x = r.x;
        uint32_t last_bg = 0;
        uint32_t cached_blend = 0;
        bool have_blend = false;
        for (; x < x1; x++) {
            uint32_t bg = row[x];
            if (have_blend && bg == last_bg) {
                row[x] = cached_blend;
            } else {
                last_bg = bg;
                cached_blend = render_blend_px_lum(color, bg, fg_l, (int)render_lum16(bg), a);
                have_blend = true;
                row[x] = cached_blend;
            }
        }
    }
}

static void paint_rect_color_alpha(uint32_t *px, int width, int height,
                            struct rect2i r, struct rect2i clip,
                            uint32_t color)
{
    paint_rect(px, width, height, r, clip, color, -1);
}

static inline uint8_t corner_coverage(int64_t x, int64_t y, int64_t cx,
                                      int64_t cy, int64_t r)
{
    int64_t dx = x - cx, dy = y - cy;
    int64_t d2 = dx * dx + dy * dy;
    int64_t outer = r + 1;
    if (d2 <= (r - 1) * (r - 1))
        return 255;
    if (d2 >= outer * outer)
        return 0;
    float cov = (float)r + 0.5f - sqrtf((float)d2);
    if (cov <= 0.0f)
        return 0;
    if (cov >= 1.0f)
        return 255;
    return (uint8_t)(cov * 255.0f + 0.5f);
}

static void paint_rounded_rect(uint32_t *px, int width, int height,
                               struct rect2i r, int radius,
                               struct rect2i clip, uint32_t color)
{
    if (radius <= 0) {
        paint_rect_color_alpha(px, width, height, r, clip, color);
        return;
    }
    struct rect2i visible = rect2i_intersect(r, clip);
    if (!rect2i_clip(&visible, (struct vector2i){ .x = width, .y = height })) return;
    int64_t shape_x1 = (int64_t)r.x + r.width;
    int64_t shape_y1 = (int64_t)r.y + r.height;
    if (radius > r.width / 2) radius = r.width / 2;
    if (radius > r.height / 2) radius = r.height / 2;

    int64_t ctl_x = (int64_t)r.x + radius, ctl_y = (int64_t)r.y + radius;
    int64_t ctr_x = shape_x1 - radius - 1;
    int64_t cbl_y = shape_y1 - radius - 1;
    int64_t left_cx = ctl_x <= ctr_x ? ctl_x : ctr_x;
    int64_t right_cx = ctl_x <= ctr_x ? ctr_x : ctl_x;
    int64_t top_cy = ctl_y <= cbl_y ? ctl_y : cbl_y;
    int64_t bottom_cy = ctl_y <= cbl_y ? cbl_y : ctl_y;
    struct vector2i vis_end = rect2i_end(visible);
    int x0 = visible.x, y0 = visible.y;
    int x1 = vis_end.x, y1 = vis_end.y;

    for (int y = y0; y < y1; y++) {
        uint32_t *row = px + (size_t)y * width;
        uint8_t alpha = (uint8_t)(color >> 24);
        if (y >= ctl_y && y <= cbl_y) {
            for (int x = x0; x < x1; x++)
                row[x] = alpha == 255 ? color :
                    render_blend_px(color, row[x], alpha);
        } else {
            int64_t cy = (y < ctl_y) ? top_cy : bottom_cy;
            for (int x = x0; x < x1; x++) {
                if (x >= ctl_x && x <= ctr_x) {
                    row[x] = alpha == 255 ? color :
                        render_blend_px(color, row[x], alpha);
                } else {
                    int64_t cx = (x < ctl_x) ? left_cx : right_cx;
                    uint8_t cov = corner_coverage(x, y, cx, cy, radius);
                    if (cov) {
                        uint8_t a = cov == 255 ? alpha
                            : (uint8_t)(((uint32_t)alpha * cov + 127) / 255);
                        row[x] = a == 255 ? color :
                            render_blend_px(color, row[x], a);
                    }
                }
            }
        }
    }
}

/* painted is the box that casts the shadow, grown by the extent of the
 * blur. Every pixel is one read from the shadow mask and one blend. */
static void paint_shadow(uint32_t *px, int width, int height,
                         struct rect2i painted, int radius, int blur,
                         struct rect2i clip, uint32_t color)
{
    uint8_t base = (uint8_t)(color >> 24);
    const struct ui_shadow_mask *mask = ui_shadow_mask(radius, blur);
    if (!base || !mask)
        return;
    struct rect2i box = rect2i_inset(painted, mask->extent);
    if (rect2i_is_empty(box))
        return;
    struct rect2i visible = rect2i_intersect(painted, clip);
    if (!rect2i_clip(&visible, (struct vector2i){ .x = width, .y = height }))
        return;

    struct vector2i end = rect2i_end(visible);
    int fg_l = (int)render_lum16(color);
    for (int y = visible.y; y < end.y; y++) {
        uint32_t *row = px + (size_t)y * width;
        for (int x = visible.x; x < end.x; x++) {
            uint8_t cov = ui_shadow_coverage(
                mask, box, (struct vector2i){ .x = x, .y = y });
            if (!cov)
                continue;
            uint8_t a = base == 255 ? cov
                : (uint8_t)(((unsigned)base * cov + 127) / 255);
            if (!a)
                continue;
            row[x] = render_blend_px_lum(color, row[x], fg_l,
                                         (int)render_lum16(row[x]), a);
        }
    }
}

static void paint_circle(uint32_t *px, int width, int height,
                         struct rect2i bounds, int radius,
                         struct rect2i clip, uint32_t color, uint8_t alpha)
{
    if (radius <= 0) return;
    struct rect2i visible = rect2i_intersect(bounds, clip);
    if (!rect2i_clip(&visible, (struct vector2i){ .x = width, .y = height })) return;

    int64_t cx = (int64_t)bounds.x + radius;
    int64_t cy = (int64_t)bounds.y + radius;
    struct vector2i vis_end = rect2i_end(visible);
    int x0 = visible.x, y0 = visible.y;
    int x1 = vis_end.x, y1 = vis_end.y;

    int64_t r2_inner = (int64_t)(radius - 1) * (radius - 1);
    int64_t r2_outer = (int64_t)(radius + 1) * (radius + 1);

    for (int y = y0; y < y1; y++) {
        uint32_t *row = px + (size_t)y * width;
        int64_t dy = (int64_t)y - cy;
        int64_t dy2 = dy * dy;
        for (int x = x0; x < x1; x++) {
            int64_t dx = (int64_t)x - cx;
            int64_t d2 = dx * dx + dy2;
            if (d2 >= r2_outer) continue;

            uint8_t a = alpha;
            if (d2 > r2_inner) {
                float dist = sqrtf((float)d2);
                float cov = (float)radius + 0.5f - dist;
                if (cov <= 0.0f) continue;
                a = (uint8_t)((float)alpha * (cov >= 1.0f ? 1.0f : cov) + 0.5f);
            }
            row[x] = render_blend_px(color, row[x], a);
        }
    }
}

static void paint_glyph(uint32_t *px, int width, int height,
                        int x0, int y0, const struct glyph *g, uint32_t fg,
                        int cell_h, int glyph_rows, struct rect2i clip)
{
    if (!g || !g->width || cell_h <= 0) return;
    int cx1 = clip.x + clip.width;
    int cy1 = clip.y + clip.height;
    int max_y = cell_h < glyph_rows ? cell_h : glyph_rows;
    if (g->has_subpx_ink && g->subpx) {
        for (int y = 0; y < max_y; y++) {
            int64_t py = (int64_t)y0 + y;
            if (py < clip.y || py >= cy1 || py >= height) continue;
            uint32_t *row = px + (size_t)py * width;
            for (int x = 0; x < g->width; x++) {
                uint8_t sr = g->subpx[y][x * 3 + 0];
                uint8_t sg = g->subpx[y][x * 3 + 1];
                uint8_t sb = g->subpx[y][x * 3 + 2];
                uint8_t opacity = (uint8_t)(fg >> 24);
                if (opacity != 255) {
                    sr = (uint8_t)(((unsigned)sr * opacity + 127) / 255);
                    sg = (uint8_t)(((unsigned)sg * opacity + 127) / 255);
                    sb = (uint8_t)(((unsigned)sb * opacity + 127) / 255);
                }
                if (!sr && !sg && !sb) continue;
                int64_t dx = (int64_t)x0 + g->x_offset + x;
                if (dx < clip.x || dx >= cx1 || dx >= width) continue;
                row[dx] = render_blend_subpx(fg, row[dx], sr, sg, sb);
            }
        }
    } else if (g->alpha) {
        for (int y = 0; y < max_y; y++) {
            int64_t py = (int64_t)y0 + y;
            if (py < clip.y || py >= cy1 || py >= height) continue;
            uint32_t *row = px + (size_t)py * width;
            for (int x = 0; x < g->width; x++) {
                uint8_t a = g->alpha[y][x];
                uint8_t opacity = (uint8_t)(fg >> 24);
                if (opacity != 255)
                    a = (uint8_t)(((unsigned)a * opacity + 127) / 255);
                if (!a) continue;
                int64_t dx = (int64_t)x0 + g->x_offset + x;
                if (dx < clip.x || dx >= cx1 || dx >= width) continue;
                row[dx] = render_blend_px(fg, row[dx], a);
            }
        }
    }
}

static void paint_text(struct font_instance *fi, uint32_t *px, int width, int height,
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
    uint16_t flags = render_text_style(op->style.flags);
    int64_t tx = op->rect.x;
    for (uint32_t i = 0; i < op->text_len;) {
        uint32_t cp;
        int cells;
        i += (uint32_t)ui_text_run_next(text + i, op->text_len - i, &cp, &cells);
        const struct glyph *g =
            tx + 382 <= clip.x ? NULL : glyph_cache_fetch(fi, cp, flags);
        int gx = tx > INT_MAX ? INT_MAX : tx < INT_MIN ? INT_MIN : (int)tx;
        paint_glyph(px, width, height, gx, op->rect.y, g, op->color,
                    op->style.cell_h, glyph_rows, clip);
        tx += (int64_t)cells * op->style.advance;
        if (tx >= (int64_t)clip.x + clip.width + 128)
            break;
    }
}

static bool op_outside_clip(const struct ui_scene *s,
                            const struct ui_scene_op *op, struct rect2i clip)
{
    struct rect2i ink = op->kind == UI_SCENE_OP_TEXT
        ? ui_scene_text_ink(s, op) : op->rect;
    return rect2i_is_empty(rect2i_intersect(ink, clip));
}

static void paint_image(uint32_t *px, int width, int height, struct rect2i rect,
                        struct rect2i clip, const struct ui_image *image)
{
    if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
        return;
    struct rect2i img_rect = {
        .pos = rect.pos,
        .size = {
            .x = rect.width < image->size.x ? rect.width : image->size.x,
            .y = rect.height < image->size.y ? rect.height : image->size.y,
        },
    };
    struct rect2i r = rect2i_intersect(img_rect, clip);
    if (!rect2i_clip(&r, (struct vector2i){ .x = width, .y = height }))
        return;
    int sx = r.x - rect.x, sy = r.y - rect.y;
    for (int y = 0; y < r.height; y++)
        memcpy(px + (size_t)(r.y + y) * width + r.x,
               image->pixels + (size_t)(sy + y) * image->size.x + sx,
               (size_t)r.width * sizeof(*px));
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

    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        switch (op->kind) {
        case UI_SCENE_OP_PUSH_CLIP:
            /* clips_cap counts the clips open when the scene was built, which
             * is not the deepest nesting its ops contain. Do not push past the
             * stack. Keep narrowing the clip either way, so the painted area
             * stays correct; only the restore level is lost. */
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
            paint_rect(px, width, height, op->rect, clip,
                       op->color, op->kind == UI_SCENE_OP_RECT ? -1 : op->alpha);
            break;
        case UI_SCENE_OP_ROUNDED_RECT:
            if (op_outside_clip(scene, op, clip)) break;
            paint_rounded_rect(px, width, height, op->rect, op->radius,
                               clip, op->color);
            break;
        case UI_SCENE_OP_CIRCLE:
            if (op_outside_clip(scene, op, clip)) break;
            paint_circle(px, width, height, op->rect, op->radius,
                         clip, op->color, op->alpha);
            break;
        case UI_SCENE_OP_SHADOW:
            if (op_outside_clip(scene, op, clip)) break;
            paint_shadow(px, width, height, op->rect, op->radius,
                         op->blur, clip, op->color);
            break;
        case UI_SCENE_OP_TEXT:
            if (op_outside_clip(scene, op, clip)) break;
            paint_text(fi, px, width, height, scene, op, clip);
            break;
        case UI_SCENE_OP_IMAGE:
            if (op_outside_clip(scene, op, clip)) break;
            paint_image(px, width, height, op->rect, clip, op->image);
            break;
        }
    }
    if (clip_stack != inline_stack)
        free(clip_stack);
    return true;
}

bool ui_scene_paint(struct font_instance *fi, uint32_t *px, int width, int height,
                    const struct ui_scene *scene)
{
    glyph_cache_refresh(fi);
    return ui_scene_paint_clipped(fi, px, width, height, scene,
        (struct rect2i){ .size = { .x = width, .y = height } });
}
