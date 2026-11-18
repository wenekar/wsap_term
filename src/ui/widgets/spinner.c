#include "ui/widgets/spinner.h"

#include "ui/css.h"
#include "ui/paint.h"
#include "ui/widget_ops.h"

#include <math.h>
#include <stdio.h>

#define SPINNER_TICK_MS 90
/* The number of dots on the ring. */
#define SPINNER_STEPS 8

static struct ui_spinner *spinner_of(struct ui_widget *widget)
{
    return ui_container_of(widget, struct ui_spinner, widget);
}

static int spinner_span(const struct ui_spinner *spinner)
{
    return spinner->state.cell_span > 0 ? spinner->state.cell_span : 1;
}

static struct vector2i spinner_measure(const struct ui_widget *widget,
                                       struct ui_constraints constraints)
{
    const struct ui_spinner *spinner = widget
        ? ui_container_of_const(widget, struct ui_spinner, widget) : NULL;
    if (!spinner)
        return (struct vector2i){0};
    int advance = ui_widget_cell_w(widget);
    int cell_h = ui_widget_cell_h(widget);
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    struct vector2i extent = insets2i_extent(pad);
    int border = ui_widget_border_width(widget);
    int side = spinner_span(spinner);
    struct vector2i natural = {
        .x = advance * side + extent.x + 2 * border,
        .y = cell_h * side + extent.y + 2 * border,
    };
    return ui_constraints_clamp(constraints, natural);
}

static void spinner_paint(const struct ui_widget *widget,
                          struct ui_paint_context *paint)
{
    const struct ui_spinner *spinner = widget
        ? ui_container_of_const(widget, struct ui_spinner, widget) : NULL;
    if (!spinner || !paint)
        return;
    struct rect2i bounds = ui_paint_bounds(paint);
    struct ui_paint_box_style box = ui_paint_box_style_of(&widget->style,
                                                          &widget->style_scale);
    struct rect2i content = bounds;
    struct rect2i fill;
    if (ui_paint_box(paint, bounds, &box, &fill))
        content = fill;
    if (rect2i_is_empty(content))
        return;
    uint32_t color = ui_color_opacity(
        widget->style.has_text_color ? widget->style.text_color : 0xFFFFFFFF,
        css_style_effective_opacity(&widget->style));
    int side = content.width < content.height ? content.width : content.height;
    int dot = side / 5;
    if (dot < 1)
        dot = 1;
    int radius = (side - dot) / 2;
    struct vector2i center = {
        .x = content.x + content.width / 2,
        .y = content.y + content.height / 2,
    };
    for (int i = 0; i < SPINNER_STEPS; i++) {
        int step = (spinner->state.phase + i) % SPINNER_STEPS;
        float angle = (float)step * 6.2831853f / (float)SPINNER_STEPS;
        float opacity = 0.15f + 0.85f * (float)(SPINNER_STEPS - i) /
                                (float)SPINNER_STEPS;
        struct rect2i rect = {
            .pos = {
                .x = (int)lroundf((float)center.x + cosf(angle) *
                                  (float)radius) - dot / 2,
                .y = (int)lroundf((float)center.y + sinf(angle) *
                                  (float)radius) - dot / 2,
            },
            .size = { .x = dot, .y = dot },
        };
        ui_paint_rounded_rect(paint, rect, dot / 2,
                              ui_color_opacity(color, opacity));
    }
}

static uint64_t spinner_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_spinner *spinner = spinner_of(widget);
    if (!spinner->state.running)
        return 0;
    if (!spinner->state.next_tick)
        spinner->state.next_tick = now_ms + SPINNER_TICK_MS;
    else if (now_ms >= spinner->state.next_tick) {
        spinner->state.phase = (spinner->state.phase + 1) % SPINNER_STEPS;
        while (now_ms >= spinner->state.next_tick)
            spinner->state.next_tick += SPINNER_TICK_MS;
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    }
    return spinner->state.next_tick;
}

static bool spinner_accessibility(const struct ui_widget *widget,
                                  struct ui_accessibility_node_spec *out)
{
    (void)widget;
    out->role = UI_ACCESSIBILITY_ROLE_PROGRESS_INDICATOR;
    out->states |= UI_ACCESSIBILITY_STATE_BUSY;
    return true;
}

static const struct ui_widget_ops SPINNER_OPS = {
    .name = "spinner",
    .measure = spinner_measure,
    .paint = spinner_paint,
    .tick = spinner_tick,
    .accessibility = spinner_accessibility,
};

void ui_spinner_init(struct ui_spinner *spinner)
{
    if (!spinner)
        return;
    *spinner = (struct ui_spinner){0};
    spinner->state.running = true;
    spinner->state.cell_span = 1;
    ui_widget_init(&spinner->widget, &SPINNER_OPS, NULL);
    ui_widget_set_style_identity(&spinner->widget, "spinner", NULL, "spinner");
}

void ui_spinner_set_cell_span(struct ui_spinner *spinner, int cells)
{
    if (!spinner)
        return;
    ui_spinner_widget(spinner);
    if (cells < 1)
        cells = 1;
    if (spinner->state.cell_span == cells)
        return;
    spinner->state.cell_span = cells;
    ui_widget_invalidate(&spinner->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

int ui_spinner_cell_span(const struct ui_spinner *spinner)
{
    return spinner ? spinner_span(spinner) : 1;
}

void ui_spinner_set_running(struct ui_spinner *spinner, bool running)
{
    if (!spinner)
        return;
    ui_spinner_widget(spinner);
    if (spinner->state.running == running)
        return;
    spinner->state.running = running;
    spinner->state.next_tick = 0;
    ui_widget_invalidate(&spinner->widget, UI_WIDGET_INVALIDATE_PAINT);
}

bool ui_spinner_is_running(const struct ui_spinner *spinner)
{
    return spinner && spinner->state.running;
}
