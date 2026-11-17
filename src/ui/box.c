#include "ui/box.h"
#include "ui/widget_ops.h"

#include "ui/debug.h"
#include "ui/internal/layout.h"
#include "ui/css.h"
#include "ui/widget.h"

#include <limits.h>

struct ui_box_params ui_box_params(const struct ui_box *box,
                                   struct vector2i basis)
{
    struct ui_box_params out = {
        .direction = box ? box->impl.direction : UI_BOX_ROW,
        .align = CSS_ALIGN_START,
        .justify = CSS_JUSTIFY_START,
    };
    if (!box)
        return out;

    const struct css_style *style = &box->widget.style;
    const struct css_scale_context *scale = &box->widget.style_scale;
    if (style->has_flex_direction) {
        out.direction = style->flex_direction == CSS_FLEX_COLUMN
            ? UI_BOX_COLUMN : UI_BOX_ROW;
    }
    if (style->has_align_items)
        out.align = style->align_items;
    if (style->has_justify_content)
        out.justify = style->justify_content;
    out.padding = ui_css_resolve_insets(style->padding, basis, scale, true);
    int border = ui_widget_border_width(&box->widget);
    if (border > 0) {
        for (int i = 0; i < 4; i++)
            out.padding.v[i] = geom_sat_i((int64_t)out.padding.v[i] + border);
    }
    float main_basis = (float)(out.direction == UI_BOX_COLUMN
        ? basis.y : basis.x);
    out.gap = ui_css_resolve_int(style->gap, main_basis, 0, scale, true);
    return out;
}

static struct ui_box *box_of(const struct ui_widget *widget)
{
    if (!widget)
        return NULL;
    return widget->userdata ? (struct ui_box *)widget->userdata
                            : (struct ui_box *)widget;
}

static struct vector2i box_measure(const struct ui_widget *widget,
                                   struct ui_constraints constraints)
{
    return ui_layout_measure((struct ui_widget *)widget, constraints);
}

static void box_layout(struct ui_widget *widget, struct rect2i bounds)
{
    ui_layout_place(widget, bounds);
}

static void box_debug_parts(const struct ui_widget *widget,
                            struct ui_debug_parts *out)
{
    const struct ui_box *box = box_of(widget);
    if (!box)
        return;
    struct ui_box_params p = ui_box_params(box, widget->bounds.size);
    ui_debug_part(out, "content", -1,
                  rect2i_apply_insets(widget->bounds, p.padding));
}

static void box_paint(const struct ui_widget *widget, struct ui_paint_context *paint)
{
    struct ui_paint_box_style style = ui_paint_box_style_of(
        &widget->style, &widget->style_scale);
    ui_paint_box(paint, ui_paint_bounds(paint), &style, NULL);
}

static const struct ui_widget_ops BOX_OPS = {
    .name = "box",
    .paint = box_paint,
    .measure = box_measure,
    .layout = box_layout,
    .debug_parts = box_debug_parts,
};

bool ui_widget_is_box(const struct ui_widget *widget)
{
    return widget && widget->ops == &BOX_OPS;
}

static void box_ensure_init(struct ui_box *box)
{
    if (box && !box->widget.ops)
        ui_box_init(box);
}

void ui_box_init(struct ui_box *box)
{
    if (!box)
        return;
    *box = (struct ui_box){0};
    ui_widget_init(&box->widget, &BOX_OPS, box);
}

void ui_box_set_direction(struct ui_box *box,
                          enum ui_box_direction direction)
{
    if (!box)
        return;
    box_ensure_init(box);
    if (box->impl.direction == direction)
        return;
    box->impl.direction = direction;
    ui_widget_invalidate(&box->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

