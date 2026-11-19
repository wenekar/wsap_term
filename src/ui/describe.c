#include "ui/describe.h"
#include "util/app_identity.h"

#include "ui/surface.h"
#include "ui/widget_ops.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_out_of_memory(void);

#define UI_VIEW_MAX_DEPTH 128

struct ui_view_node {
    /* The descriptor of the node. NULL once the view holds the node. */
    const struct ui_node_desc *desc;
    uint32_t parent;
    char *key;
    char *class_name;
    char *id_name;
    char *tag_name;
    void *storage;
    struct ui_widget *widget;
    /* The slots that the view wrote. It clears them when the node leaves. */
    struct ui_node_bindings bind;
    enum ui_node_kind kind;
};

struct ui_view {
    struct ui_view_node *nodes;
    size_t count;
    size_t capacity;
    struct ui_widget *root;
    bool committed;
    /* The view destroys its own widgets, so it ignores its observer. */
    bool destroying;
    /* An update runs. A handler that it calls cannot update the view, and a
     * destroy waits until the update ends. */
    bool updating;
    bool destroy_pending;
};

/* A menu and the layer that shows it. */
struct ui_menu_node {
    struct ui_menu menu;
    struct ui_menu_layer layer;
};

struct validate_context {
    struct ui_view *view;
    enum ui_view_error error;
    const struct ui_node_desc *failed_node;
    const char *detail;
    const struct ui_node_desc *path[UI_VIEW_MAX_DEPTH];
};

static char *copy_string(const char *text)
{
    if (!text)
        return NULL;
    size_t len = strlen(text);
    if (len == SIZE_MAX)
        return NULL;
    char *copy = malloc(len + 1);
    if (copy)
        memcpy(copy, text, len + 1);
    return copy;
}

enum node_flag {
    NODE_SUPPORTED = 1u << 0,
    /* The node is a box that lays out child widgets. */
    NODE_CONTAINER = 1u << 1,
    /* The node accepts descriptor children. */
    NODE_CHILDREN = 1u << 2,
};

struct node_kind_info {
    const char *class_name;
    const char *tag_name;
    size_t storage_size;
    unsigned flags;
};

#define NODE_KIND(class, tag, type, extra) \
    { .class_name = class, .tag_name = tag, .storage_size = sizeof(type), \
      .flags = NODE_SUPPORTED | (extra) }

/* One row per node kind. An unsupported kind leaves its row zero. */
static const struct node_kind_info NODE_KINDS[] = {
    [UI_NODE_ROW] = NODE_KIND(NULL, "box", struct ui_box,
                              NODE_CONTAINER | NODE_CHILDREN),
    [UI_NODE_COLUMN] = NODE_KIND(NULL, "box", struct ui_box,
                                 NODE_CONTAINER | NODE_CHILDREN),
    [UI_NODE_STACK] = NODE_KIND(NULL, "box", struct ui_box,
                                NODE_CONTAINER | NODE_CHILDREN),
    [UI_NODE_LABEL] = NODE_KIND("label", "label", struct ui_label, 0),
    [UI_NODE_BUTTON] = NODE_KIND("button", "button", struct ui_button, 0),
    [UI_NODE_INPUT] = NODE_KIND("text-input", "input", struct ui_input, 0),
    [UI_NODE_DROPDOWN] = NODE_KIND("dropdown", "dropdown",
                                   struct ui_dropdown, 0),
    [UI_NODE_SCROLLVIEW] = NODE_KIND("scrollview", "scrollview",
                                     struct ui_scrollview, NODE_CHILDREN),
    /* The list sets its own style class from the node class name. */
    [UI_NODE_LIST] = NODE_KIND(NULL, NULL, struct ui_list, 0),
    [UI_NODE_MENU] = NODE_KIND("menu", "menu", struct ui_menu_node, 0),
    /* A supplied widget has no storage of its own. */
    [UI_NODE_WIDGET] = { .flags = NODE_SUPPORTED | NODE_CHILDREN },
};

#undef NODE_KIND

static const struct node_kind_info *node_kind_info(enum ui_node_kind kind)
{
    size_t index = (size_t)kind;
    if (index >= sizeof(NODE_KINDS) / sizeof(*NODE_KINDS) ||
        !(NODE_KINDS[index].flags & NODE_SUPPORTED))
        return NULL;
    return &NODE_KINDS[index];
}

static bool node_has_flag(enum ui_node_kind kind, unsigned flag)
{
    const struct node_kind_info *info = node_kind_info(kind);
    return info && (info->flags & flag);
}

static bool node_is_container(enum ui_node_kind kind)
{
    return node_has_flag(kind, NODE_CONTAINER);
}

static bool node_has_children(enum ui_node_kind kind)
{
    return node_has_flag(kind, NODE_CHILDREN);
}

static bool node_kind_valid(enum ui_node_kind kind)
{
    return node_kind_info(kind) != NULL;
}

static bool node_props_valid(const struct ui_node_desc *desc,
                             enum ui_view_error *error, const char **detail)
{
    if ((desc->bind.box && !node_is_container(desc->kind)) ||
        (desc->bind.label && desc->kind != UI_NODE_LABEL) ||
        (desc->bind.button && desc->kind != UI_NODE_BUTTON) ||
        (desc->bind.input && desc->kind != UI_NODE_INPUT) ||
        (desc->bind.dropdown && desc->kind != UI_NODE_DROPDOWN) ||
        (desc->bind.scrollview && desc->kind != UI_NODE_SCROLLVIEW) ||
        (desc->bind.list && desc->kind != UI_NODE_LIST) ||
        (desc->bind.menu && desc->kind != UI_NODE_MENU)) {
        *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
        *detail = "Binding does not match the node kind";
        return false;
    }
    switch (desc->kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK:
        return true;
    case UI_NODE_LABEL:
        if (desc->props.label.wrap > UI_LABEL_ELLIPSIS ||
            desc->props.label.align > UI_LABEL_ALIGN_RIGHT) {
            *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            *detail = "Invalid label wrap or alignment";
            return false;
        }
        return true;
    case UI_NODE_BUTTON:
    case UI_NODE_INPUT:
        return true;
    case UI_NODE_DROPDOWN:
        if (desc->props.dropdown.count > INT_MAX ||
            (desc->props.dropdown.count && !desc->props.dropdown.items)) {
            *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            *detail = "Invalid dropdown items or count";
            return false;
        }
        return true;
    case UI_NODE_SCROLLVIEW:
        if (desc->props.scrollview.scroll_step < 0) {
            *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            *detail = "Scroll step cannot be negative";
            return false;
        }
        return true;
    case UI_NODE_LIST:
        if (desc->props.list.count < 0 ||
            (desc->props.list.count && !desc->props.list.items)) {
            *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            *detail = "Invalid list items or count";
            return false;
        }
        return true;
    case UI_NODE_MENU:
        if (desc->props.menu.count < 0 ||
            (desc->props.menu.count && !desc->props.menu.items)) {
            *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            *detail = "Invalid menu items or count";
            return false;
        }
        return true;
    case UI_NODE_WIDGET:
        if (!desc->props.widget ||
            !ui_widget_accessibility_id(desc->props.widget) ||
            desc->props.widget->impl.destroyed ||
            desc->props.widget->parent ||
            ui_widget_surface(desc->props.widget)) {
            *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            *detail = "Widget must be initialized and detached";
            return false;
        }
        return true;
    }
    *error = UI_VIEW_ERROR_INVALID_KIND;
    *detail = "Node kind is not supported";
    return false;
}

static bool append_node(struct ui_view *view,
                        const struct ui_node_desc *desc, uint32_t parent,
                        uint32_t *index)
{
    if (view->count >= UINT32_MAX)
        return false;
    if (view->count == view->capacity) {
        size_t capacity = view->capacity ? view->capacity * 2 : 16;
        if (capacity < view->capacity ||
            capacity > SIZE_MAX / sizeof(*view->nodes))
            return false;
        struct ui_view_node *nodes = realloc(view->nodes,
                                               capacity * sizeof(*nodes));
        if (!nodes)
            return false;
        view->nodes = nodes;
        view->capacity = capacity;
    }
    *index = (uint32_t)view->count++;
    view->nodes[*index] = (struct ui_view_node){
        .desc = desc,
        .parent = parent,
        .kind = desc->kind,
    };
    return true;
}

static bool validate_node(struct validate_context *context,
                          const struct ui_node_desc *desc, uint32_t parent,
                          size_t depth)
{
    context->failed_node = desc;
    if (!desc) {
        context->error = UI_VIEW_ERROR_INVALID_ARGUMENT;
        context->detail = "Descriptor is NULL";
        return false;
    }
    if (depth >= UI_VIEW_MAX_DEPTH) {
        context->error = UI_VIEW_ERROR_INVALID_CHILDREN;
        context->detail = "Descriptor tree is too deep";
        return false;
    }
    for (size_t i = 0; i < depth; i++) {
        if (context->path[i] == desc) {
            context->error = UI_VIEW_ERROR_INVALID_CHILDREN;
            context->detail = "Descriptor tree contains a cycle";
            return false;
        }
    }
    if (!node_kind_valid(desc->kind)) {
        context->error = UI_VIEW_ERROR_INVALID_KIND;
        context->detail = "Node kind is not supported";
        return false;
    }
    if (desc->key && !desc->key[0]) {
        context->error = UI_VIEW_ERROR_INVALID_ARGUMENT;
        context->detail = "Node key is empty";
        return false;
    }
    if (desc->child_count && !desc->children) {
        context->error = UI_VIEW_ERROR_INVALID_CHILDREN;
        context->detail = "Child count is set but children is NULL";
        return false;
    }
    if (!node_has_children(desc->kind) && desc->child_count) {
        context->error = UI_VIEW_ERROR_INVALID_CHILDREN;
        context->detail = "Node kind does not accept children";
        return false;
    }
    if (desc->kind == UI_NODE_SCROLLVIEW && desc->child_count > 1) {
        context->error = UI_VIEW_ERROR_INVALID_CHILDREN;
        context->detail = "Scrollview accepts at most one child";
        return false;
    }
    if (!node_props_valid(desc, &context->error, &context->detail))
        return false;

    if (desc->key) {
        for (size_t i = 0; i < context->view->count; i++) {
            const char *key = context->view->nodes[i].desc->key;
            if (key && !strcmp(key, desc->key)) {
                context->error = UI_VIEW_ERROR_DUPLICATE_KEY;
                context->detail = "Node key occurs more than once";
                return false;
            }
        }
    }

    if (desc->kind == UI_NODE_WIDGET) {
        for (size_t i = 0; i < context->view->count; i++) {
            const struct ui_node_desc *other = context->view->nodes[i].desc;
            if (other->kind == UI_NODE_WIDGET &&
                other->props.widget == desc->props.widget) {
                context->error = UI_VIEW_ERROR_INVALID_ARGUMENT;
                context->detail = "Widget occurs more than once";
                return false;
            }
        }
    }

    uint32_t index;
    if (!append_node(context->view, desc, parent, &index)) {
        context->error = UI_VIEW_ERROR_OUT_OF_MEMORY;
        context->detail = "Cannot allocate descriptor list";
        return false;
    }

    context->path[depth] = desc;
    for (size_t i = 0; i < desc->child_count; i++) {
        if (!validate_node(context, &desc->children[i], index, depth + 1))
            return false;
    }
    return true;
}

/* Checks the descriptor tree and returns its nodes in pre-order. */
static struct ui_view_node *plan_nodes(const struct ui_node_desc *root,
                                       size_t *count,
                                       struct ui_view_diagnostic *diagnostic)
{
    struct ui_view plan = {0};
    struct validate_context context = { .view = &plan };
    if (!validate_node(&context, root, UINT32_MAX, 0)) {
        if (diagnostic) {
            diagnostic->error = context.error;
            diagnostic->node = context.failed_node;
            diagnostic->detail = context.detail;
        }
        free(plan.nodes);
        return NULL;
    }
    *count = plan.count;
    return plan.nodes;
}

static struct ui_view_node *find_node(struct ui_view *view, const char *key)
{
    if (!view || !key)
        return NULL;
    for (size_t i = 0; i < view->count; i++) {
        if (view->nodes[i].key && !strcmp(view->nodes[i].key, key))
            return &view->nodes[i];
    }
    return NULL;
}

static const char *default_class(enum ui_node_kind kind)
{
    const struct node_kind_info *info = node_kind_info(kind);
    return info ? info->class_name : NULL;
}

static const char *default_tag(enum ui_node_kind kind)
{
    const struct node_kind_info *info = node_kind_info(kind);
    return info ? info->tag_name : NULL;
}

static void free_node_strings(struct ui_view_node *node)
{
    free(node->key);
    free(node->class_name);
    free(node->id_name);
    free(node->tag_name);
    node->key = NULL;
    node->class_name = NULL;
    node->id_name = NULL;
    node->tag_name = NULL;
}

/* Frees the strings and the storage of `node`. Call it after the node widget
 * is destroyed. The layer of a menu node holds only the shown levels, so the
 * menu destroys its other levels here. */
static void free_node(struct ui_view_node *node)
{
    free_node_strings(node);
    if (node->kind == UI_NODE_MENU && node->storage)
        ui_menu_destroy(&((struct ui_menu_node *)node->storage)->menu);
    free(node->storage);
    node->storage = NULL;
}

static bool copy_node_strings(struct ui_view_node *node)
{
    const struct ui_node_desc *desc = node->desc;
    node->key = copy_string(desc->key);
    node->class_name = copy_string(desc->class_name
                                   ? desc->class_name
                                   : default_class(desc->kind));
    node->id_name = copy_string(desc->id_name);
    node->tag_name = copy_string(desc->tag_name
                                 ? desc->tag_name
                                 : default_tag(desc->kind));
    return (!desc->key || node->key) &&
           (!(desc->class_name || default_class(desc->kind)) || node->class_name) &&
           (!desc->id_name || node->id_name) &&
           (!(desc->tag_name || default_tag(desc->kind)) || node->tag_name);
}

static size_t widget_size(enum ui_node_kind kind)
{
    const struct node_kind_info *info = node_kind_info(kind);
    return info ? info->storage_size : 0;
}

/* Allocates the storage of `node` and initializes its widget. A supplied
 * widget node adopts its widget as it is. */
static bool node_create(struct ui_view_node *node, enum ui_view_error *error)
{
    enum ui_node_kind kind = node->kind;
    if (kind == UI_NODE_WIDGET) {
        node->widget = node->desc->props.widget;
        return true;
    }
    node->storage = calloc(1, widget_size(kind));
    if (!node->storage) {
        *error = UI_VIEW_ERROR_OUT_OF_MEMORY;
        return false;
    }
    switch (kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK: {
        struct ui_box *box = node->storage;
        ui_box_init(box);
        node->widget = &box->widget;
        ui_box_set_direction(box, kind == UI_NODE_COLUMN ? UI_BOX_COLUMN
                                   : kind == UI_NODE_STACK ? UI_BOX_STACK
                                                           : UI_BOX_ROW);
        break;
    }
    case UI_NODE_LABEL: {
        struct ui_label *label = node->storage;
        ui_label_init(label);
        node->widget = &label->widget;
        break;
    }
    case UI_NODE_BUTTON: {
        struct ui_button *button = node->storage;
        ui_button_init(button);
        node->widget = &button->widget;
        break;
    }
    case UI_NODE_INPUT: {
        struct ui_input *input = node->storage;
        ui_input_init(input);
        node->widget = &input->widget;
        break;
    }
    case UI_NODE_DROPDOWN: {
        struct ui_dropdown *dropdown = node->storage;
        ui_dropdown_init(dropdown);
        node->widget = &dropdown->widget;
        break;
    }
    case UI_NODE_SCROLLVIEW: {
        struct ui_scrollview *scrollview = node->storage;
        ui_scrollview_init(scrollview);
        node->widget = &scrollview->widget;
        break;
    }
    case UI_NODE_LIST: {
        struct ui_list *list = node->storage;
        ui_list_init(list);
        node->widget = &list->widget;
        break;
    }
    case UI_NODE_MENU: {
        struct ui_menu_node *menu = node->storage;
        ui_menu_init(&menu->menu);
        ui_menu_layer_init(&menu->layer, &menu->menu);
        node->widget = &menu->layer.widget;
        break;
    }
    case UI_NODE_WIDGET:
        return true;
    }
    return true;
}

static bool set_list_props(struct ui_list *list,
                           const struct ui_node_desc *node,
                           enum ui_view_error *error)
{
    const struct ui_node_desc *desc = node;
    ui_list_set_style_class(list, desc->class_name);
    ui_list_set_wrap_navigation(list, desc->props.list.wrap_navigation);
    /* No items empty the list. */
    if (!ui_list_set_items(list, desc->props.list.items,
                           desc->props.list.count)) {
        *error = UI_VIEW_ERROR_OUT_OF_MEMORY;
        return false;
    }
    if (desc->props.list.selected >= 0)
        ui_list_select(list, desc->props.list.selected);
    return true;
}

static bool set_menu_props(struct ui_menu_node *menu,
                           const struct ui_node_desc *desc,
                           enum ui_view_error *error)
{
    ui_menu_set_action_handler(&menu->menu, desc->on_action,
                               desc->action_userdata);
    ui_menu_set_submenu_provider(&menu->menu, desc->props.menu.on_submenu,
                                 desc->props.menu.submenu_userdata);
    ui_menu_set_hover_delay(&menu->menu, desc->props.menu.hover_delay_ms);
    ui_menu_layer_set_origin(&menu->layer, desc->props.menu.pos);
    /* An open menu with the same items stays open with its submenus. */
    if (ui_menu_is_open(&menu->menu) &&
        ui_list_has_items(&menu->menu.levels[0].list, desc->props.menu.items,
                          desc->props.menu.count))
        return true;
    if (!desc->props.menu.count) {
        ui_menu_close(&menu->menu);
        return true;
    }
    if (!ui_menu_open(&menu->menu, desc->props.menu.items,
                      desc->props.menu.count)) {
        *error = UI_VIEW_ERROR_INIT_FAILED;
        return false;
    }
    return true;
}

/* Applies the descriptor to the widget of `node`. */
static bool node_set_props(struct ui_view_node *node,
                           enum ui_view_error *error)
{
    const struct ui_node_desc *desc = node->desc;
    /* A list sets its style class from the node class name. */
    if (node->kind != UI_NODE_WIDGET && node->kind != UI_NODE_LIST)
        ui_widget_set_style_identity(node->widget, node->class_name,
                                     node->id_name, node->tag_name);
    ui_widget_set_debug_key(node->widget, node->key);
    ui_widget_set_action_handler(node->widget, desc->on_action,
                                 desc->action_userdata);
    ui_widget_set_event_handler(node->widget, desc->on_event,
                                desc->event_userdata);

    switch (node->kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK:
        return true;
    case UI_NODE_LABEL: {
        struct ui_label *label = node->storage;
        if (!ui_label_set_text(label, desc->props.label.text) ||
            (desc->props.label.link &&
             !ui_label_set_link(label, desc->props.label.link))) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        ui_label_set_wrap(label, desc->props.label.wrap);
        ui_label_set_align(label, desc->props.label.align);
        ui_label_set_selectable(label, desc->props.label.selectable);
        ui_label_set_disabled(label, desc->props.label.disabled);
        ui_label_set_action_id(label, desc->action_id);
        return true;
    }
    case UI_NODE_BUTTON: {
        struct ui_button *button = node->storage;
        if (!ui_button_set_text(button, desc->props.button.text)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        ui_button_set_disabled(button, desc->props.button.disabled);
        ui_button_set_action_id(button, desc->action_id);
        return true;
    }
    case UI_NODE_INPUT: {
        struct ui_input *input = node->storage;
        if (!ui_input_set_text(input, desc->props.input.text) ||
            !ui_input_set_placeholder(input, desc->props.input.placeholder)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        if (desc->props.input.accessible_name &&
            !ui_widget_set_accessible_name(&input->widget,
                desc->props.input.accessible_name)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        ui_input_set_action_id(input, desc->action_id);
        return true;
    }
    case UI_NODE_DROPDOWN: {
        struct ui_dropdown *dropdown = node->storage;
        if (!ui_dropdown_set_items(dropdown, desc->props.dropdown.items,
                                   desc->props.dropdown.count)) {
            *error = UI_VIEW_ERROR_OUT_OF_MEMORY;
            return false;
        }
        if (desc->props.dropdown.selected_value &&
            !ui_dropdown_set_value(dropdown,
                                   desc->props.dropdown.selected_value)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        if (desc->props.dropdown.accessible_name &&
            !ui_widget_set_accessible_name(&dropdown->widget,
                desc->props.dropdown.accessible_name)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        ui_dropdown_set_action_id(dropdown, desc->action_id);
        return true;
    }
    case UI_NODE_SCROLLVIEW: {
        struct ui_scrollview *scrollview = node->storage;
        ui_scrollview_set_scrollbar(scrollview,
                                    desc->props.scrollview.scrollbar);
        ui_scrollview_set_scroll_step(scrollview,
                                      desc->props.scrollview.scroll_step);
        return true;
    }
    case UI_NODE_LIST:
        return set_list_props(node->storage, desc, error);
    case UI_NODE_MENU:
        return set_menu_props(node->storage, desc, error);
    case UI_NODE_WIDGET:
        return true;
    }
    *error = UI_VIEW_ERROR_INVALID_KIND;
    return false;
}

/* Initializes a node that the view does not hold yet. */
static bool node_init(struct ui_view_node *node, enum ui_view_error *error)
{
    if (!copy_node_strings(node)) {
        *error = UI_VIEW_ERROR_OUT_OF_MEMORY;
        return false;
    }
    node->bind = node->desc->bind;
    if (!node_create(node, error))
        return false;
    return node_set_props(node, error);
}

/* Clears each handle slot that still points at the storage of `node`. */
static void clear_bind_slots(const struct ui_view_node *node)
{
    const struct ui_node_bindings *bind = &node->bind;
    if (bind->box && *bind->box == node->storage)
        *bind->box = NULL;
    if (bind->label && *bind->label == node->storage)
        *bind->label = NULL;
    if (bind->button && *bind->button == node->storage)
        *bind->button = NULL;
    if (bind->input && *bind->input == node->storage)
        *bind->input = NULL;
    if (bind->dropdown && *bind->dropdown == node->storage)
        *bind->dropdown = NULL;
    if (bind->scrollview && *bind->scrollview == node->storage)
        *bind->scrollview = NULL;
    if (bind->list && *bind->list == node->storage)
        *bind->list = NULL;
    if (bind->menu && *bind->menu == node->storage)
        *bind->menu = NULL;
}

static void write_bind_slots(const struct ui_view_node *node)
{
    const struct ui_node_bindings *bind = &node->bind;
    if (bind->box) *bind->box = node->storage;
    if (bind->label) *bind->label = node->storage;
    if (bind->button) *bind->button = node->storage;
    if (bind->input) *bind->input = node->storage;
    if (bind->dropdown) *bind->dropdown = node->storage;
    if (bind->scrollview) *bind->scrollview = node->storage;
    if (bind->list) *bind->list = node->storage;
    if (bind->menu) *bind->menu = node->storage;
}

/* The storage of nodes that the view released. A busy surface frees it in a
 * post, because a handler can run an update. */
struct ui_view_garbage {
    struct ui_view_node *nodes;
    size_t count;
};

static void free_garbage(struct ui_surface *surface, void *userdata)
{
    (void)surface;
    struct ui_view_garbage *garbage = userdata;
    for (size_t i = 0; i < garbage->count; i++)
        free_node(&garbage->nodes[i]);
    free(garbage->nodes);
    free(garbage);
}

/* Frees the strings and the storage of `count` nodes. The widgets are gone.
 * The call frees now, or posts the free when the surface is busy. */
static void dispose_nodes(struct ui_surface *surface, struct ui_view_node *nodes,
                          size_t count)
{
    if (!surface || !ui_surface_busy(surface)) {
        for (size_t i = 0; i < count; i++)
            free_node(&nodes[i]);
        free(nodes);
        return;
    }
    struct ui_view_garbage *garbage = calloc(1, sizeof(*garbage));
    if (garbage) {
        garbage->nodes = nodes;
        garbage->count = count;
        if (ui_surface_post(surface, free_garbage, garbage))
            return;
        free(garbage);
    }
    /* No memory for the queue. Freeing now would break the rule that no
     * storage is freed while the surface is busy, so it stays. */
    log_out_of_memory();
}

/* Reports an allocation failure that costs a queue entry. */
static void log_out_of_memory(void)
{
    const struct app_identity *identity = app_identity_current();
    fprintf(stderr, "%s: view storage stays: out of memory\n",
            identity ? identity->id : "ui");
}

/* Drops the nodes that a failed update created. A supplied widget goes back
 * to its caller, because the view does not adopt it. */
static void discard_nodes(struct ui_surface *surface, struct ui_view_node *nodes,
                          size_t count, const size_t *match)
{
    for (size_t i = 0; i < count; i++) {
        if (match && match[i] != SIZE_MAX) {
            /* The view keeps this node. Free the copy of the strings only. */
            free_node_strings(&nodes[i]);
            nodes[i] = (struct ui_view_node){0};
            continue;
        }
        if (!nodes[i].widget)
            continue;
        if (nodes[i].kind == UI_NODE_WIDGET) {
            if (nodes[i].widget->parent)
                ui_widget_remove_child(nodes[i].widget->parent,
                                       nodes[i].widget);
        } else {
            ui_widget_destroy(nodes[i].widget);
        }
        nodes[i].widget = NULL;
    }
    dispose_nodes(surface, nodes, count);
}

/* Drops the nodes that an update matched away. */
static void retire_nodes(struct ui_surface *surface, struct ui_view_node *nodes,
                         size_t count, const bool *kept)
{
    for (size_t i = 0; i < count; i++) {
        if (kept[i]) {
            /* The view holds its own strings now. */
            free_node_strings(&nodes[i]);
            nodes[i] = (struct ui_view_node){0};
            continue;
        }
        clear_bind_slots(&nodes[i]);
        if (nodes[i].widget) {
            ui_widget_destroy(nodes[i].widget);
            nodes[i].widget = NULL;
        }
    }
    dispose_nodes(surface, nodes, count);
}

static void view_widget_destroyed(struct ui_widget *widget, void *userdata)
{
    struct ui_view *view = userdata;
    if (!view || view->destroying)
        return;
    for (size_t i = 0; i < view->count; i++) {
        if (view->nodes[i].widget != widget)
            continue;
        view->nodes[i].widget = NULL;
        clear_bind_slots(&view->nodes[i]);
        if (view->root == widget)
            view->root = NULL;
        return;
    }
}

static void observe_widgets(struct ui_view *view)
{
    for (size_t i = 0; i < view->count; i++)
        if (view->nodes[i].widget)
            ui_widget_set_destroy_observer(view->nodes[i].widget,
                                           view_widget_destroyed, view);
}

static void free_view(struct ui_surface *surface, void *userdata)
{
    (void)surface;
    struct ui_view *view = userdata;
    for (size_t i = 0; i < view->count; i++)
        free_node(&view->nodes[i]);
    free(view->nodes);
    free(view);
}

void ui_view_destroy(struct ui_view *view)
{
    if (!view)
        return;
    if (view->updating) {
        view->destroy_pending = true;
        return;
    }
    if (view->destroying)
        return;
    view->destroying = true;
    struct ui_surface *surface = NULL;
    if (view->root) {
        surface = ui_widget_surface(view->root);
        if (surface && ui_surface_root(surface) == view->root)
            ui_surface_set_root(surface, NULL);
    }
    /* On failure, return supplied widgets to their caller before the owned
     * tree is destroyed. A successful mount adopts their widget resources. */
    if (!view->committed) {
        for (size_t i = 0; i < view->count; i++) {
            struct ui_view_node *node = &view->nodes[i];
            if (node->kind == UI_NODE_WIDGET && node->widget &&
                node->widget->parent)
                ui_widget_remove_child(node->widget->parent, node->widget);
        }
    }
    for (size_t i = 0; i < view->count; i++) {
        struct ui_view_node *node = &view->nodes[i];
        clear_bind_slots(node);
        if (node->widget &&
            (view->committed || node->kind != UI_NODE_WIDGET))
            ui_widget_destroy(node->widget);
        node->widget = NULL;
    }
    /* The widgets are gone. The storage waits when the surface still reads
     * its tree, because a widget handler can call this function. */
    if (surface && ui_surface_busy(surface)) {
        if (ui_surface_post(surface, free_view, view))
            return;
        /* No memory for the queue. Freeing now would break the rule that no
         * storage is freed while the surface is busy, so it stays. */
        log_out_of_memory();
        return;
    }
    free_view(surface, view);
}

const char *ui_view_error_message(enum ui_view_error error)
{
    switch (error) {
    case UI_VIEW_ERROR_NONE: return "No error";
    case UI_VIEW_ERROR_INVALID_ARGUMENT: return "Invalid argument or property";
    case UI_VIEW_ERROR_INVALID_KIND: return "Invalid node kind";
    case UI_VIEW_ERROR_INVALID_CHILDREN: return "Invalid children";
    case UI_VIEW_ERROR_DUPLICATE_KEY: return "Duplicate key";
    case UI_VIEW_ERROR_OUT_OF_MEMORY: return "Out of memory";
    case UI_VIEW_ERROR_INIT_FAILED: return "Widget initialization failed";
    case UI_VIEW_ERROR_BUSY: return "View update is running";
    }
    return "Unknown error";
}

/* True when a mounted node takes over the descriptor node. */
static bool node_matchable(const struct ui_view_node *node,
                           const struct ui_node_desc *desc)
{
    if (node->kind != desc->kind || !node->widget)
        return false;
    if (desc->kind == UI_NODE_WIDGET &&
        node->widget != desc->props.widget)
        return false;
    if (desc->key)
        return node->key && !strcmp(node->key, desc->key);
    return !node->key;
}

/* The mounted node at child `ordinal` of `parent`, or SIZE_MAX. */
static size_t mounted_child(const struct ui_view *view, uint32_t parent,
                            size_t ordinal)
{
    size_t seen = 0;
    for (size_t i = 0; i < view->count; i++) {
        if (view->nodes[i].parent != parent)
            continue;
        if (seen++ == ordinal)
            return i;
    }
    return SIZE_MAX;
}

/* The index of plan node `i` among the children of `parent`. */
static size_t plan_ordinal(const struct ui_view_node *nodes, size_t i,
                           uint32_t parent)
{
    size_t ordinal = 0;
    for (size_t k = 0; k < i; k++)
        if (nodes[k].parent == parent)
            ordinal++;
    return ordinal;
}

/* Fills `match` with the mounted node of each plan node, or SIZE_MAX. A node
 * matches inside the subtree of its matched parent only. */
static void match_nodes(const struct ui_view *view, const struct ui_view_node *plan,
                        size_t count, size_t *match, bool *taken)
{
    for (size_t i = 0; i < count; i++) {
        match[i] = SIZE_MAX;
        uint32_t parent = UINT32_MAX;
        size_t ordinal = 0;
        if (i) {
            uint32_t plan_parent = plan[i].parent;
            if (match[plan_parent] == SIZE_MAX)
                continue;
            parent = (uint32_t)match[plan_parent];
            ordinal = plan_ordinal(plan, i, plan_parent);
        }
        if (plan[i].desc->key) {
            for (size_t j = 0; j < view->count; j++) {
                if (taken[j] || view->nodes[j].parent != parent)
                    continue;
                if (node_matchable(&view->nodes[j], plan[i].desc)) {
                    match[i] = j;
                    break;
                }
            }
        } else {
            size_t j = mounted_child(view, parent, ordinal);
            if (j != SIZE_MAX && !taken[j] &&
                node_matchable(&view->nodes[j], plan[i].desc))
                match[i] = j;
        }
        if (match[i] != SIZE_MAX)
            taken[match[i]] = true;
    }
}

/* True when each container accepts the children of the plan. `failed` names
 * the first node that does not fit. */
static bool child_limits_ok(const struct ui_view_node *nodes, size_t count,
                            size_t *failed)
{
    for (size_t p = 0; p < count; p++) {
        const struct ui_widget *widget = nodes[p].widget;
        if (!widget || !widget->ops || !widget->ops->child_limit)
            continue;
        size_t children = 0;
        for (size_t i = 1; i < count; i++) {
            if (nodes[i].parent != p)
                continue;
            if (++children > widget->ops->child_limit) {
                *failed = i;
                return false;
            }
        }
    }
    return true;
}

static bool widget_is_ancestor(const struct ui_widget *ancestor,
                               const struct ui_widget *widget)
{
    for (const struct ui_widget *p = widget; p; p = p->parent)
        if (p == ancestor)
            return true;
    return false;
}

/* True when place_nodes can insert each node widget in its parent. The check
 * reads the tree and changes nothing, so call it before the update changes a
 * kept widget. `failed` names the first node that does not fit. */
static bool placement_ok(const struct ui_view_node *nodes, size_t count,
                         size_t *failed)
{
    *failed = 0;
    if (!child_limits_ok(nodes, count, failed))
        return false;
    for (size_t p = 0; p < count; p++) {
        const struct ui_widget *parent = nodes[p].widget;
        size_t limit = parent && parent->ops ? parent->ops->child_limit : 0;
        size_t added = 0;
        for (size_t i = 1; i < count; i++) {
            if (nodes[i].parent != p)
                continue;
            const struct ui_widget *child = nodes[i].widget;
            *failed = i;
            if (!parent || !child || parent == child ||
                parent->impl.destroyed || child->impl.destroyed)
                return false;
            if (child->parent == parent)
                continue;
            /* The insert adds a child. The widgets that the update retires
             * leave after the insert, so they still count. */
            if (widget_is_ancestor(child, parent))
                return false;
            added++;
            if (limit && ui_widget_child_count(parent) + added > limit)
                return false;
        }
    }
    *failed = 0;
    return true;
}

/* Places each node widget in descriptor order. A widget that stays in its
 * parent keeps its focus and its hover. Call placement_ok first. */
static bool place_nodes(struct ui_view_node *nodes, size_t count,
                        struct ui_widget **before, size_t *failed)
{
    *failed = 0;
    bool placed = true;
    for (size_t i = count; i-- > 1;) {
        uint32_t parent = nodes[i].parent;
        struct ui_widget *parent_widget = nodes[parent].widget;
        struct ui_widget *child = nodes[i].widget;
        *failed = i;
        if (!parent_widget || !child) {
            placed = false;
            break;
        }
        if (child->parent && child->parent != parent_widget &&
            !ui_widget_remove_child(child->parent, child)) {
            placed = false;
            break;
        }
        if (!ui_widget_insert_child(parent_widget, child, before[parent])) {
            placed = false;
            break;
        }
        before[parent] = child;
    }
    return placed;
}

/* Applies the descriptor attributes to the supplied widgets, after all
 * fallible work has succeeded. */
static void apply_supplied_attributes(struct ui_view *view)
{
    for (size_t i = 0; i < view->count; i++) {
        struct ui_view_node *node = &view->nodes[i];
        if (node->kind != UI_NODE_WIDGET || !node->widget || !node->desc)
            continue;
        const struct ui_node_desc *desc = node->desc;
        struct ui_widget *widget = node->widget;
        if (desc->class_name || desc->id_name || desc->tag_name)
            ui_widget_set_style_identity(widget,
                desc->class_name ? node->class_name : widget->impl.style_class,
                desc->id_name ? node->id_name : widget->impl.style_id,
                desc->tag_name ? node->tag_name : widget->impl.style_tag);
        if (node->key)
            /* The view owns this string until destruction. */
            ui_widget_set_debug_key(widget, node->key);
        if (desc->on_action)
            ui_widget_set_action_handler(widget, desc->on_action,
                                         desc->action_userdata);
        if (desc->on_event)
            ui_widget_set_event_handler(widget, desc->on_event,
                                        desc->event_userdata);
    }
}

/* Points each kept widget back at the strings that the view holds. Call it
 * on a failed update, before the copies of the update are freed. */
static void restore_kept_strings(const struct ui_view *view,
                                 const struct ui_view_node *nodes,
                                 size_t count, const size_t *match)
{
    for (size_t i = 0; i < count; i++) {
        if (match[i] == SIZE_MAX || !nodes[i].widget)
            continue;
        const struct ui_view_node *held = &view->nodes[match[i]];
        if (held->kind != UI_NODE_WIDGET && held->kind != UI_NODE_LIST)
            ui_widget_set_style_identity(held->widget, held->class_name,
                                         held->id_name, held->tag_name);
        ui_widget_set_debug_key(held->widget, held->key);
    }
}

/* Updates the view. The caller sets `updating`. */
static bool view_update(struct ui_view *view, const struct ui_node_desc *root,
                        struct ui_view_diagnostic *diagnostic)
{
    if (!root) {
        if (diagnostic) {
            diagnostic->error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            diagnostic->node = root;
            diagnostic->detail = "Root descriptor is NULL";
        }
        return false;
    }

    size_t count = 0;
    struct ui_view_node *plan = plan_nodes(root, &count, diagnostic);
    if (!plan)
        return false;

    struct ui_surface *surface = view->root ? ui_widget_surface(view->root)
                                            : NULL;
    size_t *match = malloc(count * sizeof(*match));
    bool *taken = calloc(view->count ? view->count : 1, sizeof(*taken));
    struct ui_view_node *next = calloc(count ? count : 1, sizeof(*next));
    struct ui_widget **before = calloc(count ? count : 1, sizeof(*before));
    if (!match || !taken || !next || !before) {
        if (diagnostic) {
            diagnostic->error = UI_VIEW_ERROR_OUT_OF_MEMORY;
            diagnostic->node = root;
            diagnostic->detail = "Cannot allocate the update";
        }
        free(match);
        free(taken);
        free(next);
        free(before);
        free(plan);
        return false;
    }

    enum ui_view_error error = UI_VIEW_ERROR_NONE;
    const struct ui_node_desc *failed = root;
    bool kept_changed = false;
    match_nodes(view, plan, count, match, taken);
    /* Create the new nodes and check the placement first. A failure here
     * leaves each kept widget as it was. */
    for (size_t i = 0; i < count; i++) {
        failed = plan[i].desc;
        if (match[i] == SIZE_MAX) {
            next[i].desc = plan[i].desc;
            next[i].kind = plan[i].kind;
            next[i].parent = plan[i].parent;
            if (!node_init(&next[i], &error))
                goto fail;
            continue;
        }
        next[i] = view->nodes[match[i]];
        next[i].desc = plan[i].desc;
        next[i].bind = plan[i].desc->bind;
        if (!copy_node_strings(&next[i])) {
            error = UI_VIEW_ERROR_OUT_OF_MEMORY;
            goto fail;
        }
    }
    size_t bad = 0;
    if (!placement_ok(next, count, &bad)) {
        failed = plan[bad].desc;
        error = UI_VIEW_ERROR_INVALID_CHILDREN;
        goto fail;
    }
    /* The kept widgets change from here. Only a lack of memory fails now. */
    kept_changed = true;
    for (size_t i = 0; i < count; i++) {
        if (match[i] == SIZE_MAX)
            continue;
        failed = plan[i].desc;
        if (!node_set_props(&next[i], &error))
            goto fail;
    }
    if (!place_nodes(next, count, before, &bad)) {
        failed = plan[bad].desc;
        error = UI_VIEW_ERROR_INVALID_CHILDREN;
        goto fail;
    }

    /* The update succeeded. Take the new node list, and retire the nodes that
     * lost their match. */
    struct ui_view_node *old = view->nodes;
    size_t old_count = view->count;
    for (size_t i = 0; i < count; i++)
        write_bind_slots(&next[i]);
    view->nodes = next;
    view->count = count;
    view->capacity = count;
    view->root = count ? next[0].widget : NULL;
    view->committed = true;
    apply_supplied_attributes(view);
    observe_widgets(view);
    for (size_t i = 0; i < count; i++)
        view->nodes[i].desc = NULL;
    retire_nodes(surface, old, old_count, taken);
    free(match);
    free(taken);
    free(before);
    free(plan);
    return true;

fail:
    if (diagnostic) {
        diagnostic->error = error == UI_VIEW_ERROR_NONE
            ? UI_VIEW_ERROR_INIT_FAILED : error;
        diagnostic->node = failed;
        diagnostic->detail = ui_view_error_message(diagnostic->error);
    }
    if (kept_changed)
        restore_kept_strings(view, next, count, match);
    discard_nodes(surface, next, count, match);
    free(match);
    free(taken);
    free(before);
    free(plan);
    return false;
}

bool ui_view_update_ex(struct ui_view *view, const struct ui_node_desc *root,
                       struct ui_view_diagnostic *diagnostic)
{
    if (diagnostic)
        *diagnostic = (struct ui_view_diagnostic){0};
    if (!view) {
        if (diagnostic) {
            diagnostic->error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            diagnostic->node = root;
            diagnostic->detail = "View is NULL";
        }
        return false;
    }
    if (view->updating || view->destroying) {
        if (diagnostic) {
            diagnostic->error = UI_VIEW_ERROR_BUSY;
            diagnostic->node = root;
            diagnostic->detail = view->updating ? "View update is running"
                                                : "View is being destroyed";
        }
        return false;
    }
    view->updating = true;
    bool updated = view_update(view, root, diagnostic);
    view->updating = false;
    if (!view->destroy_pending)
        return updated;
    /* A handler destroyed the view during the update. */
    if (diagnostic) {
        diagnostic->error = UI_VIEW_ERROR_BUSY;
        diagnostic->node = root;
        diagnostic->detail = "View was destroyed during the update";
    }
    ui_view_destroy(view);
    return false;
}

bool ui_view_update(struct ui_view *view, const struct ui_node_desc *root)
{
    struct ui_view_diagnostic diagnostic;
    return ui_view_update_ex(view, root, &diagnostic);
}

struct ui_view *ui_view_mount_ex(const struct ui_node_desc *root,
                                 struct ui_view_diagnostic *diagnostic)
{
    struct ui_view *view = calloc(1, sizeof(*view));
    if (!view) {
        if (diagnostic) {
            *diagnostic = (struct ui_view_diagnostic){
                .error = UI_VIEW_ERROR_OUT_OF_MEMORY,
                .node = root,
                .detail = "Cannot allocate view",
            };
        }
        return NULL;
    }
    if (diagnostic)
        *diagnostic = (struct ui_view_diagnostic){0};
    view->updating = true;
    bool mounted = view_update(view, root, diagnostic);
    view->updating = false;
    if (!mounted || view->destroy_pending) {
        ui_view_destroy(view);
        return NULL;
    }
    return view;
}

struct ui_view *ui_view_mount(const struct ui_node_desc *root,
                              enum ui_view_error *error)
{
    struct ui_view_diagnostic diagnostic;
    struct ui_view *view = ui_view_mount_ex(root, &diagnostic);
    if (error)
        *error = diagnostic.error;
    return view;
}

struct ui_widget *ui_view_root(struct ui_view *view)
{
    return view ? view->root : NULL;
}

struct ui_widget *ui_view_widget(struct ui_view *view, const char *key)
{
    struct ui_view_node *node = find_node(view, key);
    return node ? node->widget : NULL;
}

#define UI_VIEW_TYPED_LOOKUP(name, type, expected) \
    type *name(struct ui_view *view, const char *key) \
    { \
        struct ui_view_node *node = find_node(view, key); \
        return node && (expected) ? node->storage : NULL; \
    }

UI_VIEW_TYPED_LOOKUP(ui_view_box, struct ui_box,
                     node->kind == UI_NODE_ROW ||
                     node->kind == UI_NODE_COLUMN ||
                     node->kind == UI_NODE_STACK)
UI_VIEW_TYPED_LOOKUP(ui_view_label, struct ui_label,
                     node->kind == UI_NODE_LABEL)
UI_VIEW_TYPED_LOOKUP(ui_view_button, struct ui_button,
                     node->kind == UI_NODE_BUTTON)
UI_VIEW_TYPED_LOOKUP(ui_view_input, struct ui_input,
                     node->kind == UI_NODE_INPUT)
UI_VIEW_TYPED_LOOKUP(ui_view_dropdown, struct ui_dropdown,
                     node->kind == UI_NODE_DROPDOWN)
UI_VIEW_TYPED_LOOKUP(ui_view_scrollview, struct ui_scrollview,
                     node->kind == UI_NODE_SCROLLVIEW)
UI_VIEW_TYPED_LOOKUP(ui_view_list, struct ui_list,
                     node->kind == UI_NODE_LIST)

struct ui_menu *ui_view_menu(struct ui_view *view, const char *key)
{
    struct ui_view_node *node = find_node(view, key);
    if (!node || node->kind != UI_NODE_MENU || !node->storage)
        return NULL;
    return &((struct ui_menu_node *)node->storage)->menu;
}
