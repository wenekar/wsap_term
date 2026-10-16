#include "ui/box.h"
#include "ui/widget_impl.h"

#include "ui/debug.h"
#include "ui/internal/layout.h"
#include "ui/ui_util.h"

#include <limits.h>

static int box_nonneg(int value)
{
    return value > 0 ? value : 0;
}

/* The effective layout parameters: struct fields overlaid by the resolved
 * style. basis is the box's own extent (offered constraints at measure
 * time, bounds at layout time) used to resolve % values. */
struct ui_box_params ui_box_params(const struct ui_box *box,
                                       struct vector2i basis)
{
    struct ui_box_params out = {0};
    if (!box)
        return out;
    out.direction = box->impl.direction;
    out.align = box->impl.align;
    out.justify = box->impl.justify;
    out.padding = (struct insets2i){
        .top = box_nonneg(box->impl.padding.top),
        .right = box_nonneg(box->impl.padding.right),
        .bottom = box_nonneg(box->impl.padding.bottom),
        .left = box_nonneg(box->impl.padding.left),
    };
    out.gap = box_nonneg(box->impl.gap);

    const struct css_style *style = &box->widget.style;
    const struct css_scale_context *scale = &box->widget.style_scale;
    if (style->has_flex_direction) {
        out.direction = style->flex_direction == CSS_FLEX_COLUMN
            ? UI_BOX_COLUMN : UI_BOX_ROW;
    }
    if (style->has_align_items) {
        switch (style->align_items) {
        case CSS_ALIGN_START:
            out.align = UI_BOX_ALIGN_START;
            break;
        case CSS_ALIGN_CENTER:
            out.align = UI_BOX_ALIGN_CENTER;
            break;
        case CSS_ALIGN_END:
            out.align = UI_BOX_ALIGN_END;
            break;
        case CSS_ALIGN_STRETCH:
        default:
            out.align = UI_BOX_ALIGN_STRETCH;
            break;
        }
    }
    if (style->has_justify_content) {
        switch (style->justify_content) {
        case CSS_JUSTIFY_CENTER:
            out.justify = UI_BOX_JUSTIFY_CENTER;
            break;
        case CSS_JUSTIFY_END:
            out.justify = UI_BOX_JUSTIFY_END;
            break;
        case CSS_JUSTIFY_START:
        default:
            out.justify = UI_BOX_JUSTIFY_START;
            break;
        }
    }
    for (int i = 0; i < 4; i++) {
        struct css_value v = style->padding.v[i];
        if (v.unit == CSS_UNIT_AUTO || v.unit == CSS_UNIT_NONE)
            continue;
        out.padding.v[i] = ui_css_resolve_int(
            v, (float)((i & 1) ? basis.x : basis.y),
            (float)out.padding.v[i], scale, true);
    }
    if (style->gap.unit != CSS_UNIT_AUTO &&
        style->gap.unit != CSS_UNIT_NONE) {
        float main_basis = (float)(out.direction == UI_BOX_COLUMN
            ? basis.y : basis.x);
        out.gap = ui_css_resolve_int(style->gap, main_basis,
                                     (float)out.gap, scale, true);
    }
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
    if (!widget->style.has_bg_color)
        return;
    int radius = ui_css_resolve_int(widget->style.border_radius, 0, 0,
                                    &widget->style_scale, true);
    struct rect2i bounds = ui_paint_bounds(paint);
    if (radius > 0)
        ui_paint_rounded_rect(paint, bounds, radius, widget->style.bg_color);
    else
        ui_paint_rect(paint, bounds, widget->style.bg_color);
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

void ui_box_set_align(struct ui_box *box,
                              enum ui_box_align align)
{
    if (!box)
        return;
    box_ensure_init(box);
    if (box->impl.align == align)
        return;
    box->impl.align = align;
    ui_widget_invalidate(&box->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

void ui_box_set_justify(struct ui_box *box,
                                enum ui_box_justify justify)
{
    if (!box)
        return;
    box_ensure_init(box);
    if (box->impl.justify == justify)
        return;
    box->impl.justify = justify;
    ui_widget_invalidate(&box->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

void ui_box_set_gap(struct ui_box *box, int gap)
{
    if (!box)
        return;
    box_ensure_init(box);
    if (box->impl.gap == gap)
        return;
    box->impl.gap = gap;
    ui_widget_invalidate(&box->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_box_set_padding(struct ui_box *box,
                                struct insets2i padding)
{
    if (!box)
        return;
    box_ensure_init(box);
    if (insets2i_eq(box->impl.padding, padding))
        return;
    box->impl.padding = padding;
    ui_widget_invalidate(&box->widget, UI_WIDGET_INVALIDATE_MEASURE);
}
