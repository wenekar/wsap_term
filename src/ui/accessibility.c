#include "ui/accessibility.h"

#include "ui/widget.h"
#include "ui/widget_impl.h"
#include "wsap_vtcore/unicode/utf8.h"
#include "util/hash.h"

#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *src)
{
    return src ? wsap_vtcore_utf8_sanitize_dup(src) : NULL;
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
        int bytes = wsap_vtcore_utf8_decode_one(text + source, length - source,
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

bool ui_accessibility_tree_add(struct ui_accessibility_tree *tree,
                               const struct ui_accessibility_node_spec *spec)
{
    if (!tree || !spec || !spec->id ||
        ui_accessibility_tree_find(tree, spec->id) ||
        (spec->parent_id && !ui_accessibility_tree_find(tree, spec->parent_id)) ||
        tree->count == SIZE_MAX || !tree_reserve(tree, tree->count + 1) ||
        !index_reserve(tree, tree->count + 1))
        return false;

    struct ui_accessibility_node node = {
        .id = spec->id,
        .parent_id = spec->parent_id,
        .order = spec->order,
        .role = spec->role,
        .states = spec->states,
        .actions = spec->actions,
        .bounds = spec->bounds,
        .caret_offset = spec->states & UI_ACCESSIBILITY_STATE_PASSWORD
            ? 0 : sanitized_offset(spec->value, spec->caret_offset),
        .selection_start = spec->states & UI_ACCESSIBILITY_STATE_PASSWORD
            ? 0 : sanitized_offset(spec->value, spec->selection_start),
        .selection_end = spec->states & UI_ACCESSIBILITY_STATE_PASSWORD
            ? 0 : sanitized_offset(spec->value, spec->selection_end),
    };
    node.name = copy_string(spec->name);
    node.description = copy_string(spec->description);
    node.value = copy_string(
        spec->states & UI_ACCESSIBILITY_STATE_PASSWORD ? NULL : spec->value);
    if ((spec->name && !node.name) ||
        (spec->description && !node.description) ||
        (spec->value &&
         !(spec->states & UI_ACCESSIBILITY_STATE_PASSWORD) && !node.value)) {
        free_node(&node);
        return false;
    }

    size_t slot = index_slot(tree, node.id);
    tree->nodes[tree->count++] = node;
    tree->id_index[slot] = tree->count;
    return true;
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

bool ui_accessibility_tree_copy(struct ui_accessibility_tree *dst,
                                 const struct ui_accessibility_tree *src)
{
    if (!dst || !src)
        return false;
    if (dst == src)
        return true;
    struct ui_accessibility_tree copy = {0};
    for (size_t i = 0; i < src->count; i++) {
        const struct ui_accessibility_node *node = &src->nodes[i];
        const struct ui_accessibility_node_spec spec = {
            .id = node->id,
            .parent_id = node->parent_id,
            .order = node->order,
            .role = node->role,
            .states = node->states,
            .actions = node->actions,
            .bounds = node->bounds,
            .name = node->name,
            .description = node->description,
            .value = node->value,
            .caret_offset = node->caret_offset,
            .selection_start = node->selection_start,
            .selection_end = node->selection_end,
        };
        if (!ui_accessibility_tree_add(&copy, &spec)) {
            ui_accessibility_tree_destroy(&copy);
            return false;
        }
    }
    ui_accessibility_tree_destroy(dst);
    *dst = copy;
    return true;
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
        if (!ui_accessibility_tree_add(tree, &spec))
            return false;
    }

    for (const struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling)
        if (!build_widget(child, child_parent, child_order, tree, modal))
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
            if (!ui_accessibility_tree_add(tree, &child))
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
    ui_accessibility_tree_clear(tree);
    uint32_t root_order = 0;
    const struct ui_widget *modal = modal_in_tree(root);
    if (!build_widget(root, 0, &root_order, tree, modal)) {
        ui_accessibility_tree_clear(tree);
        return false;
    }
    return true;
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
