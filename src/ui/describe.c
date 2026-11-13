#include "ui/describe.h"

#include "ui/surface.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UI_VIEW_MAX_DEPTH 128

struct ui_view_node {
    const struct ui_node_desc *desc;
    size_t parent;
    size_t overlay;
    enum ui_node_kind kind;
    char *key;
    char *class_name;
    char *id_name;
    char *tag_name;
    void *storage;
    struct ui_widget *widget;
};

struct ui_view {
    struct ui_view_node *nodes;
    size_t count;
    size_t capacity;
    struct ui_widget *root;
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

static bool node_is_container(enum ui_node_kind kind)
{
    return kind == UI_NODE_ROW || kind == UI_NODE_COLUMN ||
           kind == UI_NODE_STACK;
}

static bool node_has_children(enum ui_node_kind kind)
{
    return node_is_container(kind) || kind == UI_NODE_SCROLLVIEW;
}

static bool node_kind_valid(enum ui_node_kind kind)
{
    switch (kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK:
    case UI_NODE_LABEL:
    case UI_NODE_BUTTON:
    case UI_NODE_INPUT:
    case UI_NODE_DROPDOWN:
    case UI_NODE_SCROLLVIEW:
        return true;
    }
    return false;
}

static bool node_props_valid(const struct ui_node_desc *desc,
                             enum ui_view_error *error, const char **detail)
{
    if ((desc->bind.box && !node_is_container(desc->kind)) ||
        (desc->bind.label && desc->kind != UI_NODE_LABEL) ||
        (desc->bind.button && desc->kind != UI_NODE_BUTTON) ||
        (desc->bind.input && desc->kind != UI_NODE_INPUT) ||
        (desc->bind.dropdown && desc->kind != UI_NODE_DROPDOWN) ||
        (desc->bind.scrollview && desc->kind != UI_NODE_SCROLLVIEW)) {
        *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
        *detail = "Binding does not match the node kind";
        return false;
    }
    switch (desc->kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK:
        if (desc->props.layout.align < UI_BOX_ALIGN_START ||
            desc->props.layout.align > UI_BOX_ALIGN_STRETCH ||
            desc->props.layout.justify < UI_BOX_JUSTIFY_START ||
            desc->props.layout.justify > UI_BOX_JUSTIFY_END) {
            *error = UI_VIEW_ERROR_INVALID_ARGUMENT;
            *detail = "Invalid box alignment or justification";
            return false;
        }
        return true;
    case UI_NODE_LABEL:
        if (desc->props.label.wrap < UI_LABEL_WRAP_WORD ||
            desc->props.label.wrap > UI_LABEL_ELLIPSIS ||
            desc->props.label.align < UI_LABEL_ALIGN_LEFT ||
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
        if (desc->props.dropdown.overlay_key &&
            !desc->props.dropdown.overlay_key[0]) {
            *error = UI_VIEW_ERROR_MISSING_OVERLAY;
            *detail = "Dropdown overlay key is empty";
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
    }
    *error = UI_VIEW_ERROR_INVALID_KIND;
    *detail = "Node kind is not supported";
    return false;
}

static bool append_node(struct ui_view *view,
                        const struct ui_node_desc *desc, size_t parent,
                        size_t *index)
{
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
    *index = view->count++;
    view->nodes[*index] = (struct ui_view_node){
        .desc = desc,
        .parent = parent,
        .overlay = SIZE_MAX,
        .kind = desc->kind,
    };
    return true;
}

static bool validate_node(struct validate_context *context,
                          const struct ui_node_desc *desc, size_t parent,
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

    size_t index;
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

static struct ui_view_node *find_descriptor_node(struct ui_view *view,
                                                  const char *key)
{
    if (!view || !key)
        return NULL;
    for (size_t i = 0; i < view->count; i++) {
        const char *node_key = view->nodes[i].desc
            ? view->nodes[i].desc->key : NULL;
        if (node_key && !strcmp(node_key, key))
            return &view->nodes[i];
    }
    return NULL;
}

static const char *default_class(enum ui_node_kind kind)
{
    switch (kind) {
    case UI_NODE_LABEL: return "label";
    case UI_NODE_BUTTON: return "button";
    case UI_NODE_INPUT: return "text-input";
    case UI_NODE_DROPDOWN: return "dropdown";
    case UI_NODE_SCROLLVIEW: return "scrollview";
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK:
        return NULL;
    }
    return NULL;
}

static const char *default_tag(enum ui_node_kind kind)
{
    switch (kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK: return "box";
    case UI_NODE_LABEL: return "label";
    case UI_NODE_BUTTON: return "button";
    case UI_NODE_INPUT: return "input";
    case UI_NODE_DROPDOWN: return "dropdown";
    case UI_NODE_SCROLLVIEW: return "scrollview";
    }
    return NULL;
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
    switch (kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK: return sizeof(struct ui_box);
    case UI_NODE_LABEL: return sizeof(struct ui_label);
    case UI_NODE_BUTTON: return sizeof(struct ui_button);
    case UI_NODE_INPUT: return sizeof(struct ui_input);
    case UI_NODE_DROPDOWN: return sizeof(struct ui_dropdown);
    case UI_NODE_SCROLLVIEW: return sizeof(struct ui_scrollview);
    }
    return 0;
}

static bool initialize_node(struct ui_view_node *node,
                            enum ui_view_error *error)
{
    const struct ui_node_desc *desc = node->desc;
    node->storage = calloc(1, widget_size(node->kind));
    if (!node->storage) {
        *error = UI_VIEW_ERROR_OUT_OF_MEMORY;
        return false;
    }
    if (!copy_node_strings(node)) {
        *error = UI_VIEW_ERROR_OUT_OF_MEMORY;
        return false;
    }

    switch (node->kind) {
    case UI_NODE_ROW:
    case UI_NODE_COLUMN:
    case UI_NODE_STACK: {
        struct ui_box *box = node->storage;
        ui_box_init(box);
        node->widget = &box->widget;
        enum ui_box_direction direction = node->kind == UI_NODE_COLUMN
            ? UI_BOX_COLUMN : node->kind == UI_NODE_STACK
            ? UI_BOX_STACK : UI_BOX_ROW;
        ui_box_set_direction(box, direction);
        ui_box_set_align(box, desc->props.layout.align);
        ui_box_set_justify(box, desc->props.layout.justify);
        ui_box_set_gap(box, desc->props.layout.gap);
        ui_box_set_padding(box, desc->props.layout.padding);
        break;
    }
    case UI_NODE_LABEL: {
        struct ui_label *label = node->storage;
        ui_label_init(label);
        node->widget = &label->widget;
        if (!ui_label_set_text(label, desc->props.label.text)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        if (desc->props.label.link &&
            !ui_label_set_link(label, desc->props.label.link)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        ui_label_set_wrap(label, desc->props.label.wrap);
        ui_label_set_align(label, desc->props.label.align);
        ui_label_set_selectable(label, desc->props.label.selectable);
        ui_label_set_disabled(label, desc->props.label.disabled);
        ui_label_set_action_id(label, desc->action_id);
        break;
    }
    case UI_NODE_BUTTON: {
        struct ui_button *button = node->storage;
        ui_button_init(button);
        node->widget = &button->widget;
        ui_button_set_text(button, desc->props.button.text);
        ui_button_set_disabled(button, desc->props.button.disabled);
        ui_button_set_action_id(button, desc->action_id);
        break;
    }
    case UI_NODE_INPUT: {
        struct ui_input *input = node->storage;
        ui_input_init(input);
        node->widget = &input->widget;
        ui_input_set_text(input, desc->props.input.text);
        ui_input_set_placeholder(input, desc->props.input.placeholder);
        if (desc->props.input.accessible_name &&
            !ui_widget_set_accessible_name(&input->widget,
                desc->props.input.accessible_name)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        ui_input_set_action_id(input, desc->action_id);
        break;
    }
    case UI_NODE_DROPDOWN: {
        struct ui_dropdown *dropdown = node->storage;
        ui_dropdown_init(dropdown);
        node->widget = &dropdown->widget;
        if (!ui_dropdown_set_items(dropdown, desc->props.dropdown.items,
                                   desc->props.dropdown.count)) {
            *error = UI_VIEW_ERROR_OUT_OF_MEMORY;
            return false;
        }
        if (desc->props.dropdown.selected_value)
            ui_dropdown_set_value(dropdown,
                                  desc->props.dropdown.selected_value);
        if (desc->props.dropdown.accessible_name &&
            !ui_widget_set_accessible_name(&dropdown->widget,
                desc->props.dropdown.accessible_name)) {
            *error = UI_VIEW_ERROR_INIT_FAILED;
            return false;
        }
        ui_dropdown_set_action_id(dropdown, desc->action_id);
        break;
    }
    case UI_NODE_SCROLLVIEW: {
        struct ui_scrollview *scrollview = node->storage;
        ui_scrollview_init(scrollview);
        node->widget = &scrollview->widget;
        ui_scrollview_set_scrollbar(scrollview,
                                    desc->props.scrollview.scrollbar);
        ui_scrollview_set_scroll_step(scrollview,
                                      desc->props.scrollview.scroll_step);
        break;
    }
    }

    ui_widget_set_style_identity(node->widget, node->class_name,
                                 node->id_name, node->tag_name);
    ui_widget_set_debug_key(node->widget, node->key);
    ui_widget_set_action_handler(node->widget, desc->on_action,
                                 desc->action_userdata);
    ui_widget_set_event_handler(node->widget, desc->on_event,
                                desc->event_userdata);
    return true;
}

static bool validate_overlays(struct ui_view *view, enum ui_view_error *error,
                              const struct ui_node_desc **failed_node,
                              const char **detail)
{
    for (size_t i = 0; i < view->count; i++) {
        struct ui_view_node *node = &view->nodes[i];
        if (node->kind != UI_NODE_DROPDOWN)
            continue;
        *failed_node = node->desc;
        const char *key = node->desc->props.dropdown.overlay_key;
        struct ui_view_node *overlay = NULL;
        if (key) {
            overlay = find_descriptor_node(view, key);
        } else {
            size_t parent = node->parent;
            while (parent != SIZE_MAX) {
                if (view->nodes[parent].kind == UI_NODE_STACK) {
                    overlay = &view->nodes[parent];
                    break;
                }
                parent = view->nodes[parent].parent;
            }
        }
        if (!overlay) {
            *error = UI_VIEW_ERROR_MISSING_OVERLAY;
            *detail = key ? "Dropdown overlay key was not found"
                          : "Dropdown has no ancestor stack";
            return false;
        }
        if (overlay->kind != UI_NODE_STACK) {
            *error = UI_VIEW_ERROR_INVALID_OVERLAY;
            *detail = "Dropdown overlay must be a stack";
            return false;
        }
        size_t ancestor = (size_t)(overlay - view->nodes);
        while (ancestor != SIZE_MAX) {
            if (ancestor == i) {
                *error = UI_VIEW_ERROR_INVALID_OVERLAY;
                *detail = "Dropdown overlay cannot be a descendant";
                return false;
            }
            ancestor = view->nodes[ancestor].parent;
        }
        node->overlay = (size_t)(overlay - view->nodes);
    }
    return true;
}

void ui_view_destroy(struct ui_view *view)
{
    if (!view)
        return;
    if (view->root) {
        struct ui_surface *surface = ui_widget_surface(view->root);
        if (surface && ui_surface_root(surface) == view->root)
            ui_surface_set_root(surface, NULL);
    }
    for (size_t i = 0; i < view->count; i++)
        if (view->nodes[i].widget)
            ui_widget_destroy(view->nodes[i].widget);
    for (size_t i = 0; i < view->count; i++) {
        free(view->nodes[i].key);
        free(view->nodes[i].class_name);
        free(view->nodes[i].id_name);
        free(view->nodes[i].tag_name);
        free(view->nodes[i].storage);
    }
    free(view->nodes);
    free(view);
}

const char *ui_view_error_message(enum ui_view_error error)
{
    switch (error) {
    case UI_VIEW_ERROR_NONE: return "No error";
    case UI_VIEW_ERROR_INVALID_ARGUMENT: return "Invalid argument or property";
    case UI_VIEW_ERROR_INVALID_KIND: return "Invalid node kind";
    case UI_VIEW_ERROR_INVALID_CHILDREN: return "Invalid children";
    case UI_VIEW_ERROR_DUPLICATE_KEY: return "Duplicate key";
    case UI_VIEW_ERROR_MISSING_OVERLAY: return "Dropdown needs an overlay stack";
    case UI_VIEW_ERROR_INVALID_OVERLAY: return "Invalid dropdown overlay";
    case UI_VIEW_ERROR_OUT_OF_MEMORY: return "Out of memory";
    case UI_VIEW_ERROR_INIT_FAILED: return "Widget initialization failed";
    }
    return "Unknown error";
}

struct ui_view *ui_view_mount_ex(const struct ui_node_desc *root,
                                 struct ui_view_diagnostic *diagnostic)
{
    if (diagnostic)
        *diagnostic = (struct ui_view_diagnostic){0};
    if (!root) {
        if (diagnostic)
            diagnostic->error = UI_VIEW_ERROR_INVALID_ARGUMENT;
        if (diagnostic)
            diagnostic->detail = "Root descriptor is NULL";
        return NULL;
    }

    struct ui_view *view = calloc(1, sizeof(*view));
    if (!view) {
        if (diagnostic) {
            diagnostic->error = UI_VIEW_ERROR_OUT_OF_MEMORY;
            diagnostic->node = root;
            diagnostic->detail = "Cannot allocate view";
        }
        return NULL;
    }

    struct validate_context context = { .view = view };
    if (!validate_node(&context, root, SIZE_MAX, 0)) {
        if (diagnostic) {
            diagnostic->error = context.error;
            diagnostic->node = context.failed_node;
            diagnostic->detail = context.detail;
        }
        ui_view_destroy(view);
        return NULL;
    }
    enum ui_view_error mount_error = UI_VIEW_ERROR_NONE;
    const struct ui_node_desc *failed_node = NULL;
    const char *detail = NULL;
    if (!validate_overlays(view, &mount_error, &failed_node, &detail)) {
        if (diagnostic) {
            diagnostic->error = mount_error;
            diagnostic->node = failed_node;
            diagnostic->detail = detail;
        }
        ui_view_destroy(view);
        return NULL;
    }

    for (size_t i = 0; i < view->count; i++) {
        if (!initialize_node(&view->nodes[i], &mount_error)) {
            if (diagnostic) {
                diagnostic->error = mount_error;
                diagnostic->node = view->nodes[i].desc;
                diagnostic->detail = ui_view_error_message(mount_error);
            }
            ui_view_destroy(view);
            return NULL;
        }
    }
    for (size_t i = 1; i < view->count; i++) {
        struct ui_view_node *node = &view->nodes[i];
        if (node->parent >= view->count ||
            !ui_widget_add_child(view->nodes[node->parent].widget,
                                 node->widget)) {
            if (diagnostic) {
                diagnostic->error = UI_VIEW_ERROR_INVALID_CHILDREN;
                diagnostic->node = node->desc;
                diagnostic->detail = "Cannot attach child to parent";
            }
            ui_view_destroy(view);
            return NULL;
        }
    }
    for (size_t i = 0; i < view->count; i++) {
        struct ui_view_node *node = &view->nodes[i];
        if (node->kind == UI_NODE_DROPDOWN) {
            struct ui_view_node *overlay = &view->nodes[node->overlay];
            ui_dropdown_set_overlay(node->storage, overlay->widget);
        }
    }

    view->root = view->nodes[0].widget;
    for (size_t i = 0; i < view->count; i++) {
        struct ui_view_node *node = &view->nodes[i];
        const struct ui_node_bindings *bind = &node->desc->bind;
        if (bind->box) *bind->box = node->storage;
        if (bind->label) *bind->label = node->storage;
        if (bind->button) *bind->button = node->storage;
        if (bind->input) *bind->input = node->storage;
        if (bind->dropdown) *bind->dropdown = node->storage;
        if (bind->scrollview) *bind->scrollview = node->storage;
        view->nodes[i].desc = NULL;
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
