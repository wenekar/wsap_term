#include "ui/widgets/toggle.h"

#include "ui/css.h"
#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/text_break.h"
#include "ui/widget_ops.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    TOGGLE_HOVERED = 1u << 0,
    TOGGLE_FOCUSED = 1u << 1,
    TOGGLE_PRESSED = 1u << 2,
};

/* Gap between the indicator and the label, in text cells. */
#define TOGGLE_GAP_CELLS 1
/* The indicator is as high as one text cell and as wide as one cell. */
#define TOGGLE_INDICATOR_CELLS 1

static const char *const TOGGLE_CLASSES[3] = {
    "checkbox",
    "radio",
    "switch",
};

static struct ui_toggle *toggle_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_toggle,
                           widget);
}

static struct ui_text_style toggle_text_style(const struct ui_toggle *toggle)
{
    return ui_widget_text_style(&toggle->widget.style_scale);
}

static struct vector2i toggle_indicator_size(const struct ui_toggle *toggle)
{
    struct ui_text_style style = toggle_text_style(toggle);
    int side = style.cell_h > 0 ? style.cell_h : UI_DEFAULT_CELL_H;
    if (toggle->state.kind == UI_TOGGLE_SWITCH) {
        int width = side + (style.advance > 0 ? style.advance : 1);
        return (struct vector2i){ .x = width, .y = side };
    }
    (void)TOGGLE_INDICATOR_CELLS;
    return (struct vector2i){ .x = side, .y = side };
}

static struct vector2i toggle_measure(const struct ui_widget *widget,
                                      struct ui_constraints constraints)
{
    const struct ui_toggle *toggle = widget
        ? ui_container_of_const(widget, struct ui_toggle, widget) : NULL;
    if (!toggle)
        return (struct vector2i){0};
    struct ui_text_style style = toggle_text_style(toggle);
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    struct vector2i extent = insets2i_extent(pad);
    int border = ui_widget_border_width(widget);
    struct vector2i indicator = toggle_indicator_size(toggle);
    const char *label = ui_toggle_label(toggle);
    int gap = 0;
    int64_t label_w = 0;
    if (label[0]) {
        label_w = (int64_t)ui_text_run_cells(label, strlen(label)) *
                  style.advance;
        gap = (int64_t)TOGGLE_GAP_CELLS * (style.advance > 0 ?
                                              style.advance : 1);
    }
    int64_t row_h = indicator.y > style.cell_h ? indicator.y : style.cell_h;
    struct vector2i natural = {
        .x = geom_sat_i((int64_t)indicator.x + gap + label_w + extent.x +
                        2 * (int64_t)border),
        .y = geom_sat_i(row_h + extent.y + 2 * (int64_t)border),
    };
    return ui_constraints_clamp(constraints, natural);
}

static uint32_t toggle_accent(const struct ui_widget *widget, uint32_t fallback)
{
    if (widget->style.has_bg_color)
        return widget->style.bg_color;
    return fallback;
}

static void toggle_paint_checkbox(const struct ui_toggle *toggle,
                                  struct ui_paint_context *paint,
                                  struct rect2i box, int border, uint32_t fg)
{
    const struct ui_widget *widget = &toggle->widget;
    uint32_t fill = widget->style.has_bg_color ? widget->style.bg_color
                                               : 0xFF20242E;
    uint32_t edge = widget->style.has_border_color ? widget->style.border_color
                                                   : fg;
    int radius = ui_css_resolve_int(widget->style.border_radius, 0, 0,
                                    &widget->style_scale, true);
    if (radius <= 0)
        radius = 3;
    ui_paint_rounded_rect(paint, box, radius, fill);
    ui_paint_rect_stroke(paint, box, border > 0 ? border : 1, edge);
    if (!toggle->state.checked)
        return;
    int inset = box.width / 3;
    if (inset < 1)
        inset = 1;
    struct rect2i mark = rect2i_apply_insets(box, (struct insets2i){
        .left = inset, .top = inset, .right = inset, .bottom = inset,
    });
    ui_paint_rounded_rect(paint, mark, radius > inset ? radius - inset : 0,
                          toggle_accent(widget, 0xFF4C8DFF));
}

static void toggle_paint_radio(const struct ui_toggle *toggle,
                               struct ui_paint_context *paint,
                               struct rect2i box, int border, uint32_t fg)
{
    const struct ui_widget *widget = &toggle->widget;
    uint32_t fill = widget->style.has_bg_color ? widget->style.bg_color
                                               : 0xFF20242E;
    uint32_t edge = widget->style.has_border_color ? widget->style.border_color
                                                   : fg;
    int radius = box.height / 2;
    ui_paint_rounded_rect(paint, box, radius, fill);
    ui_paint_rect_stroke(paint, box, border > 0 ? border : 1, edge);
    if (!toggle->state.checked)
        return;
    int inset = box.width / 4;
    if (inset < 1)
        inset = 1;
    struct rect2i dot = rect2i_apply_insets(box, (struct insets2i){
        .left = inset, .top = inset, .right = inset, .bottom = inset,
    });
    ui_paint_rounded_rect(paint, dot, dot.height / 2,
                          toggle_accent(widget, 0xFF4C8DFF));
}

static void toggle_paint_switch(const struct ui_toggle *toggle,
                                struct ui_paint_context *paint,
                                struct rect2i track, uint32_t fg)
{
    const struct ui_widget *widget = &toggle->widget;
    bool on = toggle->state.checked;
    uint32_t off = widget->style.has_bg_color ? widget->style.bg_color
                                              : 0xFF3A404E;
    uint32_t accent = widget->style.has_border_color
        ? widget->style.border_color : 0xFF4C8DFF;
    int radius = track.height / 2;
    ui_paint_rounded_rect(paint, track, radius, on ? accent : off);
    if (track.height <= 0)
        return;
    int knob = track.height - 4;
    if (knob < 2)
        knob = track.height;
    struct rect2i thumb = {
        .size = { .x = knob, .y = knob },
        .pos = {
            .x = on ? track.x + track.width - knob - 2 : track.x + 2,
            .y = track.y + (track.height - knob) / 2,
        },
    };
    ui_paint_rounded_rect(paint, thumb, knob / 2, fg);
}

static void toggle_paint(const struct ui_widget *widget,
                         struct ui_paint_context *paint)
{
    const struct ui_toggle *toggle = widget
        ? ui_container_of_const(widget, struct ui_toggle, widget) : NULL;
    if (!toggle || !paint)
        return;
    struct rect2i bounds = ui_paint_bounds(paint);
    struct ui_paint_box_style box_style = ui_paint_box_style_of(&widget->style,
                                                               &widget->style_scale);
    struct rect2i fill;
    struct rect2i content = bounds;
    if (ui_paint_box(paint, bounds, &box_style, &fill))
        content = fill;
    uint32_t fg = ui_color_opacity(
        widget->style.has_text_color ? widget->style.text_color : 0xFFFFFFFF,
        css_style_effective_opacity(&widget->style));
    if (toggle->state.focused && !toggle->state.disabled)
        ui_paint_rect_stroke(paint, content, 1, fg);

    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    struct rect2i inner = rect2i_apply_insets(content, pad);
    struct vector2i indicator = toggle_indicator_size(toggle);
    if (indicator.x > inner.width)
        indicator.x = inner.width;
    if (indicator.y > inner.height)
        indicator.y = inner.height;
    if (indicator.x <= 0 || indicator.y <= 0)
        return;
    struct rect2i box = {
        .size = indicator,
        .pos = {
            .x = inner.x,
            .y = geom_sat_i((int64_t)inner.y +
                            geom_half_up((int64_t)inner.height -
                                         (int64_t)indicator.y)),
        },
    };
    int border = ui_widget_border_width(widget);
    switch (toggle->state.kind) {
    case UI_TOGGLE_RADIO:
        toggle_paint_radio(toggle, paint, box, border, fg);
        break;
    case UI_TOGGLE_SWITCH:
        toggle_paint_switch(toggle, paint, box, fg);
        break;
    case UI_TOGGLE_CHECKBOX:
        toggle_paint_checkbox(toggle, paint, box, border, fg);
        break;
    }

    const char *label = ui_toggle_label(toggle);
    if (!label[0])
        return;
    struct ui_text_style style = toggle_text_style(toggle);
    int gap = TOGGLE_GAP_CELLS * (style.advance > 0 ? style.advance : 1);
    struct vector2i origin = {
        .x = box.x + box.width + gap,
        .y = box.y,
    };
    int room = inner.x + inner.width - origin.x;
    int cap = style.advance > 0 && room > 0 ? room / style.advance : 0;
    size_t len = ui_text_fit(label, strlen(label), cap);
    if (!len)
        return;
    struct vector2i extent = {
        .x = (int)((int64_t)ui_text_run_cells(label, len) * style.advance),
        .y = style.cell_h,
    };
    int64_t y = (int64_t)content.y +
                geom_half_up((int64_t)content.height - (int64_t)extent.y);
    if (y < inner.y)
        y = inner.y;
    ui_paint_text(paint, (struct rect2i){
        .pos = { .x = origin.x, .y = geom_sat_i(y) },
        .size = extent,
    }, style, fg, label, len);
}

static void toggle_sync(struct ui_toggle *toggle)
{
    unsigned pseudo = 0;
    if (toggle->state.flags & TOGGLE_HOVERED)
        pseudo |= CSS_PSEUDO_HOVER;
    if (toggle->state.flags & TOGGLE_PRESSED)
        pseudo |= CSS_PSEUDO_ACTIVE;
    if (toggle->state.focused)
        pseudo |= CSS_PSEUDO_FOCUS;
    if (toggle->state.disabled)
        pseudo |= CSS_PSEUDO_DISABLED;
    if (toggle->state.checked)
        pseudo |= CSS_PSEUDO_CHECKED;
    ui_widget_set_style_pseudo(&toggle->widget, pseudo);
    ui_widget_invalidate(&toggle->widget, UI_WIDGET_INVALIDATE_PAINT);
}

static void toggle_flip(struct ui_toggle *toggle)
{
    bool checked;
    if (toggle->state.kind == UI_TOGGLE_RADIO)
        checked = true;
    else
        checked = !toggle->state.checked;
    if (checked != toggle->state.checked) {
        toggle->state.checked = checked;
        toggle_sync(toggle);
    }
    ui_widget_notify(&toggle->widget, (struct ui_action){
        .kind = UI_ACTION_ACTIVATE,
        .id = toggle->state.action_id,
        .index = toggle->state.checked,
    });
}

static bool toggle_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_toggle *toggle = widget
        ? ui_container_of(widget, struct ui_toggle, widget) : NULL;
    if (!toggle || !event)
        return false;
    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        toggle->state.flags &= ~TOGGLE_PRESSED;
        toggle_sync(toggle);
        return true;
    }
    if (toggle->state.disabled)
        return false;
    if (event->kind == UI_EVENT_FOCUS_IN) {
        toggle->state.focused = true;
        toggle_sync(toggle);
        return true;
    }
    if (event->kind == UI_EVENT_FOCUS_OUT) {
        toggle->state.focused = false;
        toggle->state.flags &= ~TOGGLE_PRESSED;
        toggle_sync(toggle);
        return true;
    }
    if (event->kind == UI_EVENT_KEY) {
        const struct ui_key_event *key = &event->data.key;
        if (key->key != UI_KEY_ENTER &&
            !(key->key == UI_KEY_CHAR && key->text == ' '))
            return false;
        if (key->action == UI_KEY_PRESS)
            toggle_flip(toggle);
        return true;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct ui_pointer_event *pointer = &event->data.pointer;
    bool inside = rect2i_contains(widget->bounds, pointer->pos);
    switch (pointer->kind) {
    case UI_POINTER_MOTION: {
        bool hovered = inside;
        if (hovered == ((toggle->state.flags & TOGGLE_HOVERED) != 0))
            return false;
        toggle->state.flags = hovered ? toggle->state.flags | TOGGLE_HOVERED
                                      : toggle->state.flags & ~TOGGLE_HOVERED;
        toggle_sync(toggle);
        return false;
    }
    case UI_POINTER_LEAVE:
        toggle->state.flags &= ~TOGGLE_HOVERED;
        toggle_sync(toggle);
        return false;
    case UI_POINTER_PRESS:
        if (!inside || pointer->button != 0)
            return false;
        toggle->state.flags |= TOGGLE_PRESSED;
        toggle_sync(toggle);
        ui_widget_focus(widget);
        ui_widget_capture_pointer(widget);
        return true;
    case UI_POINTER_RELEASE: {
        if (pointer->button != 0)
            return false;
        ui_widget_release_pointer(widget);
        bool activate = inside && (toggle->state.flags & TOGGLE_PRESSED);
        toggle->state.flags &= ~TOGGLE_PRESSED;
        toggle_sync(toggle);
        if (activate)
            toggle_flip(toggle);
        return activate;
    }
    case UI_POINTER_NONE:
    case UI_POINTER_SCROLL:
        return false;
    }
    return false;
}

static bool toggle_accessibility(const struct ui_widget *widget,
                                 struct ui_accessibility_node_spec *out)
{
    const struct ui_toggle *toggle = ui_container_of_const(
        widget, struct ui_toggle, widget);
    out->role = toggle->state.kind == UI_TOGGLE_RADIO
        ? UI_ACCESSIBILITY_ROLE_RADIO_BUTTON
        : UI_ACCESSIBILITY_ROLE_CHECKBOX;
    const char *label = ui_toggle_label(toggle);
    out->name = label[0] ? label : NULL;
    if (toggle->state.checked)
        out->states |= UI_ACCESSIBILITY_STATE_CHECKED;
    if (toggle->state.disabled)
        out->states |= UI_ACCESSIBILITY_STATE_DISABLED;
    else
        out->actions |= UI_ACCESSIBILITY_ACTION_INVOKE;
    return true;
}

static bool toggle_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_toggle *toggle = toggle_of(widget);
    if (node_id != ui_widget_accessibility_id(widget) ||
        request->action != UI_ACCESSIBILITY_ACTION_INVOKE ||
        toggle->state.disabled)
        return false;
    toggle_flip(toggle);
    return true;
}

static void toggle_destroy(struct ui_widget *widget)
{
    struct ui_toggle *toggle = toggle_of(widget);
    free(toggle->state.label);
    toggle->state.label = NULL;
}

static const struct ui_widget_ops TOGGLE_OPS = {
    .name = "toggle",
    .measure = toggle_measure,
    .event = toggle_event,
    .paint = toggle_paint,
    .destroy = toggle_destroy,
    .accessibility = toggle_accessibility,
    .accessibility_action = toggle_accessibility_action,
};

void ui_toggle_init(struct ui_toggle *toggle, enum ui_toggle_kind kind)
{
    if (!toggle)
        return;
    *toggle = (struct ui_toggle){0};
    if (kind != UI_TOGGLE_RADIO && kind != UI_TOGGLE_SWITCH)
        kind = UI_TOGGLE_CHECKBOX;
    toggle->state.kind = kind;
    ui_widget_init(&toggle->widget, &TOGGLE_OPS, NULL);
    ui_widget_set_focusable(&toggle->widget, true);
    ui_widget_set_style_identity(&toggle->widget, TOGGLE_CLASSES[kind], NULL,
                                TOGGLE_CLASSES[kind]);
    toggle_sync(toggle);
}

void ui_checkbox_init(struct ui_toggle *toggle)
{
    ui_toggle_init(toggle, UI_TOGGLE_CHECKBOX);
}

void ui_radio_init(struct ui_toggle *toggle)
{
    ui_toggle_init(toggle, UI_TOGGLE_RADIO);
}

void ui_switch_init(struct ui_toggle *toggle)
{
    ui_toggle_init(toggle, UI_TOGGLE_SWITCH);
}

enum ui_toggle_kind ui_toggle_kind_of(const struct ui_toggle *toggle)
{
    return toggle ? toggle->state.kind : UI_TOGGLE_CHECKBOX;
}

bool ui_toggle_set_label(struct ui_toggle *toggle, const char *label)
{
    if (!toggle)
        return false;
    ui_toggle_widget(toggle);
    if (!label)
        label = "";
    size_t len = strlen(label);
    if (len > INT_MAX)
        return false;
    if (!strcmp(ui_toggle_label(toggle), label))
        return true;
    char *copy = len ? malloc(len + 1) : NULL;
    if (len && !copy)
        return false;
    if (copy)
        memcpy(copy, label, len + 1);
    free(toggle->state.label);
    toggle->state.label = copy;
    ui_widget_invalidate(&toggle->widget, UI_WIDGET_INVALIDATE_MEASURE);
    return true;
}

const char *ui_toggle_label(const struct ui_toggle *toggle)
{
    return toggle && toggle->state.label ? toggle->state.label : "";
}

void ui_toggle_set_checked(struct ui_toggle *toggle, bool checked)
{
    if (!toggle)
        return;
    ui_toggle_widget(toggle);
    if (toggle->state.checked == checked)
        return;
    toggle->state.checked = checked;
    toggle_sync(toggle);
}

bool ui_toggle_is_checked(const struct ui_toggle *toggle)
{
    return toggle && toggle->state.checked;
}

void ui_toggle_set_disabled(struct ui_toggle *toggle, bool disabled)
{
    if (!toggle)
        return;
    ui_toggle_widget(toggle);
    toggle->state.disabled = disabled;
    ui_widget_set_focusable(&toggle->widget, !disabled);
    if (disabled) {
        toggle->state.flags &= ~(TOGGLE_HOVERED | TOGGLE_PRESSED);
        toggle->state.focused = false;
        ui_widget_release_pointer(&toggle->widget);
    }
    toggle_sync(toggle);
}

bool ui_toggle_is_disabled(const struct ui_toggle *toggle)
{
    return toggle && toggle->state.disabled;
}

void ui_toggle_set_action_id(struct ui_toggle *toggle, int id)
{
    if (!toggle)
        return;
    ui_toggle_widget(toggle);
    toggle->state.action_id = id;
}
