#include "ui/accessibility.h"

#include "ui/surface.h"
#include "ui/text_break.h"
#include "ui/widget.h"
#include "ui/widget_ops.h"
#include "util/hash.h"
#include "util/unicode/utf8.h"

#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *src)
{
    return src ? unicode_utf8_sanitize_dup(src) : NULL;
}

static size_t sanitized_offset(const char *text, size_t offset)
{
    if (!text)
        return 0;
    size_t length = strlen(text);
    if (offset > length)
        offset = length;
    size_t source = 0;
    size_t result = 0;
    while (source < offset) {
        uint32_t codepoint;
        int bytes = unicode_utf8_decode_one(text + source, length - source,
                                            &codepoint);
        if (bytes <= 0) {
            source++;
            continue;
        }
        if (source + (size_t)bytes > offset)
            break;
        source += (size_t)bytes;
        result += (size_t)bytes;
    }
    return result;
}

static void free_node(struct ui_accessibility_node *node)
{
    free(node->name);
    free(node->description);
    free(node->value);
    *node = (struct ui_accessibility_node){0};
}

static size_t index_slot(const struct ui_accessibility_tree *tree, uint64_t id)
{
    size_t mask = tree->id_index_capacity - 1;
    size_t slot = (size_t)hash64_finish(id) & mask;
    while (tree->id_index[slot] &&
           tree->nodes[tree->id_index[slot] - 1].id != id)
        slot = (slot + 1) & mask;
    return slot;
}

static bool index_reserve(struct ui_accessibility_tree *tree, size_t count)
{
    if (count <= tree->id_index_capacity / 2)
        return true;
    size_t capacity = tree->id_index_capacity ? tree->id_index_capacity : 16;
    while (count > capacity / 2) {
        if (capacity > SIZE_MAX / 2)
            return false;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*tree->id_index))
        return false;
    size_t *index = calloc(capacity, sizeof(*index));
    if (!index)
        return false;
    free(tree->id_index);
    tree->id_index = index;
    tree->id_index_capacity = capacity;
    for (size_t i = 0; i < tree->count; i++)
        tree->id_index[index_slot(tree, tree->nodes[i].id)] = i + 1;
    return true;
}

static bool tree_reserve(struct ui_accessibility_tree *tree, size_t count)
{
    if (count <= tree->capacity)
        return true;
    if (count > SIZE_MAX / sizeof(*tree->nodes))
        return false;

    size_t capacity = tree->capacity ? tree->capacity : 8;
    while (capacity < count) {
        if (capacity > SIZE_MAX / 2) {
            capacity = count;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*tree->nodes))
        capacity = count;

    void *nodes = realloc(tree->nodes, capacity * sizeof(*tree->nodes));
    if (!nodes)
        return false;
    tree->nodes = nodes;
    tree->capacity = capacity;
    return true;
}

/* A value equal to the copy in the previous tree moves to the new node.
 * The copy is valid UTF-8, so the sanitized value is the same, and each
 * offset needs no decode. A terminal gives a long value at each frame, and
 * the value changes much less often. */
static bool tree_add(struct ui_accessibility_tree *tree,
                     const struct ui_accessibility_node_spec *spec,
                     struct ui_accessibility_tree *previous)
{
    if (!tree || !spec || !spec->id ||
        ui_accessibility_tree_find(tree, spec->id) ||
        (spec->parent_id && !ui_accessibility_tree_find(tree, spec->parent_id)) ||
        tree->count == SIZE_MAX || !tree_reserve(tree, tree->count + 1) ||
        !index_reserve(tree, tree->count + 1))
        return false;

    bool password = spec->states & UI_ACCESSIBILITY_STATE_PASSWORD;
    struct ui_accessibility_node node = {
        .id = spec->id,
        .parent_id = spec->parent_id,
        .order = spec->order,
        .role = spec->role,
        .states = spec->states,
        .actions = spec->actions,
        .bounds = spec->bounds,
    };
    struct ui_accessibility_node *old = previous && spec->value && !password
        ? (struct ui_accessibility_node *)ui_accessibility_tree_find(previous,
                                                                     spec->id)
        : NULL;
    if (old && old->value && strcmp(old->value, spec->value) == 0) {
        size_t length = strlen(old->value);
        node.value = old->value;
        old->value = NULL;
        node.caret_offset = ui_text_clamp_offset(node.value, length,
                                                 spec->caret_offset);
        node.selection_start = ui_text_clamp_offset(node.value, length,
                                                    spec->selection_start);
        node.selection_end = ui_text_clamp_offset(node.value, length,
                                                  spec->selection_end);
    } else if (!password) {
        node.caret_offset = sanitized_offset(spec->value, spec->caret_offset);
        node.selection_start = sanitized_offset(spec->value,
                                                spec->selection_start);
        node.selection_end = sanitized_offset(spec->value, spec->selection_end);
        node.value = copy_string(spec->value);
    }
    node.name = copy_string(spec->name);
    node.description = copy_string(spec->description);
    node.text = spec->text;
    if (node.text) {
        node.text_offset = spec->text_offset;
        node.text_length = spec->text_length;
        node.text_direction = spec->text_direction;
    }
    if ((spec->name && !node.name) ||
        (spec->description && !node.description) ||
        (spec->value && !password && !node.value)) {
        free_node(&node);
        return false;
    }

    size_t slot = index_slot(tree, node.id);
    tree->nodes[tree->count++] = node;
    tree->id_index[slot] = tree->count;
    if (node.parent_id) {
        size_t parent = tree->id_index[index_slot(tree, node.parent_id)];
        if (parent)
            tree->nodes[parent - 1].child_count++;
    }
    return true;
}

bool ui_accessibility_tree_add(struct ui_accessibility_tree *tree,
                               const struct ui_accessibility_node_spec *spec)
{
    return tree_add(tree, spec, NULL);
}

void ui_accessibility_tree_clear(struct ui_accessibility_tree *tree)
{
    if (!tree)
        return;
    for (size_t i = 0; i < tree->count; i++)
        free_node(&tree->nodes[i]);
    tree->count = 0;
    if (tree->id_index)
        memset(tree->id_index, 0,
               tree->id_index_capacity * sizeof(*tree->id_index));
}

void ui_accessibility_tree_destroy(struct ui_accessibility_tree *tree)
{
    if (!tree)
        return;
    ui_accessibility_tree_clear(tree);
    free(tree->nodes);
    free(tree->id_index);
    *tree = (struct ui_accessibility_tree){0};
}

const struct ui_accessibility_node *
ui_accessibility_tree_find(const struct ui_accessibility_tree *tree,
                           uint64_t id)
{
    if (!tree || !id || !tree->id_index_capacity)
        return NULL;
    size_t entry = tree->id_index[index_slot(tree, id)];
    return entry ? &tree->nodes[entry - 1] : NULL;
}

static bool node_is_provider(const struct ui_accessibility_node *node)
{
    return node && node->text && node->text->length && node->text->read;
}

size_t ui_accessibility_node_text_length(const struct ui_accessibility_node *node)
{
    if (!node)
        return 0;
    if (node_is_provider(node)) {
        size_t total = node->text->length(node->text->instance);
        if (node->text_offset >= total)
            return 0;
        size_t rest = total - node->text_offset;
        return node->text_length < rest ? node->text_length : rest;
    }
    return node->value ? strlen(node->value) : 0;
}

size_t ui_accessibility_node_text_read(const struct ui_accessibility_node *node,
                                       size_t offset, char *out,
                                       size_t capacity)
{
    if (!out || !capacity)
        return 0;
    size_t length = ui_accessibility_node_text_length(node);
    if (offset >= length)
        return 0;
    size_t count = length - offset;
    if (count > capacity)
        count = capacity;
    if (node_is_provider(node)) {
        size_t read = node->text->read(node->text->instance,
                                       node->text_offset + offset, out, count);
        return read < count ? read : count;
    }
    memcpy(out, node->value + offset, count);
    return count;
}

static size_t publish_slot(const struct ui_accessibility_publish_set *set,
                           uint64_t id)
{
    size_t mask = set->index_capacity - 1;
    size_t slot = (size_t)hash64_finish(id) & mask;
    while (set->index[slot] && set->ids[set->index[slot] - 1] != id)
        slot = (slot + 1) & mask;
    return slot;
}

bool ui_accessibility_publish_set_has(
    const struct ui_accessibility_publish_set *set, uint64_t id)
{
    if (!set || !id || !set->index_capacity)
        return false;
    size_t entry = set->index[publish_slot(set, id)];
    return entry && set->ids[entry - 1] == id;
}

static bool publish_reserve(struct ui_accessibility_publish_set *set,
                            size_t count)
{
    if (count > set->index_capacity / 2 || !set->index_capacity) {
        size_t capacity = set->index_capacity ? set->index_capacity : 16;
        while (count > capacity / 2) {
            if (capacity > SIZE_MAX / 2)
                return false;
            capacity *= 2;
        }
        if (capacity > SIZE_MAX / sizeof(*set->index))
            return false;
        size_t *index = calloc(capacity, sizeof(*index));
        if (!index)
            return false;
        free(set->index);
        set->index = index;
        set->index_capacity = capacity;
        for (size_t i = 0; i < set->count; i++)
            set->index[publish_slot(set, set->ids[i])] = i + 1;
    }
    if (count > set->capacity) {
        size_t capacity = set->capacity ? set->capacity : 8;
        while (capacity < count) {
            if (capacity > SIZE_MAX / 2)
                return false;
            capacity *= 2;
        }
        if (capacity > SIZE_MAX / sizeof(*set->ids))
            return false;
        uint64_t *ids = realloc(set->ids, capacity * sizeof(*ids));
        if (!ids)
            return false;
        set->ids = ids;
        set->capacity = capacity;
    }
    return true;
}

static bool publish_add(struct ui_accessibility_publish_set *set, uint64_t id)
{
    if (!id || ui_accessibility_publish_set_has(set, id))
        return true;
    if (!publish_reserve(set, set->count + 1))
        return false;
    set->ids[set->count++] = id;
    set->index[publish_slot(set, id)] = set->count;
    return true;
}

static bool same_text(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

/* A provider node compares by range. The document bytes stay out of the
 * compare, so a changed document costs no full read. */
static bool node_changed(const struct ui_accessibility_node *old,
                         const struct ui_accessibility_node *node)
{
    return old->parent_id != node->parent_id || old->order != node->order ||
           old->role != node->role || old->states != node->states ||
           old->actions != node->actions ||
           !rect2i_eq(old->bounds, node->bounds) ||
           old->caret_offset != node->caret_offset ||
           old->selection_start != node->selection_start ||
           old->selection_end != node->selection_end ||
           old->text != node->text || old->text_offset != node->text_offset ||
           old->text_length != node->text_length ||
           old->text_direction != node->text_direction ||
           old->child_count != node->child_count ||
           !same_text(old->name, node->name) ||
           !same_text(old->description, node->description) ||
           !same_text(old->value, node->value);
}

bool ui_accessibility_publish_set_build(
    const struct ui_accessibility_tree *previous,
    const struct ui_accessibility_tree *next,
    struct ui_accessibility_publish_set *set)
{
    if (!next || !set)
        return false;
    for (size_t i = 0; i < next->count; i++)
        if (!publish_add(set, next->nodes[i].id))
            return false;
    if (!previous || !previous->count)
        return true;

    /* An update cannot remove a node. A removed node then costs a full
     * publish. */
    for (size_t i = 0; i < previous->count; i++)
        if (!ui_accessibility_tree_find(next, previous->nodes[i].id))
            return true;

    ui_accessibility_publish_set_destroy(set);
    for (size_t i = 0; i < next->count; i++) {
        const struct ui_accessibility_node *node = &next->nodes[i];
        const struct ui_accessibility_node *old =
            ui_accessibility_tree_find(previous, node->id);
        if (old && !node_changed(old, node))
            continue;
        if (!publish_add(set, node->id) ||
            (node->parent_id && !publish_add(set, node->parent_id)))
            return false;
    }
    return true;
}

void ui_accessibility_publish_set_destroy(
    struct ui_accessibility_publish_set *set)
{
    if (!set)
        return;
    free(set->ids);
    free(set->index);
    *set = (struct ui_accessibility_publish_set){0};
}

static bool widget_visible(const struct ui_widget *widget)
{
    for (const struct ui_widget *w = widget; w; w = w->parent)
        if (w->impl.destroyed || w->impl.hidden)
            return false;
    return widget != NULL;
}

static const struct ui_widget *modal_in_tree(const struct ui_widget *widget)
{
    if (!widget_visible(widget) || widget->impl.pointer_transparent)
        return NULL;
    for (const struct ui_widget *child = widget->last_child; child;
         child = child->prev_sibling) {
        const struct ui_widget *modal = modal_in_tree(child);
        if (modal)
            return modal;
    }
    return widget->impl.modal ? widget : NULL;
}

static bool widget_is_ancestor(const struct ui_widget *ancestor,
                               const struct ui_widget *widget)
{
    if (!ancestor || !widget)
        return false;
    for (const struct ui_widget *current = widget; current;
         current = current->parent)
        if (current == ancestor)
            return true;
    return false;
}

static bool widget_in_modal_scope(const struct ui_widget *widget,
                                 const struct ui_widget *modal)
{
    return !modal || widget_is_ancestor(widget, modal) ||
           widget_is_ancestor(modal, widget);
}

static bool build_widget(const struct ui_widget *widget, uint64_t parent_id,
                         uint32_t *parent_order,
                         struct ui_accessibility_tree *tree,
                         struct ui_accessibility_tree *previous,
                         const struct ui_widget *modal)
{
    if (!widget_visible(widget) || !widget_in_modal_scope(widget, modal))
        return true;

    const struct ui_widget_ops *ops = widget->ops;
    size_t virtual_count = ops && ops->accessibility_child_count
        ? ops->accessibility_child_count(widget) : 0;
    bool has_children = widget->first_child || virtual_count;
    struct ui_accessibility_node_spec spec = {
        .role = has_children ? UI_ACCESSIBILITY_ROLE_GROUP
                             : UI_ACCESSIBILITY_ROLE_UNKNOWN,
        .bounds = widget->bounds,
    };
    bool include = false;
    if (!widget->impl.accessibility_ignored)
        include = ops && ops->accessibility
            ? ops->accessibility(widget, &spec)
            : has_children || widget->impl.focusable ||
              widget->impl.accessible_name;
    if (widget->impl.accessible_name)
        spec.name = widget->impl.accessible_name;
    if (widget->impl.accessible_description)
        spec.description = widget->impl.accessible_description;
    if (widget->impl.accessibility_invalid)
        spec.states |= UI_ACCESSIBILITY_STATE_INVALID;
    if (widget->impl.focusable) {
        spec.states |= UI_ACCESSIBILITY_STATE_FOCUSABLE;
        spec.actions |= UI_ACCESSIBILITY_ACTION_FOCUS;
    }
    if (ui_widget_has_focus(widget))
        spec.states |= UI_ACCESSIBILITY_STATE_FOCUSED;
    if (modal && !widget_is_ancestor(modal, widget)) {
        spec.states &= ~(UI_ACCESSIBILITY_STATE_FOCUSABLE |
                         UI_ACCESSIBILITY_STATE_FOCUSED);
        spec.actions = 0;
    }

    uint64_t id = widget->impl.accessibility_id;
    if (!id)
        include = false;
    uint32_t local_order = 0;
    uint32_t *child_order = include ? &local_order : parent_order;
    uint64_t child_parent = include ? id : parent_id;
    if (include) {
        if (*parent_order == UINT32_MAX)
            return false;
        spec.id = id;
        spec.parent_id = parent_id;
        spec.order = (*parent_order)++;
        if (!tree_add(tree, &spec, previous))
            return false;
    }

    for (const struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling)
        if (!build_widget(child, child_parent, child_order, tree, previous,
                          modal))
            return false;

    if ((!modal || widget_is_ancestor(modal, widget)) && ops &&
        ops->accessibility_child) {
        for (size_t i = 0; i < virtual_count; i++) {
            struct ui_accessibility_node_spec child = {0};
            if (!ops->accessibility_child(widget, i, &child))
                continue;
            if (!child.id || *child_order == UINT32_MAX)
                return false;
            child.parent_id = child_parent;
            child.order = (*child_order)++;
            if (!tree_add(tree, &child, previous))
                return false;
        }
    }
    return true;
}

bool ui_accessibility_tree_build(const struct ui_widget *root,
                                 struct ui_accessibility_tree *tree)
{
    if (!tree)
        return false;
    /* Keep the previous tree until the build ends. Unchanged values move
     * from it to the new tree. */
    struct ui_accessibility_tree previous = *tree;
    *tree = (struct ui_accessibility_tree){0};
    uint32_t root_order = 0;
    const struct ui_widget *modal = modal_in_tree(root);
    bool built = build_widget(root, 0, &root_order, tree, &previous, modal);
    ui_accessibility_tree_destroy(&previous);
    if (!built)
        ui_accessibility_tree_clear(tree);
    return built;
}

/* The topmost modal of `surface`, or NULL. The overlays come before the root,
 * because an overlay sits above it. */
static const struct ui_widget *surface_modal(const struct ui_surface *surface)
{
    for (size_t i = ui_surface_overlay_count(surface); i > 0; i--) {
        const struct ui_widget *modal =
            modal_in_tree(ui_surface_overlay_at(surface, i - 1));
        if (modal)
            return modal;
    }
    return modal_in_tree(ui_surface_root(surface));
}

bool ui_accessibility_tree_build_surface(const struct ui_surface *surface,
                                         struct ui_accessibility_tree *tree)
{
    if (!tree)
        return false;
    struct ui_accessibility_tree previous = *tree;
    *tree = (struct ui_accessibility_tree){0};
    const struct ui_widget *modal = surface_modal(surface);
    /* The root and the overlays are siblings in the published tree, so they
     * share one order. */
    uint32_t order = 0;
    bool built = build_widget(ui_surface_root(surface), 0, &order, tree,
                              &previous, modal);
    for (size_t i = 0; built && i < ui_surface_overlay_count(surface); i++)
        built = build_widget(ui_surface_overlay_at(surface, i), 0, &order,
                             tree, &previous, modal);
    ui_accessibility_tree_destroy(&previous);
    if (!built)
        ui_accessibility_tree_clear(tree);
    return built;
}

static bool dispatch_widget_action(
    struct ui_widget *widget,
    const struct ui_accessibility_action_request *request,
    const struct ui_widget *modal)
{
    if (!widget_visible(widget) || !widget_in_modal_scope(widget, modal))
        return false;
    const struct ui_widget_ops *ops = widget->ops;
    bool inside_modal = !modal || widget_is_ancestor(modal, widget);
    if (inside_modal && !widget->impl.accessibility_ignored &&
        request->node_id == widget->impl.accessibility_id) {
        struct ui_accessibility_node_spec spec = {0};
        bool described = ops && ops->accessibility &&
            ops->accessibility(widget, &spec);
        if ((ops && ops->accessibility && !described) ||
            (spec.states & (UI_ACCESSIBILITY_STATE_DISABLED |
                            UI_ACCESSIBILITY_STATE_HIDDEN)))
            return false;
        if (request->action == UI_ACCESSIBILITY_ACTION_FOCUS &&
            widget->impl.focusable) {
            ui_widget_focus(widget);
            return true;
        }
        return described && (spec.actions & request->action) &&
               ops->accessibility_action &&
               ops->accessibility_action(widget, request->node_id, request);
    }

    size_t virtual_count = ops && ops->accessibility_child_count
        ? ops->accessibility_child_count(widget) : 0;
    if (inside_modal && ops && ops->accessibility_child &&
        ops->accessibility_action) {
        for (size_t i = 0; i < virtual_count; i++) {
            struct ui_accessibility_node_spec child = {0};
            if (ops->accessibility_child(widget, i, &child) &&
                child.id == request->node_id)
                return !(child.states & (UI_ACCESSIBILITY_STATE_DISABLED |
                                         UI_ACCESSIBILITY_STATE_HIDDEN)) &&
                       (child.actions & request->action) &&
                       ops->accessibility_action(widget, request->node_id,
                                                 request);
        }
    }

    for (struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling)
        if (dispatch_widget_action(child, request, modal))
            return true;
    return false;
}

bool ui_accessibility_dispatch_action(
    struct ui_widget *root,
    const struct ui_accessibility_action_request *request)
{
    if (!root || !request || !request->node_id || !request->action ||
        ((unsigned)request->action & ((unsigned)request->action - 1)))
        return false;
    const struct ui_widget *modal = modal_in_tree(root);
    return dispatch_widget_action(root, request, modal);
}

bool ui_accessibility_dispatch_surface_action(
    struct ui_surface *surface,
    const struct ui_accessibility_action_request *request)
{
    if (!surface || !request || !request->node_id || !request->action ||
        ((unsigned)request->action & ((unsigned)request->action - 1)))
        return false;
    const struct ui_widget *modal = surface_modal(surface);
    if (dispatch_widget_action(ui_surface_root(surface), request, modal))
        return true;
    for (size_t i = 0; i < ui_surface_overlay_count(surface); i++)
        if (dispatch_widget_action(ui_surface_overlay_at(surface, i), request,
                                   modal))
            return true;
    return false;
}

bool ui_accessibility_provider_attach(
    struct ui_accessibility_provider *provider,
    const struct ui_accessibility_provider_ops *ops, void *native_handle,
    ui_accessibility_action_sink_fn action_sink, void *action_userdata)
{
    if (!provider || provider->ops || provider->instance || !ops ||
        !ops->attach || !ops->publish_snapshot || !ops->detach)
        return false;

    void *instance = ops->attach(native_handle, action_sink, action_userdata);
    if (!instance)
        return false;
    provider->ops = ops;
    provider->instance = instance;
    return true;
}

bool ui_accessibility_provider_publish_snapshot(
    const struct ui_accessibility_provider *provider,
    const struct ui_accessibility_tree *snapshot)
{
    if (!provider || !provider->ops || !provider->ops->publish_snapshot ||
        !snapshot)
        return false;
    return provider->ops->publish_snapshot(provider->instance, snapshot);
}

bool ui_accessibility_provider_is_active(
    const struct ui_accessibility_provider *provider)
{
    if (!provider || !provider->ops || !provider->instance)
        return false;
    if (provider->ops->is_active)
        return provider->ops->is_active(provider->instance);
    return true;
}

void ui_accessibility_provider_detach(
    struct ui_accessibility_provider *provider)
{
    if (!provider)
        return;
    if (provider->ops && provider->ops->detach)
        provider->ops->detach(provider->instance);
    *provider = (struct ui_accessibility_provider){0};
}
