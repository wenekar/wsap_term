#include "ui/internal/layout.h"

#include "ui/internal/tree.h"
#include "ui/surface.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wmissing-braces"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#define CLAY_IMPLEMENTATION
#include "ui/extern/clay/clay.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

struct ui_layout {
    Clay_Context *context;
    void *arena;
    int capacity;
    bool failed;
};

struct layout_node {
    struct ui_widget *widget;
    struct ui_box_params params;
    struct vector2i natural;
    struct vector2i offer;
    struct rect2i bounds;
    struct rect2f float_bounds;
    size_t end;
    bool container;
    bool anchored;
};

struct layout_pass {
    struct layout_node *nodes;
    size_t count, capacity;
    bool measuring, failed;
};

static void clay_error(Clay_ErrorData error)
{
    struct ui_layout *layout = error.userData;
    layout->failed = true;
    fprintf(stderr, "ui layout: %.*s\n", error.errorText.length,
            error.errorText.chars);
}

void ui_layout_destroy(struct ui_layout *layout)
{
    if (!layout)
        return;
    if (Clay_GetCurrentContext() == layout->context)
        Clay_SetCurrentContext(NULL);
    free(layout->arena);
    free(layout);
}

static struct ui_layout *layout_context(struct ui_widget *root, size_t count)
{
    struct ui_surface *surface = ui_widget_surface(root);
    struct ui_layout *layout = surface ? surface->impl.layout : NULL;
    if (!layout) {
        layout = calloc(1, sizeof(*layout));
        if (!layout)
            return NULL;
        if (surface)
            surface->impl.layout = layout;
    }
    if (count > (size_t)((INT_MAX - 64) / 6))
        goto failed;
    int needed = (int)count * 6 + 64;
    if (needed > layout->capacity) {
        int capacity = needed < 256 ? 256 : needed;
        Clay_Context *previous = Clay_GetCurrentContext();
        Clay_SetCurrentContext(NULL);
        Clay_SetMaxElementCount(capacity);
        Clay_SetMaxMeasureTextCacheWordCount(64);
        size_t bytes = Clay_MinMemorySize();
        void *arena = calloc(1, bytes);
        if (!arena) {
            Clay_SetCurrentContext(previous);
            goto failed;
        }
        layout->failed = false;
        Clay_Context *context = Clay_Initialize(
            Clay_CreateArenaWithCapacityAndMemory(bytes, arena),
            (Clay_Dimensions){0},
            (Clay_ErrorHandler){ .errorHandlerFunction = clay_error,
                                 .userData = layout });
        if (layout->failed) {
            Clay_SetCurrentContext(previous);
            free(arena);
            goto failed;
        }
        free(layout->arena);
        layout->arena = arena;
        layout->capacity = capacity;
        layout->context = context;
    }
    Clay_SetCurrentContext(layout->context);
    layout->failed = false;
    return layout;
failed:
    if (!surface)
        ui_layout_destroy(layout);
    return NULL;
}

static size_t collect(struct layout_pass *pass, struct ui_widget *widget,
                       struct ui_constraints constraints, bool island)
{
    if (pass->count == pass->capacity) {
        size_t cap = pass->capacity ? pass->capacity * 2 : 32;
        void *nodes = realloc(pass->nodes, cap * sizeof(*pass->nodes));
        if (!nodes) {
            pass->failed = true;
            return 0;
        }
        pass->nodes = nodes;
        pass->capacity = cap;
    }
    size_t index = pass->count++;
    struct layout_node *node = &pass->nodes[index];
    *node = (struct layout_node){ .widget = widget, .offer = constraints.max,
                                 .container = !island && (ui_widget_is_box(widget) ||
                                     !widget->ops || (!widget->ops->layout &&
                                         (!widget->ops->measure || index == 0))),
                                 .anchored = island && widget->impl.placement.kind != UI_PLACE_FILL };
    if (node->container) {
        if (ui_widget_is_box(widget))
            node->params = ui_box_params((struct ui_box *)widget, constraints.max);
        else
            node->params = (struct ui_box_params){ .direction = UI_BOX_STACK,
                                                   .align = UI_BOX_ALIGN_STRETCH };
        if (pass->measuring)
            widget->impl.invalidation &= ~UI_WIDGET_INVALIDATE_MEASURE;
        bool stack = node->params.direction == UI_BOX_STACK;
        struct vector2i padding = {
            .x = geom_sat_i((int64_t)node->params.padding.left + node->params.padding.right),
            .y = geom_sat_i((int64_t)node->params.padding.top + node->params.padding.bottom),
        };
        constraints.max = vector2i_max(vector2i_sub(constraints.max, padding),
                                         (struct vector2i){0});
        constraints.min = (struct vector2i){0};
        for (struct ui_widget *child = widget->first_child; child;
             child = child->next_sibling) {
            if (!child->impl.hidden) {
                size_t child_index = collect(pass, child, constraints, stack);
                if (!pass->failed && stack && child->impl.placement.kind == UI_PLACE_FILL)
                    pass->nodes[index].natural = vector2i_max(
                        pass->nodes[index].natural, pass->nodes[child_index].natural);
            }
            if (pass->failed)
                break;
        }
    } else {
        node->natural = pass->measuring
            ? ui_widget_measure(widget, constraints) : widget->measured_size;
    }
    pass->nodes[index].end = pass->count;
    return index;
}

static uint16_t spacing(int value)
{
    return (uint16_t)geom_clamp_i(value, 0, UINT16_MAX);
}

static Clay_Padding padding(struct insets2i p)
{
    return (Clay_Padding){ .left = spacing(p.left), .right = spacing(p.right),
                           .top = spacing(p.top), .bottom = spacing(p.bottom) };
}

static Clay_ElementId node_id(size_t index)
{
    return CLAY_IDI("widget", (uint32_t)index);
}

static Clay_SizingAxis sizing(const struct layout_node *node, int axis,
                              bool grow, bool measuring, float offer)
{
    const struct ui_widget *widget = node->widget;
    struct css_value value = widget->style.size[axis];
    float min = css_value_resolve(widget->style.min_size[axis], offer, 0,
                                  &widget->style_scale);
    float max = css_value_resolve(widget->style.max_size[axis], offer,
                                  (float)INT_MAX, &widget->style_scale);
    min = fmaxf(0, min);
    max = fmaxf(min, max);
    if (widget->impl.fixed_size.v[axis])
        return CLAY_SIZING_FIXED(widget->impl.fixed_size.v[axis]);
    if (max == 0)
        return CLAY_SIZING_FIXED(0);
    if (value.unit == CSS_UNIT_PERCENT && !measuring)
        return CLAY_SIZING_PERCENT(fminf(1, fmaxf(0, value.val / 100)));
    if (value.unit != CSS_UNIT_AUTO && value.unit != CSS_UNIT_NONE &&
        value.unit != CSS_UNIT_GROW) {
        float size = css_value_resolve(value, offer, 0, &widget->style_scale);
        return CLAY_SIZING_FIXED(fminf(max, fmaxf(min, size)));
    }
    if ((grow || value.unit == CSS_UNIT_GROW) && !measuring)
        return CLAY_SIZING_GROW(min, max);
    return CLAY_SIZING_FIT(min, max);
}

static Clay_ChildAlignment alignment(struct ui_box_params p)
{
    Clay_ChildAlignment result = {0};
    bool column = p.direction == UI_BOX_COLUMN;
    int cross = p.align == UI_BOX_ALIGN_CENTER ? 2 :
                p.align == UI_BOX_ALIGN_END ? 1 : 0;
    int main = p.justify == UI_BOX_JUSTIFY_CENTER ? 2 :
               p.justify == UI_BOX_JUSTIFY_END ? 1 : 0;
    result.x = (Clay_LayoutAlignmentX)(column ? cross : main);
    result.y = (Clay_LayoutAlignmentY)(column ? main : cross);
    return result;
}

static void emit(struct layout_pass *pass, size_t index,
                  Clay_Sizing size, Clay_FloatingElementConfig floating)
{
    struct layout_node *node = &pass->nodes[index];
    struct ui_box_params p = node->params;
    Clay_ElementDeclaration config = {
        .floating = floating,
        .layout = { .sizing = size,
                    .intrinsicSize = { .width = node->natural.x, .height = node->natural.y },
                    .padding = padding(p.padding),
                    .childGap = spacing(p.gap),
                    .childAlignment = alignment(p),
                    .layoutDirection = p.direction == UI_BOX_COLUMN
                        ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT },
    };
    CLAY(node_id(index), config) {
        if (node->container) {
            for (size_t child = index + 1; child < node->end;
                 child = pass->nodes[child].end) {
                struct layout_node *c = &pass->nodes[child];
                bool column = p.direction == UI_BOX_COLUMN;
                bool stack = p.direction == UI_BOX_STACK;
                Clay_Sizing child_size = {
                    .width = sizing(c, 0, ((column || stack) && p.align == UI_BOX_ALIGN_STRETCH),
                        pass->measuring, c->offer.x),
                    .height = sizing(c, 1, ((!column || stack) && p.align == UI_BOX_ALIGN_STRETCH),
                        pass->measuring, c->offer.y),
                };
                if (stack && !ui_widget_is_box(node->widget) && !c->anchored &&
                    !pass->measuring)
                    child_size = (Clay_Sizing){ .width = CLAY_SIZING_GROW(),
                                               .height = CLAY_SIZING_GROW() };
                if (stack) {
                    Clay_FloatingAttachPointType point = p.align == UI_BOX_ALIGN_CENTER
                        ? CLAY_ATTACH_POINT_CENTER_CENTER : p.align == UI_BOX_ALIGN_END
                        ? CLAY_ATTACH_POINT_RIGHT_BOTTOM : CLAY_ATTACH_POINT_LEFT_TOP;
                    emit(pass, child, child_size,
                        (Clay_FloatingElementConfig){ .attachTo = CLAY_ATTACH_TO_PARENT,
                            .attachPoints = { .parent = point, .element = point } });
                } else {
                    emit(pass, child, child_size, (Clay_FloatingElementConfig){0});
                }
            }
        }
    }
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

static struct rect2i raster_bounds(Clay_BoundingBox b, struct vector2i origin)
{
    int left = round_edge(b.x);
    int top = round_edge(b.y);
    int right = round_edge(b.x + b.width);
    int bottom = round_edge(b.y + b.height);
    return (struct rect2i){
        .pos = vector2i_add(origin, (struct vector2i){ .x = left, .y = top }),
        .size = vector2i_sub((struct vector2i){ .x = right, .y = bottom },
                             (struct vector2i){ .x = left, .y = top }),
    };
}

static struct vector2i solve(struct ui_widget *root,
                              struct ui_constraints constraints,
                              struct rect2i bounds, bool measuring)
{
    struct layout_pass pass = { .measuring = measuring };
    collect(&pass, root, constraints, false);
    struct vector2i result = {0};
    if (pass.failed)
        goto done;
    Clay_Context *previous = Clay_GetCurrentContext();
    struct ui_surface *surface = ui_widget_surface(root);
    bool own_previous = surface && surface->impl.layout &&
                         previous == surface->impl.layout->context;
    struct ui_layout *layout = layout_context(root, pass.count);
    if (!layout)
        goto done;
    Clay_Sizing size = {
        .width = measuring ? CLAY_SIZING_FIT(0, constraints.max.x)
                           : CLAY_SIZING_FIXED(bounds.width),
        .height = measuring ? CLAY_SIZING_FIT(0, constraints.max.y)
                            : CLAY_SIZING_FIXED(bounds.height),
    };
    for (int attempt = 0; attempt < 4; attempt++) {
        Clay_SetCurrentContext(layout->context);
        Clay_SetLayoutDimensions((Clay_Dimensions){ constraints.max.x, constraints.max.y });
        Clay_SetCullingEnabled(false);
        Clay_BeginLayout();
        emit(&pass, 0, size, (Clay_FloatingElementConfig){0});
        Clay_EndLayout(0);
        for (size_t i = 0; i < pass.count; i++) {
            Clay_ElementData data = Clay_GetElementData(node_id(i));
            if (!data.found) {
                layout->failed = true;
                break;
            }
            Clay_BoundingBox b = data.boundingBox;
            pass.nodes[i].bounds = raster_bounds(b, bounds.pos);
            pass.nodes[i].float_bounds = (struct rect2f){
                .pos = { .x = b.x + bounds.x, .y = b.y + bounds.y },
                .size = { .x = b.width, .y = b.height },
            };
        }
        if (layout->failed)
            break;
        bool changed = false;
        for (size_t i = 0; i < pass.count; i++) {
            struct layout_node *node = &pass.nodes[i];
            if (node->container || !node->widget->ops ||
                !node->widget->ops->width_sensitive)
                continue;
            int width = node->bounds.width;
            struct vector2i natural = ui_widget_measure(node->widget,
                (struct ui_constraints){ .min = { .x = width },
                                         .max = { .x = width, .y = INT_MAX } });
            if (!vector2i_eq(natural, node->natural)) {
                node->natural = natural;
                changed = true;
            }
        }
        if (!changed)
            break;
        if (attempt == 3)
            ui_widget_invalidate(root, UI_WIDGET_INVALIDATE_MEASURE);
    }
    bool failed = layout->failed;
    Clay_SetCurrentContext(own_previous ? layout->context : previous);
    if (!ui_widget_surface(root))
        ui_layout_destroy(layout);
    if (failed)
        goto done;
    result = ui_constraints_clamp(constraints, pass.nodes[0].bounds.size);
    for (size_t i = 0; i < pass.count; i++) {
        struct layout_node *node = &pass.nodes[i];
        if (measuring) {
            if (node->container) {
                node->widget->measured_size = node->bounds.size;
            }
        } else if (node->container) {
            node->widget->bounds = node->bounds;
            node->widget->layout_bounds = node->float_bounds;
            node->widget->impl.invalidation &= ~UI_WIDGET_INVALIDATE_LAYOUT;
            node->widget->impl.invalidation |= UI_WIDGET_INVALIDATE_PAINT;
        } else {
            if (node->anchored)
                ui_widget_place_child(node->widget, node->widget->parent->bounds);
            else {
                ui_widget_place(node->widget, node->bounds);
                node->widget->layout_bounds = node->float_bounds;
            }
        }
    }
done:
    free(pass.nodes);
    return result;
}

struct vector2i ui_layout_measure(struct ui_widget *root,
                                   struct ui_constraints constraints)
{
    return solve(root, constraints, (struct rect2i){0}, true);
}

void ui_layout_place(struct ui_widget *root, struct rect2i bounds)
{
    solve(root, (struct ui_constraints){ .max = bounds.size }, bounds, false);
}
