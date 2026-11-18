#include "ui/widgets/progress.h"

#include "ui/css.h"
#include "ui/paint.h"
#include "ui/widget_ops.h"

#define PROGRESS_CELLS 12
#define PROGRESS_H 6
#define PROGRESS_TICK_MS 80
/* The share of the track that the moving fill of a busy bar takes. */
#define PROGRESS_BUSY_SHARE 4

static struct ui_progress *progress_of(struct ui_widget *widget)
{
    return ui_container_of(widget, struct ui_progress, widget);
}

static struct vector2i progress_measure(const struct ui_widget *widget,
                                        struct ui_constraints constraints)
{
    const struct ui_progress *progress = widget
        ? ui_container_of_const(widget, struct ui_progress, widget) : NULL;
    if (!progress)
        return (struct vector2i){0};
    int advance = ui_widget_cell_w(widget);
    int cell_h = ui_widget_cell_h(widget);
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    struct vector2i extent = insets2i_extent(pad);
    int border = ui_widget_border_width(widget);
    int width = advance > 0 ? PROGRESS_CELLS * advance : 160;
    int height = cell_h > 0 ? cell_h / 2 : PROGRESS_H;
    if (height < PROGRESS_H)
        height = PROGRESS_H;
    struct vector2i natural = {
        .x = width + extent.x + 2 * border,
        .y = height + extent.y + 2 * border,
    };
    return ui_constraints_clamp(constraints, natural);
}

static void progress_paint(const struct ui_widget *widget,
                           struct ui_paint_context *paint)
{
    const struct ui_progress *progress = widget
        ? ui_container_of_const(widget, struct ui_progress, widget) : NULL;
    if (!progress || !paint)
        return;
    struct rect2i bounds = ui_paint_bounds(paint);
    struct ui_paint_box_style box = ui_paint_box_style_of(&widget->style,
                                                          &widget->style_scale);
    struct rect2i fill;
    struct rect2i content = bounds;
    if (ui_paint_box(paint, bounds, &box, &fill))
        content = fill;
    struct insets2i pad = ui_ctx_insets(&widget->style_scale,
                                        widget->style.padding, 0);
    content = rect2i_apply_insets(content, pad);
    if (rect2i_is_empty(content))
        return;
    uint32_t track_color = widget->style.has_bg_color ? widget->style.bg_color
                                                      : 0xFF3A404E;
    uint32_t bar_color = widget->style.has_border_color
        ? widget->style.border_color : 0xFF4C8DFF;
    int radius = content.height / 2;
    ui_paint_rounded_rect(paint, content, radius, track_color);
    struct rect2i bar = content;
    if (progress->state.indeterminate) {
        int span = content.width / PROGRESS_BUSY_SHARE;
        if (span < 1)
            span = content.width;
        int travel = content.width - span;
        int phase = progress->state.phase;
        if (phase < 0)
            phase = -phase;
        int period = travel > 0 ? 2 * travel : 1;
        int offset = travel > 0 ? (phase * travel / 40) % period : 0;
        if (offset > travel)
            offset = period - offset;
        bar.x = content.x + offset;
        bar.width = span;
    } else {
        bar.width = (int)((float)content.width *
                          ui_progress_value(progress));
    }
    if (bar.width <= 0)
        return;
    ui_paint_rounded_rect(paint, bar, radius, bar_color);
}

static uint64_t progress_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_progress *progress = progress_of(widget);
    if (!progress->state.indeterminate)
        return 0;
    if (!progress->state.next_tick)
        progress->state.next_tick = now_ms + PROGRESS_TICK_MS;
    else if (now_ms >= progress->state.next_tick) {
        progress->state.phase = (progress->state.phase + 1) % 80;
        while (now_ms >= progress->state.next_tick)
            progress->state.next_tick += PROGRESS_TICK_MS;
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    }
    return progress->state.next_tick;
}

static bool progress_accessibility(const struct ui_widget *widget,
                                   struct ui_accessibility_node_spec *out)
{
    static char value[32];
    const struct ui_progress *progress = ui_container_of_const(
        widget, struct ui_progress, widget);
    snprintf(value, sizeof(value), "%d%%",
             (int)(ui_progress_value(progress) * 100.0f + 0.5f));
    out->role = UI_ACCESSIBILITY_ROLE_PROGRESS_INDICATOR;
    out->value = value;
    if (progress->state.indeterminate)
        out->states |= UI_ACCESSIBILITY_STATE_BUSY;
    else
        out->states |= UI_ACCESSIBILITY_STATE_READ_ONLY;
    return true;
}

static const struct ui_widget_ops PROGRESS_OPS = {
    .name = "progress",
    .measure = progress_measure,
    .paint = progress_paint,
    .tick = progress_tick,
    .accessibility = progress_accessibility,
};

void ui_progress_init(struct ui_progress *progress)
{
    if (!progress)
        return;
    *progress = (struct ui_progress){0};
    ui_widget_init(&progress->widget, &PROGRESS_OPS, NULL);
    ui_widget_set_style_identity(&progress->widget, "progress", NULL,
                                "progress");
}

void ui_progress_set_value(struct ui_progress *progress, float value)
{
    if (!progress)
        return;
    ui_progress_widget(progress);
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    if (value == progress->state.value)
        return;
    progress->state.value = value;
    ui_widget_invalidate(&progress->widget, UI_WIDGET_INVALIDATE_PAINT);
}

float ui_progress_value(const struct ui_progress *progress)
{
    return progress ? progress->state.value : 0.0f;
}

void ui_progress_set_indeterminate(struct ui_progress *progress,
                                   bool indeterminate)
{
    if (!progress)
        return;
    ui_progress_widget(progress);
    if (progress->state.indeterminate == indeterminate)
        return;
    progress->state.indeterminate = indeterminate;
    progress->state.next_tick = 0;
    ui_widget_invalidate(&progress->widget, UI_WIDGET_INVALIDATE_PAINT);
}

bool ui_progress_is_indeterminate(const struct ui_progress *progress)
{
    return progress && progress->state.indeterminate;
}
