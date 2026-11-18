#include "ui/widgets/slider.h"

#include "ui/css.h"
#include "ui/paint.h"
#include "ui/paint.h"
#include "ui/widget_ops.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    SLIDER_HOVERED = 1u << 0,
    SLIDER_PRESSED = 1u << 1,
};

#define SLIDER_TRACK_CELLS 8
#define SLIDER_TRACK_H 4

static struct ui_slider *slider_of(struct ui_widget *widget)
{
    return ui_container_of(widget, struct ui_slider, widget);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static float slider_quantize(const struct ui_slider *slider, float value)
{
    float lo = slider->state.min, hi = slider->state.max;
    if (hi < lo) {
        float t = lo;
        lo = hi;
        hi = t;
    }
    value = clampf(value, lo, hi);
    float step = slider->state.step;
    if (step > 0.0f) {
        float steps = roundf((value - lo) / step);
        value = lo + steps * step;
        value = clampf(value, lo, hi);
    }
    return value;
}

static float slider_ratio(const struct ui_slider *slider)
{
    float lo = slider->state.min, hi = slider->state.max;
    if (hi <= lo)
        return 0.0f;
    return clampf((slider->state.value - lo) / (hi - lo), 0.0f, 1.0f);
}

static struct vector2i slider_measure(const struct ui_widget *widget,
                                      struct ui_constraints constraints)
{
    const struct ui_slider *slider = widget
        ? ui_container_of_const(widget, struct ui_slider, widget) : NULL;
    if (!slider)
        return (struct vector2i){0};
    int advance = ui_widget_cell_w(widget);
    int cell_h = ui_widget_cell_h(widget);
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    struct vector2i extent = insets2i_extent(pad);
    int border = ui_widget_border_width(widget);
    int track = advance > 0 ? SLIDER_TRACK_CELLS * advance : 120;
    struct vector2i natural = {
        .x = track + extent.x + 2 * border,
        .y = cell_h + extent.y + 2 * border,
    };
    return ui_constraints_clamp(constraints, natural);
}

static void slider_paint(const struct ui_widget *widget,
                         struct ui_paint_context *paint)
{
    const struct ui_slider *slider = widget
        ? ui_container_of_const(widget, struct ui_slider, widget) : NULL;
    if (!slider || !paint)
        return;
    struct rect2i bounds = ui_paint_bounds(paint);
    struct rect2i content = bounds;
    struct ui_paint_box_style box = ui_paint_box_style_of(&widget->style,
                                                          &widget->style_scale);
    struct rect2i fill;
    if (ui_paint_box(paint, bounds, &box, &fill))
        content = fill;
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    content = rect2i_apply_insets(content, pad);
    if (rect2i_is_empty(content))
        return;
    uint32_t fg = ui_color_opacity(
        widget->style.has_text_color ? widget->style.text_color : 0xFFFFFFFF,
        css_style_effective_opacity(&widget->style));
    uint32_t track_color = widget->style.has_bg_color ? widget->style.bg_color
                                                      : 0xFF3A404E;
    uint32_t accent = widget->style.has_border_color
        ? widget->style.border_color : 0xFF4C8DFF;
    int height = content.height / 3;
    if (height < SLIDER_TRACK_H)
        height = SLIDER_TRACK_H;
    if (height > content.height)
        height = content.height;
    struct rect2i track = {
        .pos = {
            .x = content.x,
            .y = geom_sat_i((int64_t)content.y +
                            geom_half_up((int64_t)content.height - height)),
        },
        .size = { .x = content.width, .y = height },
    };
    int radius = height / 2;
    ui_paint_rounded_rect(paint, track, radius, track_color);
    int filled = (int)((float)track.width * slider_ratio(slider));
    if (filled > 0) {
        struct rect2i done = track;
        done.width = filled;
        ui_paint_rounded_rect(paint, done, radius, accent);
    }
    int knob = content.height;
    if (knob > 2 * height && knob > 8)
        knob = height * 2;
    if (knob < 4)
        knob = 4;
    struct rect2i thumb = {
        .size = { .x = knob, .y = knob },
        .pos = {
            .x = geom_sat_i((int64_t)content.x + filled - knob / 2),
            .y = geom_sat_i((int64_t)content.y +
                            geom_half_up((int64_t)content.height - knob)),
        },
    };
    if (thumb.x < content.x)
        thumb.x = content.x;
    if (thumb.x + thumb.width > content.x + content.width)
        thumb.x = content.x + content.width - thumb.width;
    if (thumb.x < content.x)
        thumb.x = content.x;
    ui_paint_rounded_rect(paint, thumb, knob / 2,
                          (slider->state.flags & SLIDER_PRESSED) ? accent : fg);
    if (slider->state.focused && !slider->state.disabled)
        ui_paint_rect_stroke(paint, bounds, 1, fg);
}

static void slider_sync(struct ui_slider *slider)
{
    unsigned pseudo = 0;
    if (slider->state.flags & SLIDER_HOVERED)
        pseudo |= CSS_PSEUDO_HOVER;
    if (slider->state.flags & SLIDER_PRESSED)
        pseudo |= CSS_PSEUDO_ACTIVE;
    if (slider->state.focused)
        pseudo |= CSS_PSEUDO_FOCUS;
    if (slider->state.disabled)
        pseudo |= CSS_PSEUDO_DISABLED;
    ui_widget_set_style_pseudo(&slider->widget, pseudo);
    ui_widget_invalidate(&slider->widget, UI_WIDGET_INVALIDATE_PAINT);
}

static void slider_commit(struct ui_slider *slider, float value)
{
    float next = slider_quantize(slider, value);
    if (next == slider->state.value)
        return;
    slider->state.value = next;
    ui_widget_invalidate(&slider->widget, UI_WIDGET_INVALIDATE_PAINT);
    ui_widget_notify(&slider->widget, (struct ui_action){
        .kind = UI_ACTION_CHANGE,
        .id = slider->state.action_id,
        .arg = (int)lroundf(next * 1000.0f),
    });
}

static void slider_set_from_x(struct ui_slider *slider, int x)
{
    struct rect2i bounds = slider->widget.bounds;
    if (bounds.width <= 0)
        return;
    float ratio = (float)(x - bounds.x) / (float)bounds.width;
    ratio = clampf(ratio, 0.0f, 1.0f);
    float value = slider->state.min +
                  ratio * (slider->state.max - slider->state.min);
    slider_commit(slider, value);
}

static bool slider_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_slider *slider = widget
        ? ui_container_of(widget, struct ui_slider, widget) : NULL;
    if (!slider || !event)
        return false;
    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        slider->state.flags &= ~SLIDER_PRESSED;
        slider_sync(slider);
        return true;
    }
    if (slider->state.disabled)
        return false;
    if (event->kind == UI_EVENT_FOCUS_IN) {
        slider->state.focused = true;
        slider_sync(slider);
        return true;
    }
    if (event->kind == UI_EVENT_FOCUS_OUT) {
        slider->state.focused = false;
        slider->state.flags &= ~SLIDER_PRESSED;
        slider_sync(slider);
        return true;
    }
    if (event->kind == UI_EVENT_KEY) {
        const struct ui_key_event *key = &event->data.key;
        if (key->action != UI_KEY_PRESS)
            return key->key == UI_KEY_LEFT || key->key == UI_KEY_RIGHT ||
                   key->key == UI_KEY_HOME || key->key == UI_KEY_END;
        float step = slider->state.step > 0.0f ? slider->state.step : 0.01f;
        switch (key->key) {
        case UI_KEY_LEFT:
            slider_commit(slider, slider->state.value - step);
            return true;
        case UI_KEY_RIGHT:
            slider_commit(slider, slider->state.value + step);
            return true;
        case UI_KEY_HOME:
            slider_commit(slider, slider->state.min);
            return true;
        case UI_KEY_END:
            slider_commit(slider, slider->state.max);
            return true;
        default:
            return false;
        }
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct ui_pointer_event *pointer = &event->data.pointer;
    bool inside = rect2i_contains(widget->bounds, pointer->pos);
    switch (pointer->kind) {
    case UI_POINTER_MOTION:
        if (slider->state.flags & SLIDER_PRESSED) {
            slider_set_from_x(slider, pointer->pos.x);
            return true;
        }
        if (inside)
            slider->state.flags |= SLIDER_HOVERED;
        else
            slider->state.flags &= ~SLIDER_HOVERED;
        slider_sync(slider);
        return false;
    case UI_POINTER_LEAVE:
        slider->state.flags &= ~SLIDER_HOVERED;
        slider_sync(slider);
        return false;
    case UI_POINTER_PRESS:
        if (!inside || pointer->button != 0)
            return false;
        slider->state.flags |= SLIDER_PRESSED;
        slider_sync(slider);
        ui_widget_focus(widget);
        ui_widget_capture_pointer(widget);
        slider_set_from_x(slider, pointer->pos.x);
        return true;
    case UI_POINTER_RELEASE:
        if (pointer->button != 0)
            return false;
        bool was = (slider->state.flags & SLIDER_PRESSED) != 0;
        slider->state.flags &= ~SLIDER_PRESSED;
        ui_widget_release_pointer(widget);
        slider_sync(slider);
        return was;
    case UI_POINTER_NONE:
    case UI_POINTER_SCROLL:
        return false;
    }
    return false;
}

static bool slider_accessibility(const struct ui_widget *widget,
                                 struct ui_accessibility_node_spec *out)
{
    const struct ui_slider *slider = ui_container_of_const(
        widget, struct ui_slider, widget);
    static char value[32];
    snprintf(value, sizeof(value), "%g", (double)slider->state.value);
    out->role = UI_ACCESSIBILITY_ROLE_SLIDER;
    out->value = value;
    if (slider->state.disabled)
        out->states |= UI_ACCESSIBILITY_STATE_DISABLED;
    else
        out->actions |= UI_ACCESSIBILITY_ACTION_SET_VALUE |
                        UI_ACCESSIBILITY_ACTION_INCREMENT |
                        UI_ACCESSIBILITY_ACTION_DECREMENT;
    return true;
}

static bool slider_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_slider *slider = slider_of(widget);
    if (node_id != ui_widget_accessibility_id(widget) ||
        slider->state.disabled)
        return false;
    float step = slider->state.step > 0.0f ? slider->state.step : 0.01f;
    switch (request->action) {
    case UI_ACCESSIBILITY_ACTION_INCREMENT:
        slider_commit(slider, slider->state.value + step);
        return true;
    case UI_ACCESSIBILITY_ACTION_DECREMENT:
        slider_commit(slider, slider->state.value - step);
        return true;
    case UI_ACCESSIBILITY_ACTION_SET_VALUE: {
        if (!request->value)
            return false;
        char *end = NULL;
        float value = strtof(request->value, &end);
        if (end == request->value || !isfinite(value))
            return false;
        slider_commit(slider, value);
        return true;
    }
    default:
        return false;
    }
}

static const struct ui_widget_ops SLIDER_OPS = {
    .name = "slider",
    .measure = slider_measure,
    .event = slider_event,
    .paint = slider_paint,
    .accessibility = slider_accessibility,
    .accessibility_action = slider_accessibility_action,
};

void ui_slider_init(struct ui_slider *slider)
{
    if (!slider)
        return;
    *slider = (struct ui_slider){0};
    slider->state.min = 0.0f;
    slider->state.max = 1.0f;
    slider->state.step = 0.05f;
    ui_widget_init(&slider->widget, &SLIDER_OPS, NULL);
    ui_widget_set_focusable(&slider->widget, true);
    ui_widget_set_style_identity(&slider->widget, "slider", NULL, "slider");
    slider_sync(slider);
}

void ui_slider_set_range(struct ui_slider *slider, float min, float max)
{
    if (!slider)
        return;
    ui_slider_widget(slider);
    if (max < min) {
        float t = min;
        min = max;
        max = t;
    }
    slider->state.min = min;
    slider->state.max = max;
    ui_slider_set_value(slider, slider->state.value);
    ui_widget_invalidate(&slider->widget, UI_WIDGET_INVALIDATE_PAINT);
}

void ui_slider_set_step(struct ui_slider *slider, float step)
{
    if (!slider)
        return;
    ui_slider_widget(slider);
    slider->state.step = step > 0.0f ? step : 0.0f;
    ui_slider_set_value(slider, slider->state.value);
}

void ui_slider_set_value(struct ui_slider *slider, float value)
{
    if (!slider)
        return;
    ui_slider_widget(slider);
    float next = slider_quantize(slider, value);
    if (next == slider->state.value)
        return;
    slider->state.value = next;
    ui_widget_invalidate(&slider->widget, UI_WIDGET_INVALIDATE_PAINT);
}

float ui_slider_value(const struct ui_slider *slider)
{
    return slider ? slider->state.value : 0.0f;
}

float ui_slider_min(const struct ui_slider *slider)
{
    return slider ? slider->state.min : 0.0f;
}

float ui_slider_max(const struct ui_slider *slider)
{
    return slider ? slider->state.max : 0.0f;
}

void ui_slider_set_disabled(struct ui_slider *slider, bool disabled)
{
    if (!slider)
        return;
    ui_slider_widget(slider);
    slider->state.disabled = disabled;
    ui_widget_set_focusable(&slider->widget, !disabled);
    if (disabled) {
        slider->state.flags &= ~(SLIDER_HOVERED | SLIDER_PRESSED);
        slider->state.focused = false;
        ui_widget_release_pointer(&slider->widget);
    }
    slider_sync(slider);
}

bool ui_slider_is_disabled(const struct ui_slider *slider)
{
    return slider && slider->state.disabled;
}

void ui_slider_set_action_id(struct ui_slider *slider, int id)
{
    if (!slider)
        return;
    ui_slider_widget(slider);
    slider->state.action_id = id;
}
