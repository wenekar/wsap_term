#include "ui/paint.h"

#include "ui/internal/tree.h"

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
    bool clip = widget->ops && widget->ops->clip_children && widget->first_child;
    if (clip && !ui_paint_push_clip(&paint, ui_paint_bounds(&paint)))
        return;
    for (struct ui_widget *c = widget->first_child;
         c && ui_scene_ok(scene); c = c->next_sibling)
        paint_tree(c, scene);
    if (clip)
        ui_paint_pop_clip(&paint);
    if (widget->ops && widget->ops->paint_front)
        widget->ops->paint_front(widget, &paint);
    ui_scene_end_scope(scene);
}

static void clear_paint_invalidation(struct ui_widget *widget)
{
    if (!widget || widget->impl.destroyed)
        return;
    ui_widget_clear_invalidation(widget, UI_WIDGET_INVALIDATE_PAINT);
    for (struct ui_widget *c = widget->first_child; c; c = c->next_sibling)
        clear_paint_invalidation(c);
}

bool ui_widget_append_scene(const struct ui_widget *root,
                            struct ui_scene *scene)
{
    if (!scene)
        return false;
    paint_tree(root, scene);
    if (!ui_scene_ok(scene))
        return false;
    clear_paint_invalidation((struct ui_widget *)root);
    return true;
}

bool ui_widget_build_scene(const struct ui_widget *root,
                           struct ui_scene *scene)
{
    if (!scene)
        return false;
    ui_scene_init(scene);
    return ui_widget_append_scene(root, scene) && ui_scene_is_valid(scene);
}
