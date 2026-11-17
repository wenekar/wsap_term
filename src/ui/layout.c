#include "ui/internal/layout.h"

#include "ui/internal/tree.h"
#include "ui/ui_util.h"

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

/* The size rule of one child along one axis. */
struct flex_axis {
    enum flex_kind kind;
    float value;
    float min;
    float max;
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

static int round_edge(float value)
{
    if (!isfinite(value))
        return 0;
    if (value >= (float)INT_MAX)
        return INT_MAX;
    if (value <= (float)INT_MIN)
        return INT_MIN;
    return (int)lroundf(value);
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
        return axis_fixed((float)fixed);
    struct flex_axis out;
    out.min = css_value_resolve(style->min_size[axis], basis, 0, scale);
    float max = css_value_resolve(style->max_size[axis], basis,
                                  (float)INT_MAX, scale);
    out.min = fmaxf(0, out.min);
    out.max = fmaxf(out.min, max);
    if (fixed > 0) {
        float size = fminf(out.max, fmaxf(out.min, (float)fixed));
        return axis_fixed(size);
    }
    if (out.max == 0)
        return axis_fixed(0);

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
    return axis->kind != FLEX_FIXED && axis->kind != FLEX_PERCENT;
}

static bool axis_grows(const struct flex_axis *axis)
{
    return axis->kind == FLEX_GROW;
}

/* Removes the entry at `index` from the scratch list of `count` entries. */
static void scratch_remove(struct layout_pass *pass, size_t *count, size_t index)
{
    pass->scratch[index] = pass->scratch[*count - 1];
    (*count)--;
}

/* Compresses the resizable children until `remaining` is zero or all children
 * reach their min size. */
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
        for (size_t i = 0; i < resizable; i++) {
            struct layout_node *child = &pass->nodes[pass->scratch[i]];
            float *size = &child->size[axis];
            if (!float_equal(*size, largest))
                continue;
            float before = *size;
            *size += delta;
            if (*size <= child->axis[axis].min) {
                *size = child->axis[axis].min;
                scratch_remove(pass, &resizable, i--);
            }
            *remaining -= *size - before;
        }
    }
}

/* Grows the growing children until `remaining` is zero or all children reach
 * their max size. */
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
        float smallest = FLT_MAX, second = FLT_MAX, delta = *remaining;
        for (size_t i = 0; i < growing; i++) {
            float size = pass->nodes[pass->scratch[i]].size[axis];
            if (float_equal(size, smallest))
                continue;
            if (size < smallest) {
                second = smallest;
                smallest = size;
            }
            if (size > smallest) {
                second = fminf(second, size);
                delta = second - smallest;
            }
        }
        delta = fminf(delta, *remaining / (float)growing);
        for (size_t i = 0; i < growing; i++) {
            struct layout_node *child = &pass->nodes[pass->scratch[i]];
            float *size = &child->size[axis];
            if (!float_equal(*size, smallest))
                continue;
            float before = *size;
            *size += delta;
            if (*size >= child->axis[axis].max) {
                *size = child->axis[axis].max;
                scratch_remove(pass, &growing, i--);
            }
            *remaining -= *size - before;
        }
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
    size_t count = node->child_count;
    float gaps = child_gap(&p, count);
    float percent_basis[2];
    percent_basis[main] = inner[main] - gaps;
    percent_basis[cross] = inner[cross];
    bool stretch[2];
    stretch[0] = p.direction == UI_BOX_COLUMN && p.align == CSS_ALIGN_STRETCH;
    stretch[1] = p.direction != UI_BOX_COLUMN && p.align == CSS_ALIGN_STRETCH;

    float total = 0;
    for (size_t i = 0; i < count; i++) {
        struct layout_node *child = &pass->nodes[node->first_child + i];
        for (int axis = 0; axis < 2; axis++) {
            child->axis[axis] = axis_sizing(child->widget, axis, inner[axis],
                                            stretch[axis], true);
            if (child->axis[axis].kind == FLEX_PERCENT)
                child->size[axis] =
                    child->axis[axis].value * percent_basis[axis];
            else
                child->size[axis] =
                    axis_clamp(&child->axis[axis], child->natural[axis]);
        }
        total += child->size[main] + margin_start(&child->margin, main) +
                 margin_end(&child->margin, main);
    }

    float remaining = inner[main] - total - gaps;
    if (remaining < 0) {
        if (!pass_reserve_scratch(pass, count))
            return;
        distribute_compress(pass, node->first_child, count, main, &remaining);
    } else if (remaining > 0) {
        if (!pass_reserve_scratch(pass, count))
            return;
        distribute_grow(pass, node->first_child, count, main, &remaining);
    }

    total = 0;
    for (size_t i = 0; i < count; i++) {
        struct layout_node *child = &pass->nodes[node->first_child + i];
        struct flex_axis *axis = &child->axis[cross];
        float content_cross = inner[cross] - margin_start(&child->margin, cross) -
                              margin_end(&child->margin, cross);
        float size = child->size[cross];
        if (axis_resizable(axis)) {
            if (axis->kind == FLEX_GROW)
                size = fminf(content_cross, axis->max);
            size = fmaxf(axis->min, fminf(size, content_cross));
            child->size[cross] = size;
        }
        total += child->size[main] + margin_start(&child->margin, main) +
                 margin_end(&child->margin, main);
    }

    float extra = inner[main] - total - gaps;
    if (extra < 0)
        extra = 0;
    float cursor = content[main];
    if (p.justify == CSS_JUSTIFY_CENTER)
        cursor += extra / 2;
    else if (p.justify == CSS_JUSTIFY_END)
        cursor += extra;
    for (size_t i = 0; i < count; i++) {
        size_t index = node->first_child + i;
        struct layout_node *child = &pass->nodes[index];
        float free = inner[cross] - margin_start(&child->margin, cross) -
                     margin_end(&child->margin, cross) - child->size[cross];
        float pos[2] = { content[0], content[1] };
        pos[main] = cursor + margin_start(&child->margin, main);
        pos[cross] += margin_start(&child->margin, cross);
        if (p.align == CSS_ALIGN_CENTER)
            pos[cross] += free / 2;
        else if (p.align == CSS_ALIGN_END)
            pos[cross] += free;
        place_node(pass, index, pos, child->size);
        cursor += margin_start(&child->margin, main) + child->size[main] +
                  margin_end(&child->margin, main) + (float)p.gap;
    }
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
        return axis->value * basis;
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
    bool stretch = p.align == CSS_ALIGN_STRETCH;
    for (size_t i = 0; i < node->child_count; i++) {
        size_t index = node->first_child + i;
        struct layout_node *child = &pass->nodes[index];
        if (child->out_of_flow) {
            if (pass->applying)
                ui_widget_place_child(child->widget, node->widget->bounds);
            continue;
        }
        float size[2], pos[2] = { content[0], content[1] };
        for (int axis = 0; axis < 2; axis++) {
            float start = margin_start(&child->margin, axis);
            float end = margin_end(&child->margin, axis);
            float fill = fmaxf(0, inner[axis] - start - end);
            struct flex_axis rule = axis_sizing(child->widget, axis,
                                                inner[axis], stretch, true);
            size[axis] = stack_child_size(&rule, inner[axis], fill,
                                          child->natural[axis], plain, stretch);
            float free = inner[axis] - start - end - size[axis];
            pos[axis] += start;
            if (p.align == CSS_ALIGN_CENTER)
                pos[axis] += free / 2;
            else if (p.align == CSS_ALIGN_END)
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
    float main_sum = 0, cross_max = 0;
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
        main_sum += child->size[main] + margin_start(&child->margin, main) +
                    margin_end(&child->margin, main);
        cross_max = fmaxf(cross_max, child->size[cross] +
                                     margin_start(&child->margin, cross) +
                                     margin_end(&child->margin, cross));
    }
    float gaps = child_gap(&p, node->child_count);
    struct vector2f natural;
    if (main == 0) {
        natural.x = main_sum + gaps +
                    (float)(p.padding.left + p.padding.right);
        natural.y = cross_max + (float)(p.padding.top + p.padding.bottom);
    } else {
        natural.x = cross_max + (float)(p.padding.left + p.padding.right);
        natural.y = main_sum + gaps +
                    (float)(p.padding.top + p.padding.bottom);
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
