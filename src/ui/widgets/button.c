#include "ui/widgets/button.h"

#include "ui/widget_impl.h"
#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/ui_util.h"

#include <string.h>

enum {
    BUTTON_DISABLED = 1u << 0,
    BUTTON_PRESSED = 1u << 1,
    BUTTON_HOVERED = 1u << 2,
    BUTTON_FOCUSED = 1u << 3,
};

static struct ui_text_style button_style(const struct ui_button *button)
{
    return ui_widget_text_style(&button->widget.style_scale);
}

static const struct css_scale_context *button_scale_ctx(
    const struct ui_button *button)
{
    return &button->widget.style_scale;
}

static struct vector2i button_measure(const struct ui_widget *widget,
                                      struct ui_constraints constraints)
{
    const struct ui_button *button = widget
        ? ui_container_of_const(widget, struct ui_button, widget) : NULL;
    if (!button)
        return (struct vector2i){0};
    struct ui_text_style style = button_style(button);
    const struct css_scale_context *ctx = button_scale_ctx(button);
    struct insets2i pad = ui_ctx_insets(ctx, widget->style.padding, 0);
    struct vector2i extent = insets2i_extent(pad);
    int border = widget->style.has_border_color
        ? ui_css_resolve_int(widget->style.border_width, 0, 0, ctx, true)
        : 0;
    const char *text = button->impl.text;
    struct vector2i natural = {
        .x = geom_sat_i((int64_t)ui_text_run_cells(text, strlen(text)) *
                        style.advance + extent.x + 2 * (int64_t)border),
        .y = geom_sat_i((int64_t)style.cell_h + extent.y +
                        2 * (int64_t)border),
    };
    return ui_constraints_clamp(constraints, natural);
}

static void button_stroke(struct ui_paint_context *paint,
                          struct rect2i bounds, int px, uint32_t color)
{
    if (px <= 0 || rect2i_is_empty(bounds))
        return;
    struct rect2i inner = rect2i_inset(bounds, px);
    ui_paint_rect(paint, (struct rect2i){
        .pos = bounds.pos,
        .size = { .x = bounds.width, .y = px } }, color);
    ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = bounds.x, .y = bounds.y + bounds.height - px },
        .size = { .x = bounds.width, .y = px } }, color);
    ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = bounds.x, .y = bounds.y + px },
        .size = { .x = px, .y = inner.height } }, color);
    ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = bounds.x + bounds.width - px, .y = bounds.y + px },
        .size = { .x = px, .y = inner.height } }, color);
}

static void button_paint(const struct ui_widget *widget,
                         struct ui_paint_context *paint)
{
    const struct ui_button *button = widget
        ? ui_container_of_const(widget, struct ui_button, widget) : NULL;
    if (!button || !paint)
        return;
    struct rect2i bounds = ui_paint_bounds(paint);
    const struct css_scale_context *ctx = button_scale_ctx(button);
    int radius = ui_css_resolve_int(widget->style.border_radius, 0, 0,
                                    ctx, true);
    int border = widget->style.has_border_color
        ? ui_css_resolve_int(widget->style.border_width, 0, 0, ctx, true) : 0;
    uint32_t fg = widget->style.has_text_color
        ? widget->style.text_color : 0xFFFFFFFF;

    bool bordered = false;
    if (widget->style.has_border_color && border > 0) {
        bordered = radius > 0
            ? ui_paint_rounded_rect(paint, bounds, radius,
                                    widget->style.border_color)
            : ui_paint_rect(paint, bounds, widget->style.border_color);
    }
    if (widget->style.has_bg_color) {
        struct rect2i fill = border > 0 ? rect2i_inset(bounds, border)
                                        : bounds;
        int fill_radius = radius > border ? radius - border : 0;
        if (fill_radius > 0)
            ui_paint_rounded_rect(paint, fill, fill_radius,
                                  widget->style.bg_color);
        else
            ui_paint_rect(paint, fill, widget->style.bg_color);
    }
    if (!bordered && (button->impl.state & BUTTON_FOCUSED))
        button_stroke(paint, bounds, 1, fg);

    const char *text = button->impl.text;
    if (!text[0])
        return;
    struct ui_text_style style = button_style(button);
    struct insets2i pad = ui_ctx_insets(ctx, widget->style.padding, 0);
    struct rect2i content = rect2i_apply_insets(bounds, pad);
    content = rect2i_inset(content, border);
    int cap = style.advance > 0 && content.width > 0
        ? content.width / style.advance : 0;
    size_t len = ui_text_run_fit(text, strlen(text), cap);
    if (!len)
        return;
    int cells = ui_text_run_cells(text, len);
    struct vector2i extent = {
        .x = cells * style.advance, .y = style.cell_h,
    };
    ui_paint_text(paint, (struct rect2i){
        .pos = {
            .x = geom_sat_i((int64_t)content.x +
                            geom_half_up((int64_t)content.width - extent.x)),
            .y = geom_sat_i((int64_t)content.y +
                            geom_half_up((int64_t)content.height - extent.y)),
        },
        .size = extent,
    }, style, fg, text, len);
}

static bool button_set_state(struct ui_button *button, unsigned state)
{
    if (button->impl.state == state)
        return false;
    button->impl.state = state;
    unsigned pseudo = 0;
    if (state & BUTTON_HOVERED) pseudo |= CSS_PSEUDO_HOVER;
    if (state & BUTTON_PRESSED) pseudo |= CSS_PSEUDO_ACTIVE;
    if (state & BUTTON_FOCUSED) pseudo |= CSS_PSEUDO_FOCUS;
    if (state & BUTTON_DISABLED) pseudo |= CSS_PSEUDO_DISABLED;
    ui_widget_set_style_pseudo(&button->widget, pseudo);
    ui_widget_invalidate(&button->widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

static void button_clicked(struct ui_button *button)
{
    ui_widget_notify(&button->widget, (struct ui_action){
        .kind = UI_ACTION_ACTIVATE,
        .id = button->impl.action_id,
        .index = -1,
    });
}

static bool button_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_button *button = widget
        ? ui_container_of(widget, struct ui_button, widget) : NULL;
    if (!button || !event)
        return false;

    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        button_set_state(button, button->impl.state & ~BUTTON_PRESSED);
        return true;
    }
    if (button->impl.state & BUTTON_DISABLED)
        return false;

    if (event->kind == UI_EVENT_FOCUS_IN) {
        button_set_state(button, button->impl.state | BUTTON_FOCUSED);
        return true;
    }
    if (event->kind == UI_EVENT_FOCUS_OUT) {
        button_set_state(button, button->impl.state &
                         ~(BUTTON_FOCUSED | BUTTON_PRESSED));
        return true;
    }
    if (event->kind == UI_EVENT_KEY) {
        const struct ui_key_event *key = &event->data.key;
        if (key->key != UI_KEY_ENTER &&
            !(key->key == UI_KEY_CHAR && key->text == ' '))
            return false;
        if (key->action == UI_KEY_PRESS)
            button_clicked(button);
        return true;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;

    const struct pointer_event *pointer = &event->data.pointer;
    bool inside = rect2i_contains(button->widget.bounds, pointer->pos);
    switch (pointer->kind) {
    case PTR_MOTION: {
        unsigned state = button->impl.state;
        if (inside)
            state |= BUTTON_HOVERED;
        else
            state &= ~BUTTON_HOVERED;
        button_set_state(button, state);
        return false;
    }
    case PTR_LEAVE:
        button_set_state(button, button->impl.state & ~BUTTON_HOVERED);
        return false;
    case PTR_PRESS:
        if (!inside || pointer->button != 0)
            return false;
        button_set_state(button, button->impl.state | BUTTON_PRESSED);
        ui_widget_focus(widget);
        ui_widget_capture_pointer(widget);
        return true;
    case PTR_RELEASE: {
        if (pointer->button != 0)
            return false;
        ui_widget_release_pointer(widget);
        bool activate = inside && (button->impl.state & BUTTON_PRESSED);
        button_set_state(button, button->impl.state & ~BUTTON_PRESSED);
        if (activate)
            button_clicked(button);
        return activate;
    }
    case PTR_NONE:
    case PTR_SCROLL:
        return false;
    }
    return false;
}

static bool button_accessibility(const struct ui_widget *widget,
                                 struct ui_accessibility_node_spec *out)
{
    const struct ui_button *button = ui_container_of_const(
        widget, struct ui_button, widget);
    out->role = UI_ACCESSIBILITY_ROLE_BUTTON;
    out->name = button->impl.text[0] ? button->impl.text : NULL;
    if (button->impl.state & BUTTON_DISABLED)
        out->states |= UI_ACCESSIBILITY_STATE_DISABLED;
    else
        out->actions |= UI_ACCESSIBILITY_ACTION_INVOKE;
    return true;
}

static bool button_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_button *button = ui_container_of(widget, struct ui_button,
                                               widget);
    if (node_id != ui_widget_accessibility_id(widget) ||
        request->action != UI_ACCESSIBILITY_ACTION_INVOKE ||
        (button->impl.state & BUTTON_DISABLED))
        return false;
    button_clicked(button);
    return true;
}

static const struct ui_widget_ops BUTTON_OPS = {
    .name = "button",
    .measure = button_measure,
    .event = button_event,
    .paint = button_paint,
    .accessibility = button_accessibility,
    .accessibility_action = button_accessibility_action,
};

static void button_ensure_init(struct ui_button *button)
{
    if (!button || button->widget.ops)
        return;
    ui_widget_init(&button->widget, &BUTTON_OPS, NULL);
    ui_widget_set_focusable(&button->widget, true);
    ui_widget_set_style_identity(&button->widget, "button", NULL, "button");
}

void ui_button_init(struct ui_button *button)
{
    if (!button)
        return;
    *button = (struct ui_button){0};
    button_ensure_init(button);
}

void ui_button_set_text(struct ui_button *button, const char *text)
{
    if (!button)
        return;
    button_ensure_init(button);
    if (ui_text_copy(button->impl.text, sizeof(button->impl.text),
                               text))
        ui_widget_invalidate(&button->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

const char *ui_button_text(const struct ui_button *button)
{
    return button ? button->impl.text : "";
}

void ui_button_set_action_id(struct ui_button *button, int id)
{
    if (!button)
        return;
    button_ensure_init(button);
    button->impl.action_id = id;
}

void ui_button_set_disabled(struct ui_button *button, bool disabled)
{
    if (!button)
        return;
    button_ensure_init(button);
    ui_widget_set_focusable(&button->widget, !disabled);
    if (disabled)
        ui_widget_release_pointer(&button->widget);
    unsigned state = button->impl.state;
    if (disabled)
        state = (state | BUTTON_DISABLED) &
                ~(BUTTON_PRESSED | BUTTON_HOVERED | BUTTON_FOCUSED);
    else
        state &= ~BUTTON_DISABLED;
    button_set_state(button, state);
}

bool ui_button_is_disabled(const struct ui_button *button)
{
    return button && (button->impl.state & BUTTON_DISABLED) != 0;
}

bool ui_button_is_pressed(const struct ui_button *button)
{
    return button && (button->impl.state & BUTTON_PRESSED) != 0;
}
