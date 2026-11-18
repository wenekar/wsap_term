#include "ui/paint.h"

#include "ui/internal/tree.h"
#include "ui/css.h"

bool ui_paint_shadow_style_of(const struct css_style *style,
                              const struct css_scale_context *scale,
                              int radius, struct ui_paint_box_style *out)
{
    if (!style || !out || !style->has_shadow)
        return false;
    out->shadow_offset = (struct vector2i){
        .x = ui_css_resolve_int(style->shadow_x, 0, 0, scale, false),
        .y = ui_css_resolve_int(style->shadow_y, 0, 0, scale, false),
    };
    out->shadow_blur = ui_css_resolve_int(style->shadow_blur, 0, 0, scale,
                                          true);
    out->shadow_color = style->shadow_color;
    (void)radius;
    return out->shadow_color >> 24;
}

bool ui_paint_box_shadow(struct ui_paint_context *paint, struct rect2i local,
                         const struct ui_paint_box_style *style)
{
    if (!paint || !style || !(style->shadow_color >> 24))
        return true;
    return ui_paint_shadow(paint,
                           rect2i_translate(local, style->shadow_offset),
                           style->radius, style->shadow_blur,
                           style->shadow_color);
}

struct ui_paint_box_style ui_paint_box_style_of(
    const struct css_style *style, const struct css_scale_context *scale)
{
    if (!style)
        return (struct ui_paint_box_style){0};
    float opacity = css_style_effective_opacity(style);
    struct ui_paint_box_style out = {
        .bg_color = style->has_bg_color
            ? ui_color_opacity(style->bg_color, opacity) : 0,
        .border_color = style->has_border_color
            ? ui_color_opacity(style->border_color, opacity) : 0,
        .border_width = ui_css_border_width(style, scale),
        .radius = ui_css_resolve_int(style->border_radius, 0, 0, scale, true),
    };
    ui_paint_shadow_style_of(style, scale, out.radius, &out);
    out.shadow_color = ui_color_opacity(out.shadow_color, opacity);
    return out;
}

bool ui_paint_box(struct ui_paint_context *paint, struct rect2i local,
                  const struct ui_paint_box_style *style,
                  struct rect2i *out_fill)
{
    if (out_fill)
        *out_fill = (struct rect2i){0};
    if (!paint || !style || !ui_scene_ok(paint->scene))
        return false;
    if (rect2i_is_empty(local))
        return true;
    int border = style->border_width > 0 ? style->border_width : 0;
    int radius = style->radius > 0 ? style->radius : 0;
    if (border > 0 && (style->border_color >> 24)) {
        bool ok = radius > 0
            ? ui_paint_rounded_rect(paint, local, radius, style->border_color)
            : ui_paint_rect_stroke(paint, local, border, style->border_color);
        if (!ok)
            return false;
    }
    struct rect2i fill = border > 0 ? rect2i_inset(local, border) : local;
    if (rect2i_is_empty(fill))
        return true;
    if (style->bg_color >> 24) {
        int fill_radius = radius > border ? radius - border : 0;
        if (!ui_paint_rounded_rect(paint, fill, fill_radius, style->bg_color))
            return false;
    }
    if (out_fill)
        *out_fill = fill;
    return true;
}

struct rect2i ui_paint_bounds(const struct ui_paint_context *paint)
{
    if (!paint)
        return (struct rect2i){0};
    return (struct rect2i){ .size = paint->size };
}

bool ui_paint_push_clip(struct ui_paint_context *paint, struct rect2i local)
{
    return paint && ui_scene_push_clip(paint->scene, local);
}

bool ui_paint_pop_clip(struct ui_paint_context *paint)
{
    return paint && ui_scene_pop_clip(paint->scene);
}

bool ui_paint_rect(struct ui_paint_context *paint, struct rect2i local,
                   uint32_t color)
{
    return paint && ui_scene_rect(paint->scene, local, color);
}

bool ui_paint_rect_stroke(struct ui_paint_context *paint, struct rect2i local,
                          int width, uint32_t color)
{
    return paint && ui_scene_rect_stroke(paint->scene, local, width, color);
}

bool ui_paint_rounded_rect(struct ui_paint_context *paint, struct rect2i local,
                           int radius, uint32_t color)
{
    return paint && ui_scene_rounded_rect(paint->scene, local, radius, color);
}

bool ui_paint_shadow(struct ui_paint_context *paint, struct rect2i local,
                     int radius, int blur, uint32_t color)
{
    return paint && ui_scene_shadow(paint->scene, local, radius, blur, color);
}

bool ui_paint_image(struct ui_paint_context *paint, struct vector2i local,
                    const struct ui_image *image)
{
    return paint && ui_scene_image(paint->scene, local, image);
}

bool ui_paint_text(struct ui_paint_context *paint, struct rect2i local,
                   struct ui_text_style style, uint32_t color,
                   const char *text, size_t len)
{
    return paint && ui_scene_text_run(paint->scene, local, style, color,
                                      text, len);
}

static bool visible_children(const struct ui_widget *widget)
{
    for (const struct ui_widget *c = widget->first_child; c;
         c = c->next_sibling)
        if (!c->impl.destroyed && !c->impl.hidden)
            return true;
    return false;
}

static void paint_tree(const struct ui_widget *widget, struct ui_scene *scene)
{
    if (!widget || widget->impl.destroyed || widget->impl.hidden || !scene ||
        !ui_scene_begin_node(scene, widget, widget->bounds.pos))
        return;
    struct ui_paint_context paint = {
        .scene = scene,
        .size = widget->bounds.size,
    };
    if (widget->ops && widget->ops->paint)
        widget->ops->paint(widget, &paint);
    enum css_overflow overflow = ui_css_overflow(&widget->style,
                                                CSS_OVERFLOW_VISIBLE);
    bool clip = ((widget->ops && widget->ops->clip_children) ||
                 overflow == CSS_OVERFLOW_HIDDEN ||
                 overflow == CSS_OVERFLOW_CLIP ||
                 overflow == CSS_OVERFLOW_SCROLL ||
                 overflow == CSS_OVERFLOW_AUTO) && visible_children(widget);
    if (clip && !ui_paint_push_clip(&paint, ui_paint_bounds(&paint)))
        return;
    for (struct ui_widget *c = widget->first_child;
         c && ui_scene_ok(scene); c = c->next_sibling)
        paint_tree(c, scene);
    if (clip)
        ui_paint_pop_clip(&paint);
    ui_scene_end_scope(scene);
}

bool ui_widget_append_scene(const struct ui_widget *root,
                            struct ui_scene *scene)
{
    if (!scene)
        return false;
    paint_tree(root, scene);
    return ui_scene_ok(scene);
}

bool ui_widget_build_scene(const struct ui_widget *root,
                           struct ui_scene *scene)
{
    if (!scene)
        return false;
    ui_scene_init(scene);
    return ui_widget_append_scene(root, scene) && ui_scene_is_valid(scene);
}
