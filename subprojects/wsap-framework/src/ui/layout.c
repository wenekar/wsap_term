#include "ui/internal/layout.h"

#include "ui/internal/tree.h"
#include "ui/css.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

/* Sizes closer than this count as equal during space distribution. */
#define LAYOUT_EPSILON 0.01f

enum flex_kind {
    FLEX_FIXED,
    FLEX_PERCENT,
    FLEX_GROW,
    FLEX_FIT,
};

/* The size rule of one child along one axis. `grow` and `shrink` are the CSS
   weights of the free space and of the overflow that the child takes. */
struct flex_axis {
    enum flex_kind kind;
    float value;
    float min;
    float max;
    float grow;
    float shrink;
};

struct layout_node {
    struct ui_widget *widget;
    struct ui_box_params params;
    struct flex_axis axis[2];
    struct insets2i margin;
    float natural[2];
    float inner[2];
    float pos[2];
    float size[2];
    size_t first_child;
    size_t child_count;
    bool container;
    bool out_of_flow;
};

struct layout_pass {
    struct layout_node *nodes;
    size_t count;
    size_t capacity;
    size_t *scratch;
    size_t scratch_capacity;
    bool measuring;
    bool applying;
    bool failed;
};

static bool float_equal(float a, float b)
{
    float diff = a - b;
    return diff < LAYOUT_EPSILON && diff > -LAYOUT_EPSILON;
}

/* Rounds one edge or one size. The rule comes from css.h so every part of
   the UI core rounds the same way. */
static int round_edge(float value)
{
    return ui_float_to_int(value, false);
}

static struct vector2i raster_size(const float size[2])
{
    return (struct vector2i){ .x = round_edge(size[0]),
                              .y = round_edge(size[1]) };
}

static struct rect2i raster_rect(const float pos[2], const float size[2])
{
    int left = round_edge(pos[0]);
    int top = round_edge(pos[1]);
    int right = round_edge(pos[0] + size[0]);
    int bottom = round_edge(pos[1] + size[1]);
    return (struct rect2i){
        .pos = { .x = left, .y = top },
        .size = { .x = right - left, .y = bottom - top },
    };
}

static bool pass_reserve(struct layout_pass *pass, size_t count)
{
    if (pass->count + count <= pass->capacity)
        return true;
    size_t capacity = pass->capacity ? pass->capacity : 32;
    while (capacity < pass->count + count)
        capacity *= 2;
    struct layout_node *nodes =
        realloc(pass->nodes, capacity * sizeof(*pass->nodes));
    if (!nodes) {
        pass->failed = true;
        return false;
    }
    pass->nodes = nodes;
    pass->capacity = capacity;
    return true;
}

static bool pass_reserve_scratch(struct layout_pass *pass, size_t count)
{
    if (count <= pass->scratch_capacity)
        return true;
    size_t capacity = pass->scratch_capacity ? pass->scratch_capacity : 32;
    while (capacity < count)
        capacity *= 2;
    size_t *scratch = realloc(pass->scratch, capacity * sizeof(*scratch));
    if (!scratch) {
        pass->failed = true;
        return false;
    }
    pass->scratch = scratch;
    pass->scratch_capacity = capacity;
    return true;
}

static void pass_destroy(struct layout_pass *pass)
{
    free(pass->nodes);
    free(pass->scratch);
}

static bool pass_start(struct layout_pass *pass)
{
    if (!pass_reserve(pass, 1))
        return false;
    pass->count = 1;
    pass->nodes[0] = (struct layout_node){0};
    return true;
}

/* Returns true when `widget` lays out its children. The layout root also
 * counts when it has a measure callback but no layout callback, because
 * ui_widget_layout falls back to the stack layout for it. */
static bool is_layout_container(const struct ui_widget *widget, bool is_root)
{
    if (ui_widget_is_box(widget))
        return true;
    if (!widget->ops)
        return true;
    return !widget->ops->layout && (!widget->ops->measure || is_root);
}

static struct ui_box_params node_params(const struct ui_widget *widget,
                                        struct vector2i basis)
{
    if (ui_widget_is_box(widget))
        return ui_box_params((const struct ui_box *)widget, basis);
    return (struct ui_box_params){ .direction = UI_BOX_STACK,
                                   .align = CSS_ALIGN_STRETCH };
}

static struct flex_axis axis_fixed(float value)
{
    return (struct flex_axis){ .kind = FLEX_FIXED, .value = value };
}

/* Adds the CSS grow and shrink weights to `axis`. They default to one, so a
   child without a weight shares the space like its siblings. */
static struct flex_axis axis_weighted(struct flex_axis axis,
                                      const struct css_style *style)
{
    axis.grow = style->has_flex_grow && isfinite(style->flex_grow)
        ? fmaxf(0.0f, style->flex_grow) : 1.0f;
    axis.shrink = style->has_flex_shrink && isfinite(style->flex_shrink)
        ? fmaxf(0.0f, style->flex_shrink) : 1.0f;
    return axis;
}

/* Returns the size rule of `widget` on `axis` against a parent content size
 * `basis`. `grow` says that a stretch alignment can grow this axis. */
static struct flex_axis axis_sizing(const struct ui_widget *widget, int axis,
                                    float basis, bool grow, bool placing)
{
    const struct css_style *style = &widget->style;
    const struct css_scale_context *scale = &widget->style_scale;
    int fixed = widget->impl.fixed_size.v[axis];
    /* A fixed size needs no limit resolve when neither limit is set. Both
     * limits then resolve to their fallbacks, so the clamp cannot change the
     * size. */
    if (fixed > 0 &&
        (style->min_size[axis].unit == CSS_UNIT_AUTO ||
         style->min_size[axis].unit == CSS_UNIT_NONE) &&
        (style->max_size[axis].unit == CSS_UNIT_AUTO ||
         style->max_size[axis].unit == CSS_UNIT_NONE))
        return axis_weighted(axis_fixed((float)fixed), style);
    struct flex_axis out;
    out.min = css_value_resolve(style->min_size[axis], basis, 0, scale);
    float max = css_value_resolve(style->max_size[axis], basis,
                                  (float)INT_MAX, scale);
    out.min = fmaxf(0, out.min);
    out.max = fmaxf(out.min, max);
    out = axis_weighted(out, style);
    if (fixed > 0) {
        float size = fminf(out.max, fmaxf(out.min, (float)fixed));
        return axis_weighted(axis_fixed(size), style);
    }
    if (out.max == 0)
        return axis_weighted(axis_fixed(0), style);

    struct css_value value = style->size[axis];
    if (value.unit == CSS_UNIT_PERCENT && placing) {
        out.kind = FLEX_PERCENT;
        out.value = fminf(1, fmaxf(0, value.val / 100));
        return out;
    }
    if (value.unit != CSS_UNIT_AUTO && value.unit != CSS_UNIT_NONE &&
        value.unit != CSS_UNIT_GROW) {
        float size = css_value_resolve(value, basis, 0, scale);
        out.kind = FLEX_FIXED;
        out.value = fminf(out.max, fmaxf(out.min, size));
        return out;
    }
    out.kind = (grow || value.unit == CSS_UNIT_GROW) && placing
        ? FLEX_GROW : FLEX_FIT;
    return out;
}

/* Clamps the natural size of a child to its size rule. */
static float axis_clamp(const struct flex_axis *axis, float natural)
{
    if (axis->kind == FLEX_FIXED)
        return axis->value;
    return fminf(axis->max, fmaxf(axis->min, natural));
}

/* Returns the size of a stretched child on one axis for a parent content size
 * `content`. */
static float axis_stretch(const struct flex_axis *axis, float content)
{
    return fmaxf(axis->min, fminf(content, axis->max));
}

/* Returns the margins of a child in CSS order top, right, bottom, left. */
static struct insets2i child_margin(const struct ui_widget *widget,
                                    const float basis[2])
{
    struct vector2i parent = { .x = round_edge(basis[0]),
                               .y = round_edge(basis[1]) };
    return ui_css_resolve_insets(widget->style.margin, parent,
                                 &widget->style_scale, true);
}

/* Returns the margins along one axis as a start and an end size. */
static float margin_start(const struct insets2i *margin, int axis)
{
    return (float)(axis == 0 ? margin->left : margin->top);
}

static float margin_end(const struct insets2i *margin, int axis)
{
    return (float)(axis == 0 ? margin->right : margin->bottom);
}

static bool axis_resizable(const struct flex_axis *axis)
{
    return axis->kind != FLEX_FIXED && axis->kind != FLEX_PERCENT &&
           axis->shrink > 0.0f;
}

static bool axis_grows(const struct flex_axis *axis)
{
    return axis->kind == FLEX_GROW && axis->grow > 0.0f;
}

/* Removes the entry at `index` from the scratch list of `count` entries. */
static void scratch_remove(struct layout_pass *pass, size_t *count, size_t index)
{
    pass->scratch[index] = pass->scratch[*count - 1];
    (*count)--;
}

/* Compresses the resizable children until `remaining` is zero or all children
 * reach their min size. The largest children give up space first. A shrink
 * weight above one makes a child give up more of the same step. */
static void distribute_compress(struct layout_pass *pass, size_t first,
                                size_t count, int axis, float *remaining)
{
    size_t resizable = 0;
    for (size_t i = 0; i < count; i++) {
        struct layout_node *child = &pass->nodes[first + i];
        if (axis_resizable(&child->axis[axis]))
            pass->scratch[resizable++] = first + i;
    }
    while (*remaining < -LAYOUT_EPSILON && resizable > 0) {
        float weight_sum = 0;
        for (size_t i = 0; i < resizable; i++)
            weight_sum += pass->nodes[pass->scratch[i]].axis[axis].shrink;
        if (!(weight_sum > 0.0f))
            break;
        float mean_weight = weight_sum / (float)resizable;
        float largest = 0, second = 0, delta = *remaining;
        for (size_t i = 0; i < resizable; i++) {
            float size = pass->nodes[pass->scratch[i]].size[axis];
            if (float_equal(size, largest))
                continue;
            if (size > largest) {
                second = largest;
                largest = size;
            }
            if (size < largest) {
                second = fmaxf(second, size);
                delta = second - largest;
            }
        }
        delta = fmaxf(delta, *remaining / (float)resizable);
        bool moved = false;
        for (size_t i = 0; i < resizable; i++) {
            struct layout_node *child = &pass->nodes[pass->scratch[i]];
            float *size = &child->size[axis];
            if (!float_equal(*size, largest))
                continue;
            float before = *size;
            float share = delta * child->axis[axis].shrink / mean_weight;
            *size = fmaxf(child->axis[axis].min, before + share);
            *remaining -= *size - before;
            if (*size <= child->axis[axis].min)
                scratch_remove(pass, &resizable, i--);
            if (*size != before)
                moved = true;
        }
        if (!moved)
            break;
    }
}

/* Grows the growing children until `remaining` is zero or all children reach
 * their max size. The children grow toward one level in their size divided by
 * their grow weight, so equal weights produce equal sizes and a larger weight
 * produces a larger size. */
static void distribute_grow(struct layout_pass *pass, size_t first,
                            size_t count, int axis, float *remaining)
{
    size_t growing = 0;
    for (size_t i = 0; i < count; i++) {
        struct layout_node *child = &pass->nodes[first + i];
        if (axis_grows(&child->axis[axis]))
            pass->scratch[growing++] = first + i;
    }
    while (*remaining > LAYOUT_EPSILON && growing > 0) {
        float lowest = FLT_MAX, second = FLT_MAX;
        for (size_t i = 0; i < growing; i++) {
            const struct layout_node *child = &pass->nodes[pass->scratch[i]];
            float weight = child->axis[axis].grow;
            float level = child->size[axis] / weight;
            if (float_equal(level, lowest))
                continue;
            if (level < lowest) {
                second = lowest;
                lowest = level;
            }
            if (level > lowest)
                second = fminf(second, level);
        }
        float weight_sum = 0;
        for (size_t i = 0; i < growing; i++) {
            const struct layout_node *child = &pass->nodes[pass->scratch[i]];
            if (float_equal(child->size[axis] / child->axis[axis].grow, lowest))
                weight_sum += child->axis[axis].grow;
        }
        if (!(weight_sum > 0.0f))
            break;
        float delta = *remaining / weight_sum;
        if (second < FLT_MAX)
            delta = fminf(delta, second - lowest);
        bool moved = false;
        for (size_t i = 0; i < growing; i++) {
            struct layout_node *child = &pass->nodes[pass->scratch[i]];
            float *size = &child->size[axis];
            float weight = child->axis[axis].grow;
            if (!float_equal(*size / weight, lowest))
                continue;
            float before = *size;
            float target = fminf(child->axis[axis].max,
                                 (lowest + delta) * weight);
            if (target < before)
                target = before;
            *size = target;
            *remaining -= *size - before;
            if (*size >= child->axis[axis].max)
                scratch_remove(pass, &growing, i--);
            if (*size != before)
                moved = true;
        }
        if (!moved)
            break;
    }
}

static void place_node(struct layout_pass *pass, size_t index,
                       const float pos[2], const float size[2]);

static float child_gap(const struct ui_box_params *params, size_t count)
{
    return count > 1 ? (float)params->gap * (float)(count - 1) : 0.0f;
}

/* Returns the max size inside the padding of `params`. */
static void content_extent(const struct ui_box_params *params,
                           const float available[2], float out[2])
{
    out[0] = fmaxf(0, available[0] -
                      (float)(params->padding.left + params->padding.right));
    out[1] = fmaxf(0, available[1] -
                      (float)(params->padding.top + params->padding.bottom));
}

/* Returns the alignment of `child` on the cross axis. The CSS `align-self`
 * property of the child wins over the alignment of the parent. */
static enum css_align child_align(const struct ui_widget *widget,
                                  enum css_align parent)
{
    const struct css_style *style = &widget->style;
    return style->has_align_self
        ? (enum css_align)style->align_self : parent;
}

/* Returns the size a flex child takes on `axis` before the free space moves.
 * `percent_basis` is the space inside the margins that a percentage names. */
static float flow_hypothetical(const struct layout_node *child, int axis,
                               float basis, float percent_basis, bool placing)
{
    struct flex_axis rule = axis_sizing(child->widget, axis, basis, false,
                                        placing);
    if (rule.kind == FLEX_PERCENT)
        return fminf(rule.max, fmaxf(rule.min, rule.value * percent_basis));
    return axis_clamp(&rule, child->natural[axis]);
}

/* Returns the space that a child leaves for a percentage size on `axis`. */
static float flow_percent_basis(const struct layout_node *child, int axis,
                                const float inner[2], float gaps)
{
    float start = margin_start(&child->margin, axis);
    float end = margin_end(&child->margin, axis);
    return fmaxf(0.0f, inner[axis] - start - end - gaps);
}

/* Places children [first, first + count) on one flex line. `cross_pos` is the
 * start of the line on the cross axis and `cross_size` is its extent. */
static void place_flow_line(struct layout_pass *pass, struct layout_node *node,
                            size_t first, size_t count,
                            const float content[2], const float inner[2],
                            int main, int cross, float cross_pos,
                            float cross_size)
{
    struct ui_box_params p = node->params;
    float gaps = child_gap(&p, count);
    float total = 0;
    for (size_t i = 0; i < count; i++) {
        struct layout_node *child = &pass->nodes[first + i];
        enum css_align align = child_align(child->widget, p.align);
        bool stretch = align == CSS_ALIGN_STRETCH;
        for (int axis = 0; axis < 2; axis++) {
            float percent_basis = flow_percent_basis(
                child, axis, inner, axis == main ? gaps : 0.0f);
            child->axis[axis] = axis_sizing(
                child->widget, axis, inner[axis],
                axis == cross && stretch, true);
            if (child->axis[axis].kind == FLEX_PERCENT) {
                float size = child->axis[axis].value * percent_basis;
                child->size[axis] = fminf(child->axis[axis].max,
                                          fmaxf(child->axis[axis].min, size));
            } else {
                child->size[axis] =
                    axis_clamp(&child->axis[axis], child->natural[axis]);
            }
        }
        total += child->size[main] + margin_start(&child->margin, main) +
                 margin_end(&child->margin, main);
    }

    float remaining = inner[main] - total - gaps;
    if (remaining < 0) {
        if (!pass_reserve_scratch(pass, count))
            return;
        distribute_compress(pass, first, count, main, &remaining);
    } else if (remaining > 0) {
        if (!pass_reserve_scratch(pass, count))
            return;
        distribute_grow(pass, first, count, main, &remaining);
    }

    total = 0;
    for (size_t i = 0; i < count; i++) {
        struct layout_node *child = &pass->nodes[first + i];
        struct flex_axis *axis = &child->axis[cross];
        float content_cross = cross_size - margin_start(&child->margin, cross) -
                              margin_end(&child->margin, cross);
        float size = child->size[cross];
        if (axis_resizable(axis)) {
            if (child_align(child->widget, p.align) == CSS_ALIGN_STRETCH)
                size = fminf(content_cross, axis->max);
            size = fminf(axis->max, fmaxf(axis->min, fminf(size, content_cross)));
            child->size[cross] = size;
        }
        total += child->size[main] + margin_start(&child->margin, main) +
                 margin_end(&child->margin, main);
    }

    float extra = inner[main] - total - gaps;
    if (extra < 0)
        extra = 0;
    float lead = 0, between = 0;
    switch (p.justify) {
    case CSS_JUSTIFY_CENTER: lead = extra / 2; break;
    case CSS_JUSTIFY_END: lead = extra; break;
    case CSS_JUSTIFY_SPACE_BETWEEN:
        if (count > 1)
            between = extra / (float)(count - 1);
        break;
    case CSS_JUSTIFY_SPACE_AROUND:
        between = extra / (float)count;
        lead = between / 2;
        break;
    case CSS_JUSTIFY_SPACE_EVENLY:
        between = extra / (float)(count + 1);
        lead = between;
        break;
    case CSS_JUSTIFY_START:
        break;
    }

    float cursor = content[main] + lead;
    for (size_t i = 0; i < count; i++) {
        size_t index = first + i;
        struct layout_node *child = &pass->nodes[index];
        enum css_align align = child_align(child->widget, p.align);
        float free = cross_size - margin_start(&child->margin, cross) -
                     margin_end(&child->margin, cross) - child->size[cross];
        float pos[2] = { content[0], content[1] };
        pos[main] = cursor + margin_start(&child->margin, main);
        pos[cross] = cross_pos + margin_start(&child->margin, cross);
        if (align == CSS_ALIGN_CENTER)
            pos[cross] += free / 2;
        else if (align == CSS_ALIGN_END)
            pos[cross] += free;
        place_node(pass, index, pos, child->size);
        cursor += margin_start(&child->margin, main) + child->size[main] +
                  margin_end(&child->margin, main) + (float)p.gap + between;
    }
}

/* Places the children of a wrapping flow in lines. A line takes as many
 * children as its main extent allows. The lines follow each other on the cross
 * axis in source order, or in the opposite order for `wrap-reverse`. */
static void place_flow_wrapped(struct layout_pass *pass, struct layout_node *node,
                               const float content[2], const float inner[2],
                               int main, int cross)
{
    struct ui_box_params p = node->params;
    bool reverse = p.wrap == CSS_WRAP_REVERSE;
    float cross_cursor = reverse ? content[cross] + inner[cross]
                                 : content[cross];
    size_t first = 0, line_count = 0;
    float extent = 0, line_cross = 0;
    for (size_t i = 0; i < node->child_count; i++) {
        struct layout_node *child = &pass->nodes[node->first_child + i];
        float child_main =
            flow_hypothetical(child, main, inner[main],
                              flow_percent_basis(child, main, inner, 0.0f),
                              true) +
            margin_start(&child->margin, main) +
            margin_end(&child->margin, main);
        float child_cross =
            flow_hypothetical(child, cross, inner[cross],
                              flow_percent_basis(child, cross, inner, 0.0f),
                              true) +
            margin_start(&child->margin, cross) +
            margin_end(&child->margin, cross);
        if (line_count > 0 &&
            extent + (float)p.gap + child_main > inner[main] + LAYOUT_EPSILON) {
            float line_pos = reverse ? cross_cursor - line_cross : cross_cursor;
            place_flow_line(pass, node, node->first_child + first, line_count,
                            content, inner, main, cross, line_pos, line_cross);
            cross_cursor += reverse ? -(line_cross + (float)p.gap)
                                    : line_cross + (float)p.gap;
            first = i;
            line_count = 0;
            extent = 0;
            line_cross = 0;
        }
        extent += (line_count > 0 ? (float)p.gap : 0.0f) + child_main;
        line_cross = fmaxf(line_cross, child_cross);
        line_count++;
    }
    if (line_count > 0) {
        float line_pos = reverse ? cross_cursor - line_cross : cross_cursor;
        place_flow_line(pass, node, node->first_child + first, line_count,
                        content, inner, main, cross, line_pos, line_cross);
    }
}

static void place_flow_children(struct layout_pass *pass, struct layout_node *node)
{
    struct ui_box_params p = node->params;
    int main = p.direction == UI_BOX_COLUMN ? 1 : 0;
    int cross = 1 - main;
    float content[2] = { node->pos[0] + (float)p.padding.left,
                         node->pos[1] + (float)p.padding.top };
    float inner[2] = {
        node->size[0] - (float)(p.padding.left + p.padding.right),
        node->size[1] - (float)(p.padding.top + p.padding.bottom),
    };
    if (p.wrap == CSS_WRAP_NONE) {
        place_flow_line(pass, node, node->first_child, node->child_count,
                        content, inner, main, cross, content[cross],
                        inner[cross]);
        return;
    }
    place_flow_wrapped(pass, node, content, inner, main, cross);
}

/* Returns the size of a stack child on one axis. A plain stack stretches every
 * child. A box stack stretches a child only on stretch alignment. `basis` is
 * the parent content size and `fill` is the space inside the margins. */
static float stack_child_size(const struct flex_axis *axis, float basis,
                              float fill, float natural, bool plain,
                              bool stretch)
{
    if (plain)
        return fill;
    switch (axis->kind) {
    case FLEX_FIXED:
        return axis->value;
    case FLEX_PERCENT:
        return axis_clamp(axis, axis->value * basis);
    case FLEX_GROW:
        return axis_stretch(axis, fill);
    case FLEX_FIT:
        break;
    }
    float size = axis_clamp(axis, natural);
    return stretch ? axis_stretch(axis, fill) : size;
}

static void place_stack_children(struct layout_pass *pass, struct layout_node *node)
{
    struct ui_box_params p = node->params;
    float content[2] = { node->pos[0] + (float)p.padding.left,
                         node->pos[1] + (float)p.padding.top };
    float inner[2] = {
        node->size[0] - (float)(p.padding.left + p.padding.right),
        node->size[1] - (float)(p.padding.top + p.padding.bottom),
    };
    bool plain = !ui_widget_is_box(node->widget);
    for (size_t i = 0; i < node->child_count; i++) {
        size_t index = node->first_child + i;
        struct layout_node *child = &pass->nodes[index];
        if (child->out_of_flow) {
            if (pass->applying)
                ui_widget_place_child(child->widget, node->widget->bounds);
            continue;
        }
        enum css_align align = child_align(child->widget, p.align);
        bool stretch = align == CSS_ALIGN_STRETCH;
        float size[2], pos[2] = { content[0], content[1] };
        for (int axis = 0; axis < 2; axis++) {
            float start = margin_start(&child->margin, axis);
            float end = margin_end(&child->margin, axis);
            float fill = fmaxf(0, inner[axis] - start - end);
            struct flex_axis rule = axis_sizing(child->widget, axis,
                                                inner[axis], stretch, true);
            /* A percentage names the space inside the margins. */
            size[axis] = stack_child_size(&rule, fill, fill,
                                          child->natural[axis], plain, stretch);
            float free = inner[axis] - start - end - size[axis];
            pos[axis] += start;
            if (align == CSS_ALIGN_CENTER)
                pos[axis] += free / 2;
            else if (align == CSS_ALIGN_END)
                pos[axis] += free;
        }
        place_node(pass, index, pos, size);
    }
}

static void place_node(struct layout_pass *pass, size_t index,
                       const float pos[2], const float size[2])
{
    struct layout_node *node = &pass->nodes[index];
    node->pos[0] = pos[0];
    node->pos[1] = pos[1];
    node->size[0] = size[0];
    node->size[1] = size[1];
    struct ui_widget *widget = node->widget;
    if (!node->container) {
        if (pass->applying) {
            ui_widget_place(widget, raster_rect(pos, size));
            widget->layout_bounds = (struct rect2f){
                .pos = { .x = pos[0], .y = pos[1] },
                .size = { .x = size[0], .y = size[1] },
            };
        }
        return;
    }
    if (pass->applying) {
        widget->bounds = raster_rect(pos, size);
        widget->layout_bounds = (struct rect2f){
            .pos = { .x = pos[0], .y = pos[1] },
            .size = { .x = size[0], .y = size[1] },
        };
        widget->impl.invalidation &= ~UI_WIDGET_INVALIDATE_LAYOUT;
    }
    if (node->child_count == 0)
        return;
    if (node->params.direction == UI_BOX_STACK)
        place_stack_children(pass, node);
    else
        place_flow_children(pass, node);
}

static struct vector2f flow_natural(struct layout_pass *pass,
                                    struct layout_node *node)
{
    const float *inner = node->inner;
    struct ui_box_params p = node->params;
    int main = p.direction == UI_BOX_COLUMN ? 1 : 0;
    int cross = 1 - main;
    bool wrap = p.wrap != CSS_WRAP_NONE;
    float line_main = 0, line_cross = 0;
    float main_extent = 0, cross_extent = 0;
    size_t line_count = 0, lines_done = 0;
    for (size_t i = 0; i < node->child_count; i++) {
        struct layout_node *child = &pass->nodes[node->first_child + i];
        for (int axis = 0; axis < 2; axis++) {
            child->axis[axis] = axis_sizing(child->widget, axis,
                                            inner[axis], false, false);
            child->size[axis] =
                axis_clamp(&child->axis[axis], child->natural[axis]);
        }
        if (child->container)
            child->widget->measured_size = raster_size(child->size);
        float child_main = child->size[main] +
            margin_start(&child->margin, main) +
            margin_end(&child->margin, main);
        float child_cross = child->size[cross] +
            margin_start(&child->margin, cross) +
            margin_end(&child->margin, cross);
        if (wrap && line_count > 0 &&
            line_main + (float)p.gap + child_main > inner[main] + LAYOUT_EPSILON) {
            main_extent = fmaxf(main_extent, line_main);
            cross_extent += (lines_done ? (float)p.gap : 0.0f) + line_cross;
            lines_done++;
            line_main = 0;
            line_cross = 0;
            line_count = 0;
        }
        line_main += (line_count > 0 ? (float)p.gap : 0.0f) + child_main;
        line_cross = fmaxf(line_cross, child_cross);
        line_count++;
    }
    if (line_count > 0) {
        main_extent = fmaxf(main_extent, line_main);
        cross_extent += (lines_done ? (float)p.gap : 0.0f) + line_cross;
    }
    struct vector2f natural;
    if (main == 0) {
        natural.x = main_extent + (float)(p.padding.left + p.padding.right);
        natural.y = cross_extent + (float)(p.padding.top + p.padding.bottom);
    } else {
        natural.x = cross_extent + (float)(p.padding.left + p.padding.right);
        natural.y = main_extent + (float)(p.padding.top + p.padding.bottom);
    }
    return natural;
}

static struct vector2f stack_natural(struct layout_pass *pass,
                                     struct layout_node *node)
{
    struct ui_box_params p = node->params;
    struct vector2f extent = {0};
    for (size_t i = 0; i < node->child_count; i++) {
        struct layout_node *child = &pass->nodes[node->first_child + i];
        if (child->out_of_flow)
            continue;
        extent.x = fmaxf(extent.x, child->natural[0] +
                                    (float)(child->margin.left +
                                            child->margin.right));
        extent.y = fmaxf(extent.y, child->natural[1] +
                                    (float)(child->margin.top +
                                            child->margin.bottom));
    }
    return (struct vector2f){
        .x = extent.x + (float)(p.padding.left + p.padding.right),
        .y = extent.y + (float)(p.padding.top + p.padding.bottom),
    };
}

static void collect_node(struct layout_pass *pass, size_t index,
                         struct ui_widget *widget, const float available[2],
                         bool is_container)
{
    struct layout_node *node = &pass->nodes[index];
    node->widget = widget;
    node->container = is_container;
    if (!is_container) {
        struct vector2i measured;
        if (pass->measuring) {
            struct ui_constraints constraints = {
                .max = { .x = round_edge(available[0]),
                         .y = round_edge(available[1]) },
            };
            measured = ui_widget_measure(widget, constraints);
        } else {
            measured = widget->measured_size;
        }
        node->natural[0] = (float)measured.x;
        node->natural[1] = (float)measured.y;
        return;
    }

    if (pass->measuring)
        widget->impl.invalidation &= ~UI_WIDGET_INVALIDATE_MEASURE;
    struct vector2i basis = { .x = round_edge(available[0]),
                              .y = round_edge(available[1]) };
    node->params = node_params(widget, basis);
    content_extent(&node->params, available, node->inner);
    bool stack = node->params.direction == UI_BOX_STACK;
    size_t count = 0;
    for (struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling) {
        if (!child->impl.hidden)
            count++;
    }
    size_t first = pass->count;
    if (!pass_reserve(pass, count))
        return;
    pass->count += count;
    node = &pass->nodes[index];
    node->first_child = first;
    node->child_count = count;
    /* The child loop may reallocate `pass->nodes`. Keep a copy of the inner
     * size; `node` must not be read again until it is looked up by index. */
    float inner[2] = { node->inner[0], node->inner[1] };
    size_t slot = first;
    for (struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling) {
        if (child->impl.hidden)
            continue;
        struct layout_node *entry = &pass->nodes[slot];
        entry->margin = child_margin(child, inner);
        float child_available[2] = {
            fmaxf(0, inner[0] -
                       (float)(entry->margin.left + entry->margin.right)),
            fmaxf(0, inner[1] -
                       (float)(entry->margin.top + entry->margin.bottom)),
        };
        bool child_container = !stack && is_layout_container(child, false);
        collect_node(pass, slot, child, child_available, child_container);
        if (pass->failed)
            return;
        entry = &pass->nodes[slot];
        entry->out_of_flow =
            stack && child->impl.placement.kind != UI_PLACE_FILL;
        slot++;
    }
    node = &pass->nodes[index];
    struct vector2f natural = stack
        ? stack_natural(pass, node)
        : flow_natural(pass, node);
    node->natural[0] = natural.x;
    node->natural[1] = natural.y;
}

/* Recomputes the natural size of each container from its children. Nodes are
 * stored parent first, so a reverse scan visits children before parents. */
static void compute_naturals(struct layout_pass *pass)
{
    for (size_t i = pass->count; i-- > 0;) {
        struct layout_node *node = &pass->nodes[i];
        if (!node->container)
            continue;
        struct vector2f natural =
            node->params.direction == UI_BOX_STACK
            ? stack_natural(pass, node) : flow_natural(pass, node);
        node->natural[0] = natural.x;
        node->natural[1] = natural.y;
    }
}

static bool remeasure_width_sensitive(struct layout_pass *pass)
{
    bool changed = false;
    for (size_t i = 0; i < pass->count; i++) {
        struct layout_node *node = &pass->nodes[i];
        struct ui_widget *widget = node->widget;
        if (node->container || !widget->ops || !widget->ops->width_sensitive)
            continue;
        int width = raster_rect(node->pos, node->size).width;
        struct ui_constraints constraints = {
            .min = { .x = width },
            .max = { .x = width, .y = INT_MAX },
        };
        struct vector2i natural = ui_widget_measure(widget, constraints);
        if (vector2i_eq(natural, raster_size(node->natural)))
            continue;
        node->natural[0] = (float)natural.x;
        node->natural[1] = (float)natural.y;
        changed = true;
    }
    return changed;
}

struct vector2i ui_layout_measure(struct ui_widget *root,
                                   struct ui_constraints constraints)
{
    if (!root)
        return (struct vector2i){0};
    struct layout_pass pass = { .measuring = true };
    float available[2] = { (float)constraints.max.x,
                           (float)constraints.max.y };
    float size[2] = { 0, 0 };
    if (!pass_start(&pass)) {
        pass_destroy(&pass);
        return (struct vector2i){0};
    }
    collect_node(&pass, 0, root, available, is_layout_container(root, true));
    for (int attempt = 0; !pass.failed && attempt < 4; attempt++) {
        compute_naturals(&pass);
        for (int axis = 0; axis < 2; axis++) {
            size[axis] = fminf(fmaxf(pass.nodes[0].natural[axis], 0),
                               available[axis]);
        }
        float origin[2] = { 0, 0 };
        place_node(&pass, 0, origin, size);
        if (!remeasure_width_sensitive(&pass))
            break;
        if (attempt == 3)
            ui_widget_invalidate(root, UI_WIDGET_INVALIDATE_MEASURE);
    }
    for (size_t i = 0; !pass.failed && i < pass.count; i++) {
        struct layout_node *node = &pass.nodes[i];
        if (node->container)
            node->widget->measured_size = raster_size(node->size);
    }
    bool failed = pass.failed;
    pass_destroy(&pass);
    if (failed)
        return (struct vector2i){0};
    struct vector2i natural = { .x = round_edge(size[0]),
                                .y = round_edge(size[1]) };
    struct vector2i hi = vector2i_max(constraints.max, constraints.min);
    return vector2i_clamp(natural, constraints.min, hi);
}

void ui_layout_place(struct ui_widget *root, struct rect2i bounds)
{
    if (!root)
        return;
    struct layout_pass pass = { .applying = true };
    float size[2] = { (float)bounds.width, (float)bounds.height };
    float pos[2] = { (float)bounds.x, (float)bounds.y };
    if (!pass_start(&pass)) {
        pass_destroy(&pass);
        return;
    }
    collect_node(&pass, 0, root, size, is_layout_container(root, true));
    for (int attempt = 0; !pass.failed && attempt < 4; attempt++) {
        compute_naturals(&pass);
        place_node(&pass, 0, pos, size);
        if (!remeasure_width_sensitive(&pass))
            break;
        if (attempt == 3)
            ui_widget_invalidate(root, UI_WIDGET_INVALIDATE_MEASURE);
    }
    pass_destroy(&pass);
}
