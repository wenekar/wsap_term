#include "ui/internal/tree.h"

#include "ui/anchor.h"
#include "ui/internal/layout.h"
#include "ui/surface.h"
#include "ui/ui_util.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

static atomic_uint_fast64_t next_accessibility_id = 1;

uint64_t ui_widget_accessibility_id_allocate(void)
{
    uint64_t id;
    do {
        id = atomic_fetch_add_explicit(&next_accessibility_id, 1,
                                       memory_order_relaxed);
    } while (!id);
    return id;
}

bool ui_widget_is_ancestor(const struct ui_widget *ancestor,
                           const struct ui_widget *widget)
{
    for (const struct ui_widget *p = widget; p; p = p->parent) {
        if (p == ancestor)
            return true;
    }
    return false;
}

bool ui_widget_visible(const struct ui_widget *widget)
{
    for (const struct ui_widget *p = widget; p; p = p->parent)
        if (p->impl.destroyed || p->impl.hidden)
            return false;
    return widget != NULL;
}

void ui_widget_init(struct ui_widget *widget,
                    const struct ui_widget_ops *ops, void *userdata)
{
    if (!widget)
        return;
    *widget = (struct ui_widget){
        .ops = ops,
        .userdata = userdata,
        .impl.invalidation = UI_WIDGET_INVALIDATE_MEASURE |
                             UI_WIDGET_INVALIDATE_LAYOUT |
                             UI_WIDGET_INVALIDATE_PAINT,
        .impl.accessibility_id = ui_widget_accessibility_id_allocate(),
    };
}

uint64_t ui_widget_accessibility_id(const struct ui_widget *widget)
{
    return widget ? widget->impl.accessibility_id : 0;
}

bool ui_widget_set_accessible_name(struct ui_widget *widget, const char *name)
{
    if (!widget || widget->impl.destroyed)
        return false;
    if (name && widget->impl.accessible_name &&
        strcmp(name, widget->impl.accessible_name) == 0)
        return true;
    char *copy = name ? strdup(name) : NULL;
    if (name && !copy)
        return false;
    free(widget->impl.accessible_name);
    widget->impl.accessible_name = copy;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

bool ui_widget_set_accessible_description(struct ui_widget *widget,
                                          const char *description)
{
    if (!widget || widget->impl.destroyed)
        return false;
    if (description && widget->impl.accessible_description &&
        strcmp(description, widget->impl.accessible_description) == 0)
        return true;
    char *copy = description ? strdup(description) : NULL;
    if (description && !copy)
        return false;
    free(widget->impl.accessible_description);
    widget->impl.accessible_description = copy;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

bool ui_widget_set_accessible_invalid(struct ui_widget *widget, bool invalid)
{
    if (!widget || widget->impl.destroyed)
        return false;
    if (widget->impl.accessibility_invalid == invalid)
        return true;
    widget->impl.accessibility_invalid = invalid;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

void ui_widget_set_accessibility_ignored(struct ui_widget *widget,
                                         bool ignored)
{
    if (!widget || widget->impl.destroyed ||
        widget->impl.accessibility_ignored == ignored)
        return;
    widget->impl.accessibility_ignored = ignored;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
}

struct ui_surface *ui_widget_surface(const struct ui_widget *widget)
{
    return widget ? widget->impl.surface : NULL;
}

void ui_widget_set_style_identity(struct ui_widget *widget,
                                  const char *class_name,
                                  const char *id_name,
                                  const char *tag_name)
{
    if (!widget)
        return;
    widget->impl.style_class = class_name;
    widget->impl.style_id = id_name;
    widget->impl.style_tag = tag_name;
    widget->impl.style_valid = false;
    ui_widget_part_cache_stale_subtree(widget);
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_widget_set_style_pseudo(struct ui_widget *widget, unsigned pseudo_flags)
{
    if (!widget || widget->impl.style_pseudo == pseudo_flags)
        return;
    widget->impl.style_pseudo = pseudo_flags;
    widget->impl.style_valid = false;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
}

void ui_widget_set_action_handler(struct ui_widget *widget, ui_action_fn fn,
                                  void *userdata)
{
    if (widget && !widget->impl.destroyed)
        widget->impl.actions = (struct ui_action_handler){ fn, userdata };
}

void ui_widget_set_event_handler(struct ui_widget *widget, ui_event_fn fn,
                                 void *userdata)
{
    if (!widget || widget->impl.destroyed)
        return;
    widget->impl.event_fn = fn;
    widget->impl.event_userdata = userdata;
}

void ui_widget_set_key_capture(struct ui_widget *widget, ui_capture_fn fn,
                               void *userdata)
{
    if (!widget || widget->impl.destroyed)
        return;
    widget->impl.key_capture = fn;
    widget->impl.key_capture_userdata = userdata;
}

bool ui_widget_notify(struct ui_widget *widget, struct ui_action action)
{
    if (!widget || widget->impl.destroyed)
        return false;
    for (struct ui_widget *w = widget; w && !widget->impl.destroyed;) {
        struct ui_widget *parent = w->parent;
        if (!w->impl.destroyed && w->impl.actions.fn &&
            w->impl.actions.fn(widget, &action, w->impl.actions.userdata))
            return true;
        w = parent;
    }
    return false;
}

void ui_widget_set_focusable(struct ui_widget *widget, bool focusable)
{
    if (!widget || widget->impl.focusable == focusable)
        return;
    widget->impl.focusable = focusable;
    if (!focusable && ui_widget_has_focus(widget))
        ui_surface_focus(widget->impl.surface, NULL);
}

void ui_widget_set_modal(struct ui_widget *widget, bool modal)
{
    if (!widget || widget->impl.modal == modal)
        return;
    widget->impl.modal = modal;
    if (widget->impl.surface)
        ui_surface_sync_modal(widget->impl.surface);
}

void ui_widget_set_pointer_transparent(struct ui_widget *widget,
                                       bool transparent)
{
    if (widget)
        widget->impl.pointer_transparent = transparent;
}

void ui_widget_set_hidden(struct ui_widget *widget, bool hidden)
{
    if (!widget || widget->impl.destroyed || widget->impl.hidden == hidden)
        return;
    widget->impl.hidden = hidden;
    widget->impl.style_valid = false;
    struct ui_surface *surface = widget->impl.surface;
    if (hidden && surface)
        ui_surface_forget(surface, widget);
    ui_widget_invalidate(widget->parent ? widget->parent : widget,
                         UI_WIDGET_INVALIDATE_MEASURE);
    if (surface)
        ui_surface_sync_modal(surface);
}

bool ui_widget_is_hidden(const struct ui_widget *widget)
{
    return widget && widget->impl.hidden;
}

void ui_widget_set_placement(struct ui_widget *widget,
                             struct ui_placement placement)
{
    if (!widget || memcmp(&widget->impl.placement, &placement,
                          sizeof(placement)) == 0)
        return;
    widget->impl.placement = placement;
    ui_widget_invalidate(widget->parent ? widget->parent : widget,
                         UI_WIDGET_INVALIDATE_LAYOUT);
}

void ui_widget_set_fixed_size(struct ui_widget *widget, struct vector2i size)
{
    if (!widget)
        return;
    size = vector2i_max(size, (struct vector2i){0});
    if (vector2i_eq(widget->impl.fixed_size, size))
        return;
    widget->impl.fixed_size = size;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_widget_focus(struct ui_widget *widget)
{
    if (widget && widget->impl.surface)
        ui_surface_focus(widget->impl.surface, widget);
}

bool ui_widget_has_focus(const struct ui_widget *widget)
{
    return widget && widget->impl.surface &&
           widget->impl.surface->impl.focused == widget;
}

void ui_widget_capture_pointer(struct ui_widget *widget)
{
    if (widget && widget->impl.surface && ui_widget_visible(widget))
        widget->impl.surface->impl.captured = widget;
}

void ui_widget_release_pointer(struct ui_widget *widget)
{
    if (ui_widget_is_captured(widget))
        widget->impl.surface->impl.captured = NULL;
}

bool ui_widget_is_captured(const struct ui_widget *widget)
{
    return widget && widget->impl.surface &&
           widget->impl.surface->impl.captured == widget;
}

void ui_widget_prepare(struct ui_widget *widget)
{
    if (!widget || widget->impl.destroyed)
        return;
    if (widget->ops && widget->ops->prepare)
        widget->ops->prepare(widget);
    struct ui_widget *child = widget->first_child;
    while (!widget->impl.destroyed && child) {
        struct ui_widget *next = child->next_sibling;
        if (child->parent == widget)
            ui_widget_prepare(child);
        child = next;
    }
}

static void unlink_child(struct ui_widget *parent, struct ui_widget *child)
{
    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child->next_sibling;
    else
        parent->first_child = child->next_sibling;

    if (child->next_sibling)
        child->next_sibling->prev_sibling = child->prev_sibling;
    else
        parent->last_child = child->prev_sibling;

    child->prev_sibling = NULL;
    child->next_sibling = NULL;
}

static void link_child(struct ui_widget *parent, struct ui_widget *child,
                       struct ui_widget *before)
{
    child->prev_sibling = before ? before->prev_sibling : parent->last_child;
    child->next_sibling = before;
    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child;
    else
        parent->first_child = child;
    if (before)
        before->prev_sibling = child;
    else
        parent->last_child = child;
}

bool ui_widget_insert_child(struct ui_widget *parent, struct ui_widget *child,
                            struct ui_widget *before)
{
    if (!parent || !child || parent == child || parent->impl.destroyed ||
        child->impl.destroyed || before == child ||
        (before && before->parent != parent))
        return false;

    if (child->parent == parent) {
        if (child->next_sibling == before)
            return true;
        unlink_child(parent, child);
        link_child(parent, child, before);
        ui_widget_invalidate(parent, UI_WIDGET_INVALIDATE_LAYOUT);
        return true;
    }
    if (child->parent || ui_widget_is_ancestor(child, parent))
        return false;
    if (parent->ops && parent->ops->child_limit &&
        parent->impl.child_count >= parent->ops->child_limit)
        return false;
    if (child->impl.surface && child->impl.surface != parent->impl.surface)
        ui_widget_detach(child);
    if (child->parent || child->impl.destroyed)
        return false;

    child->parent = parent;
    link_child(parent, child, before);
    parent->impl.child_count++;
    ui_widget_part_cache_stale_subtree(child);

    ui_widget_attach(child, parent->impl.surface);
    ui_widget_invalidate(parent, UI_WIDGET_INVALIDATE_MEASURE);
    if (parent->impl.surface)
        ui_surface_sync_modal(parent->impl.surface);
    return true;
}

bool ui_widget_add_child(struct ui_widget *parent, struct ui_widget *child)
{
    if (child && child->parent)
        return false;
    return ui_widget_insert_child(parent, child, NULL);
}

void ui_widget_attach(struct ui_widget *widget, struct ui_surface *surface)
{
    if (!widget)
        return;
    if (widget->impl.surface && widget->impl.surface != surface) {
        ui_widget_detach(widget);
        if (widget->impl.surface)
            return;
    }
    if (widget->impl.surface != surface)
        ui_widget_part_cache_stale(widget);
    widget->impl.surface = surface;
    struct ui_widget *c = widget->first_child;
    while (c) {
        struct ui_widget *next = c->next_sibling;
        ui_widget_attach(c, surface);
        c = next;
    }
}

bool ui_widget_remove_child(struct ui_widget *parent, struct ui_widget *child)
{
    if (!parent || !child || child->parent != parent)
        return false;

    unlink_child(parent, child);
    child->parent = NULL;
    parent->impl.child_count--;
    ui_widget_part_cache_stale_subtree(child);

    ui_widget_detach(child);
    ui_widget_invalidate(parent, UI_WIDGET_INVALIDATE_MEASURE);
    if (parent->impl.surface)
        ui_surface_sync_modal(parent->impl.surface);
    return true;
}

bool ui_widget_add_children(struct ui_widget *parent,
                            struct ui_widget * const children[], size_t count)
{
    if (!parent || parent->impl.destroyed)
        return false;
    if (count == 0)
        return true;
    if (!children)
        return false;
    if (parent->ops && parent->ops->child_limit > 0) {
        if (count > parent->ops->child_limit ||
            parent->impl.child_count > parent->ops->child_limit - count)
            return false;
    }

    for (size_t i = 0; i < count; i++) {
        struct ui_widget *child = children[i];
        if (!child || child == parent || child->parent || child->impl.destroyed ||
            ui_widget_is_ancestor(child, parent))
            return false;
        for (size_t j = 0; j < i; j++) {
            if (children[j] == child)
                return false;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (!ui_widget_add_child(parent, children[i])) {
            for (size_t j = i; j > 0; j--)
                ui_widget_remove_child(parent, children[j - 1]);
            return false;
        }
    }

    return true;
}

static void clear_surface(struct ui_widget *widget, struct ui_surface *origin)
{
    if (widget->impl.surface == origin)
        widget->impl.surface = NULL;
    widget->impl.style_valid = false;
    ui_widget_part_cache_stale(widget);
    for (struct ui_widget *c = widget->first_child; c; c = c->next_sibling)
        if (!c->impl.surface || c->impl.surface == origin)
            clear_surface(c, origin);
}

void ui_widget_detach(struct ui_widget *widget)
{
    if (!widget)
        return;
    struct ui_surface *surface = widget->impl.surface;
    if (surface) {
        if (surface->impl.root == widget)
            surface->impl.root = NULL;
        ui_surface_forget(surface, widget);
    }
    clear_surface(widget, surface);
}

size_t ui_widget_child_count(const struct ui_widget *widget)
{
    return widget ? widget->impl.child_count : 0;
}

struct ui_widget *ui_widget_child_at(struct ui_widget *widget, size_t index)
{
    if (!widget || index >= widget->impl.child_count)
        return NULL;
    struct ui_widget *c = widget->first_child;
    while (index-- && c)
        c = c->next_sibling;
    return c;
}

struct vector2i ui_widget_measure(struct ui_widget *widget,
                                  struct ui_constraints constraints)
{
    if (!widget || widget->impl.destroyed)
        return (struct vector2i){0};

    widget->impl.invalidation &= ~UI_WIDGET_INVALIDATE_MEASURE;
    struct vector2i size = constraints.min;
    if (widget->ops && widget->ops->measure) {
        size = widget->ops->measure(widget, constraints);
    } else {
        size = ui_layout_measure(widget, constraints);
    }

    size.x = ui_css_resolve_int(widget->style.width,
                                (float)constraints.max.x, (float)size.x,
                                &widget->style_scale, true);
    size.y = ui_css_resolve_int(widget->style.height,
                                (float)constraints.max.y, (float)size.y,
                                &widget->style_scale, true);
    int min_w = ui_css_resolve_int(widget->style.min_width,
                                   (float)constraints.max.x, 0.0f,
                                   &widget->style_scale, true);
    int min_h = ui_css_resolve_int(widget->style.min_height,
                                   (float)constraints.max.y, 0.0f,
                                   &widget->style_scale, true);
    if (min_w > size.x) size.x = min_w;
    if (min_h > size.y) size.y = min_h;
    for (int axis = 0; axis < 2; axis++) {
        int max = ui_css_resolve_int(widget->style.max_size[axis],
            (float)constraints.max.v[axis], (float)constraints.max.v[axis],
            &widget->style_scale, true);
        if (size.v[axis] > max)
            size.v[axis] = max;
    }
    if (widget->impl.fixed_size.x) size.x = widget->impl.fixed_size.x;
    if (widget->impl.fixed_size.y) size.y = widget->impl.fixed_size.y;

    widget->measured_size = ui_constraints_clamp(constraints, size);
    return widget->measured_size;
}

static int clamp_axis(int v, int lo, int hi)
{
    return v > hi ? hi : v < lo ? lo : v;
}

static struct rect2i placed_rect(const struct ui_widget *child,
                                 struct rect2i bounds)
{
    const struct ui_placement *p = &child->impl.placement;
    if (p->kind == UI_PLACE_FILL)
        return bounds;
    struct vector2i size = vector2i_min(child->measured_size, bounds.size);
    struct vector2i end = rect2i_end(bounds);
    switch (p->kind) {
    case UI_PLACE_BELOW: {
        struct vector2i at = {
            .x = p->anchor.x,
            .y = geom_sat_i((int64_t)p->anchor.y + p->anchor.height),
        };
        return ui_anchor_place_below(at, size, p->gap,
                                     p->anchor.y - p->gap, bounds);
    }
    case UI_PLACE_RIGHT: {
        struct vector2i at = vector2i_sub(
            (struct vector2i){
                .x = geom_sat_i((int64_t)p->anchor.x + p->anchor.width),
                .y = p->anchor.y,
            }, bounds.pos);
        struct rect2i r = ui_anchor_place_right(
            at, size, p->gap, p->anchor.x - bounds.x - p->gap, bounds.size);
        return rect2i_translate(r, bounds.pos);
    }
    case UI_PLACE_AT:
        return (struct rect2i){
            .pos = {
                .x = clamp_axis(p->anchor.x, bounds.x, end.x - size.x),
                .y = clamp_axis(p->anchor.y, bounds.y, end.y - size.y),
            },
            .size = size,
        };
    case UI_PLACE_CENTER: {
        int free_h = bounds.height - size.y;
        int top = (int)((float)free_h * p->y_fraction);
        top = clamp_axis(top, p->gap, free_h - p->gap);
        return (struct rect2i){
            .pos = {
                .x = bounds.x + (bounds.width - size.x) / 2,
                .y = bounds.y + (top > 0 ? top : 0),
            },
            .size = size,
        };
    }
    case UI_PLACE_FILL:
        break;
    }
    return bounds;
}

void ui_widget_layout(struct ui_widget *widget, struct rect2i bounds)
{
    if (!widget || widget->impl.destroyed)
        return;
    widget->bounds = bounds;
    widget->layout_bounds = (struct rect2f){
        .pos = { .x = bounds.x, .y = bounds.y },
        .size = { .x = bounds.width, .y = bounds.height },
    };
    widget->impl.invalidation &= ~UI_WIDGET_INVALIDATE_LAYOUT;
    widget->impl.invalidation |= UI_WIDGET_INVALIDATE_PAINT;
    if (widget->ops && widget->ops->layout) {
        widget->ops->layout(widget, bounds);
        return;
    }
    if (widget->first_child)
        ui_layout_place(widget, bounds);
}

void ui_widget_place_child(struct ui_widget *child, struct rect2i parent_bounds)
{
    if (child && !child->impl.hidden)
        ui_widget_place(child, placed_rect(child, parent_bounds));
}

void ui_widget_place(struct ui_widget *widget, struct rect2i bounds)
{
    if (!widget || widget->impl.destroyed)
        return;
    bool same = vector2i_eq(widget->bounds.pos, bounds.pos) &&
                vector2i_eq(widget->bounds.size, bounds.size);
    if (!same || (widget->impl.invalidation & UI_WIDGET_INVALIDATE_LAYOUT))
        ui_widget_layout(widget, bounds);
}

struct ui_widget *ui_widget_modal(struct ui_widget *root)
{
    if (!root || root->impl.destroyed || root->impl.hidden ||
        root->impl.pointer_transparent)
        return NULL;
    for (struct ui_widget *c = root->last_child; c; c = c->prev_sibling) {
        struct ui_widget *modal = ui_widget_modal(c);
        if (modal)
            return modal;
    }
    return root->impl.modal ? root : NULL;
}

static bool takes_pointer(const struct ui_widget *widget)
{
    return (widget->ops && widget->ops->event) || widget->impl.event_fn;
}

static struct ui_widget *hit_tree(struct ui_widget *widget,
                                  struct vector2i pos)
{
    if (!widget || widget->impl.destroyed || widget->impl.hidden ||
        widget->impl.pointer_transparent)
        return NULL;
    bool inside = rect2i_contains(widget->bounds, pos);
    if (widget->ops && widget->ops->hit_front && takes_pointer(widget) &&
        widget->ops->hit_front(widget, pos))
        return widget;
    if (!widget->ops || !widget->ops->clip_children || inside) {
        for (struct ui_widget *c = widget->last_child; c; c = c->prev_sibling) {
            struct ui_widget *hit = hit_tree(c, pos);
            if (hit)
                return hit;
        }
    }
    if (!takes_pointer(widget))
        return NULL;
    bool hit = widget->ops && widget->ops->hit_test
        ? widget->ops->hit_test(widget, pos) : inside;
    return hit ? widget : NULL;
}

struct ui_widget *ui_widget_hit_test(struct ui_widget *root,
                                     struct vector2i pos)
{
    struct ui_widget *modal = ui_widget_modal(root);
    struct ui_widget *hit = hit_tree(modal ? modal : root, pos);
    return hit ? hit : modal;
}

static bool wants(const struct ui_widget *widget, const struct ui_event *event)
{
    if ((!widget->ops || !widget->ops->event) && !widget->impl.event_fn)
        return false;
    return event->kind != UI_EVENT_KEY ||
           event->data.key.action != UI_KEY_RELEASE ||
           widget->impl.event_fn ||
           (widget->ops && widget->ops->key_release);
}

bool ui_widget_bubble(struct ui_widget *target, const struct ui_event *event)
{
    if (!target || !event || target->impl.destroyed)
        return false;
    struct ui_surface *surface = target->impl.surface;
    for (struct ui_widget *w = target; w;) {
        struct ui_widget *parent = w->parent;
        if (!w->impl.destroyed && w->impl.surface == surface &&
            wants(w, event)) {
            bool key_release = event->kind == UI_EVENT_KEY &&
                               event->data.key.action == UI_KEY_RELEASE;
            bool handled = w->ops && w->ops->event &&
                           (!key_release || w->ops->key_release) &&
                           w->ops->event(w, event);
            if (!handled && !w->impl.destroyed && w->impl.surface == surface &&
                w->impl.event_fn)
                handled = w->impl.event_fn(w, event, w->impl.event_userdata);
            if (handled)
                return true;
        }
        if (target->impl.destroyed || target->impl.surface != surface)
            break;
        w = parent;
    }
    return false;
}

bool ui_widget_event(struct ui_widget *widget, const struct ui_event *event)
{
    if (!widget || !event || widget->impl.destroyed)
        return false;
    if (event->kind == UI_EVENT_POINTER)
        return ui_widget_bubble(ui_widget_hit_test(widget,
                                                   event->data.pointer.pos),
                                event);
    struct ui_widget *modal = ui_widget_modal(widget);
    return ui_widget_bubble(modal ? modal : widget, event);
}

uint64_t ui_widget_tick(struct ui_widget *widget, uint64_t now_ms)
{
    if (!widget || widget->impl.destroyed || widget->impl.hidden)
        return 0;
    uint64_t next = widget->ops && widget->ops->tick
        ? widget->ops->tick(widget, now_ms) : 0;
    widget->impl.deadline = next;
    struct ui_widget *c = widget->first_child;
    while (!widget->impl.destroyed && c) {
        struct ui_widget *following = c->next_sibling;
        uint64_t child = ui_widget_tick(c, now_ms);
        if (child && (!next || child < next))
            next = child;
        c = following;
    }
    return next;
}

void ui_widget_invalidate(struct ui_widget *widget, unsigned flags)
{
    if (!widget || widget->impl.destroyed ||
        flags == UI_WIDGET_INVALIDATE_NONE)
        return;
    if (flags & UI_WIDGET_INVALIDATE_MEASURE)
        flags |= UI_WIDGET_INVALIDATE_LAYOUT | UI_WIDGET_INVALIDATE_PAINT;
    else if (flags & UI_WIDGET_INVALIDATE_LAYOUT)
        flags |= UI_WIDGET_INVALIDATE_PAINT;

    for (struct ui_widget *p = widget; p; p = p->parent)
        p->impl.invalidation |= flags;
    if (widget->impl.surface)
        ui_surface_request_frame(widget->impl.surface);
}

void ui_widget_clear_invalidation(struct ui_widget *widget, unsigned flags)
{
    if (widget)
        widget->impl.invalidation &= ~flags;
}

const struct css_sheet *ui_widget_sheet(const struct ui_widget *widget)
{
    const struct ui_surface *surface = ui_widget_surface(widget);
    return surface ? ui_style_context_sheet(&surface->impl.style)
                   : css_default_sheet();
}

void ui_widget_destroy(struct ui_widget *widget)
{
    if (!widget || widget->impl.destroyed)
        return;
    widget->impl.destroyed = true;
    if (widget->parent)
        ui_widget_remove_child(widget->parent, widget);
    else
        ui_widget_detach(widget);
    while (widget->first_child) {
        struct ui_widget *child = widget->first_child;
        ui_widget_remove_child(widget, child);
        ui_widget_destroy(child);
    }
    if (widget->ops && widget->ops->destroy)
        widget->ops->destroy(widget);
    ui_widget_part_cache_free(widget);
    free(widget->impl.accessible_name);
    widget->impl.accessible_name = NULL;
    free(widget->impl.accessible_description);
    widget->impl.accessible_description = NULL;
}
