#include "ui/widgets/icon.h"

#include "ui/css.h"
#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/text_break.h"
#include "ui/widget_ops.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static struct ui_icon *icon_of(struct ui_widget *widget)
{
    return ui_container_of(widget, struct ui_icon, widget);
}

static struct vector2i icon_measure(const struct ui_widget *widget,
                                    struct ui_constraints constraints)
{
    const struct ui_icon *icon = widget
        ? ui_container_of_const(widget, struct ui_icon, widget) : NULL;
    if (!icon)
        return (struct vector2i){0};
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    struct vector2i extent = insets2i_extent(pad);
    int border = ui_widget_border_width(widget);
    int64_t width = 0, height = 0;
    if (icon->state.image.pixels && icon->state.image.size.x > 0 &&
        icon->state.image.size.y > 0) {
        width = icon->state.image.size.x;
        height = icon->state.image.size.y;
    } else {
        struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
        const char *label = ui_icon_label(icon);
        width = (int64_t)ui_text_run_cells(label, strlen(label)) * style.advance;
        height = style.cell_h;
    }
    struct vector2i natural = {
        .x = geom_sat_i(width + extent.x + 2 * (int64_t)border),
        .y = geom_sat_i(height + extent.y + 2 * (int64_t)border),
    };
    return ui_constraints_clamp(constraints, natural);
}

static void icon_paint(const struct ui_widget *widget,
                       struct ui_paint_context *paint)
{
    const struct ui_icon *icon = widget
        ? ui_container_of_const(widget, struct ui_icon, widget) : NULL;
    if (!icon || !paint)
        return;
    struct rect2i bounds = ui_paint_bounds(paint);
    struct ui_paint_box_style box = ui_paint_box_style_of(&widget->style,
                                                          &widget->style_scale);
    struct rect2i content = bounds;
    struct rect2i fill;
    if (ui_paint_box(paint, bounds, &box, &fill))
        content = fill;
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    content = rect2i_apply_insets(content, pad);
    if (rect2i_is_empty(content))
        return;
    if (icon->state.image.pixels && icon->state.image.size.x > 0 &&
        icon->state.image.size.y > 0) {
        if (!ui_paint_push_clip(paint, content))
            return;
        ui_paint_image(paint, content.pos, &icon->state.image);
        ui_paint_pop_clip(paint);
        return;
    }
    const char *label = ui_icon_label(icon);
    if (!label[0])
        return;
    struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
    uint32_t fg = ui_color_opacity(
        widget->style.has_text_color ? widget->style.text_color : 0xFFFFFFFF,
        css_style_effective_opacity(&widget->style));
    int cap = style.advance > 0 ? content.width / style.advance : 0;
    size_t len = ui_text_fit(label, strlen(label), cap);
    if (!len)
        return;
    struct vector2i extent = {
        .x = (int)((int64_t)ui_text_run_cells(label, len) * style.advance),
        .y = style.cell_h,
    };
    ui_paint_text(paint, (struct rect2i){
        .pos = {
            .x = geom_sat_i((int64_t)content.x +
                            geom_half_up((int64_t)content.width - extent.x)),
            .y = geom_sat_i((int64_t)content.y +
                            geom_half_up((int64_t)content.height - extent.y)),
        },
        .size = extent,
    }, style, fg, label, len);
}

static bool icon_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_icon *icon = widget
        ? ui_container_of(widget, struct ui_icon, widget) : NULL;
    if (!icon || !event || !icon->state.action_id)
        return false;
    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        icon->state.pressed = false;
        return true;
    }
    if (event->kind == UI_EVENT_FOCUS_OUT) {
        icon->state.pressed = false;
        return false;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct ui_pointer_event *pointer = &event->data.pointer;
    bool inside = rect2i_contains(widget->bounds, pointer->pos);
    switch (pointer->kind) {
    case UI_POINTER_PRESS:
        if (!inside || pointer->button != 0)
            return false;
        /* The icon takes no focus. A click must not move the keyboard focus
         * away from the widget that has it. */
        icon->state.pressed = true;
        ui_widget_capture_pointer(widget);
        return true;
    case UI_POINTER_RELEASE: {
        if (pointer->button != 0)
            return false;
        ui_widget_release_pointer(widget);
        bool activate = inside && icon->state.pressed;
        icon->state.pressed = false;
        if (!activate)
            return false;
        ui_widget_notify(widget, (struct ui_action){
            .kind = UI_ACTION_ACTIVATE,
            .id = icon->state.action_id,
            .index = -1,
        });
        return true;
    }
    case UI_POINTER_NONE:
    case UI_POINTER_MOTION:
    case UI_POINTER_SCROLL:
    case UI_POINTER_LEAVE:
        return false;
    }
    return false;
}

static bool icon_accessibility(const struct ui_widget *widget,
                               struct ui_accessibility_node_spec *out)
{
    const struct ui_icon *icon = ui_container_of_const(widget, struct ui_icon,
                                                       widget);
    out->role = UI_ACCESSIBILITY_ROLE_IMAGE;
    const char *label = ui_icon_label(icon);
    out->name = label[0] ? label : NULL;
    out->states |= UI_ACCESSIBILITY_STATE_READ_ONLY;
    if (icon->state.action_id)
        out->actions |= UI_ACCESSIBILITY_ACTION_INVOKE;
    return true;
}

static void icon_destroy(struct ui_widget *widget)
{
    struct ui_icon *icon = icon_of(widget);
    free(icon->state.label);
    icon->state.label = NULL;
}

static const struct ui_widget_ops ICON_OPS = {
    .name = "icon",
    .measure = icon_measure,
    .event = icon_event,
    .paint = icon_paint,
    .destroy = icon_destroy,
    .accessibility = icon_accessibility,
};

void ui_icon_init(struct ui_icon *icon)
{
    if (!icon)
        return;
    *icon = (struct ui_icon){0};
    ui_widget_init(&icon->widget, &ICON_OPS, NULL);
    ui_widget_set_style_identity(&icon->widget, "icon", NULL, "icon");
    /* A press on the icon must not move the keyboard focus. */
    ui_widget_set_keeps_press_focus(&icon->widget, true);
}

void ui_icon_set_image(struct ui_icon *icon, const struct ui_image *image)
{
    if (!icon)
        return;
    ui_icon_widget(icon);
    struct ui_image next = image ? *image : (struct ui_image){0};
    if (!memcmp(&next, &icon->state.image, sizeof(next)))
        return;
    icon->state.image = next;
    ui_widget_invalidate(&icon->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

bool ui_icon_has_image(const struct ui_icon *icon)
{
    return icon && icon->state.image.pixels &&
           icon->state.image.size.x > 0 && icon->state.image.size.y > 0;
}

bool ui_icon_set_label(struct ui_icon *icon, const char *label)
{
    if (!icon)
        return false;
    ui_icon_widget(icon);
    if (!label)
        label = "";
    size_t len = strlen(label);
    if (len > INT_MAX)
        return false;
    if (!strcmp(ui_icon_label(icon), label))
        return true;
    char *copy = len ? malloc(len + 1) : NULL;
    if (len && !copy)
        return false;
    if (copy)
        memcpy(copy, label, len + 1);
    free(icon->state.label);
    icon->state.label = copy;
    ui_widget_invalidate(&icon->widget, UI_WIDGET_INVALIDATE_MEASURE);
    return true;
}

const char *ui_icon_label(const struct ui_icon *icon)
{
    return icon && icon->state.label ? icon->state.label : "";
}

void ui_icon_set_action_id(struct ui_icon *icon, int id)
{
    if (!icon)
        return;
    ui_icon_widget(icon);
    icon->state.action_id = id;
}
