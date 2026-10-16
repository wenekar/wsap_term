#ifndef WSAP_UI_ACCESSIBILITY_H
#define WSAP_UI_ACCESSIBILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/geom.h"

struct ui_widget;

enum ui_accessibility_role {
    UI_ACCESSIBILITY_ROLE_UNKNOWN = 0,
    UI_ACCESSIBILITY_ROLE_WINDOW,
    UI_ACCESSIBILITY_ROLE_GROUP,
    UI_ACCESSIBILITY_ROLE_DIALOG,
    UI_ACCESSIBILITY_ROLE_LABEL,
    UI_ACCESSIBILITY_ROLE_HEADING,
    UI_ACCESSIBILITY_ROLE_BUTTON,
    UI_ACCESSIBILITY_ROLE_TOGGLE_BUTTON,
    UI_ACCESSIBILITY_ROLE_CHECKBOX,
    UI_ACCESSIBILITY_ROLE_RADIO_BUTTON,
    UI_ACCESSIBILITY_ROLE_TAB_LIST,
    UI_ACCESSIBILITY_ROLE_TAB,
    UI_ACCESSIBILITY_ROLE_TEXT_FIELD,
    UI_ACCESSIBILITY_ROLE_SEARCH_FIELD,
    UI_ACCESSIBILITY_ROLE_LINK,
    UI_ACCESSIBILITY_ROLE_LIST,
    UI_ACCESSIBILITY_ROLE_LIST_ITEM,
    UI_ACCESSIBILITY_ROLE_MENU,
    UI_ACCESSIBILITY_ROLE_MENU_ITEM,
    UI_ACCESSIBILITY_ROLE_SLIDER,
    UI_ACCESSIBILITY_ROLE_PROGRESS_INDICATOR,
    UI_ACCESSIBILITY_ROLE_SCROLL_BAR,
    UI_ACCESSIBILITY_ROLE_IMAGE,
    UI_ACCESSIBILITY_ROLE_TERMINAL,
    UI_ACCESSIBILITY_ROLE_COMBO_BOX,
};

enum ui_accessibility_state {
    UI_ACCESSIBILITY_STATE_FOCUSABLE  = 1u << 0,
    UI_ACCESSIBILITY_STATE_FOCUSED    = 1u << 1,
    UI_ACCESSIBILITY_STATE_SELECTED   = 1u << 2,
    UI_ACCESSIBILITY_STATE_DISABLED   = 1u << 3,
    UI_ACCESSIBILITY_STATE_HIDDEN     = 1u << 4,
    UI_ACCESSIBILITY_STATE_EXPANDED   = 1u << 5,
    UI_ACCESSIBILITY_STATE_CHECKED    = 1u << 6,
    UI_ACCESSIBILITY_STATE_EDITABLE   = 1u << 7,
    UI_ACCESSIBILITY_STATE_READ_ONLY  = 1u << 8,
    UI_ACCESSIBILITY_STATE_MULTILINE  = 1u << 9,
    UI_ACCESSIBILITY_STATE_BUSY       = 1u << 10,
    UI_ACCESSIBILITY_STATE_REQUIRED   = 1u << 11,
    UI_ACCESSIBILITY_STATE_INVALID    = 1u << 12,
    UI_ACCESSIBILITY_STATE_PASSWORD   = 1u << 13,
};

enum ui_accessibility_action {
    UI_ACCESSIBILITY_ACTION_INVOKE = 1u << 0,
    UI_ACCESSIBILITY_ACTION_FOCUS = 1u << 1,
    UI_ACCESSIBILITY_ACTION_SET_VALUE = 1u << 2,
    UI_ACCESSIBILITY_ACTION_SET_SELECTION = 1u << 3,
    UI_ACCESSIBILITY_ACTION_INCREMENT = 1u << 4,
    UI_ACCESSIBILITY_ACTION_DECREMENT = 1u << 5,
};

struct ui_accessibility_node {
    uint64_t id;
    uint64_t parent_id;
    uint32_t order;
    enum ui_accessibility_role role;
    uint32_t states;
    uint32_t actions;
    struct rect2i bounds;
    char *name;
    char *description;
    char *value;
    size_t caret_offset;
    size_t selection_start;
    size_t selection_end;
};

struct ui_accessibility_node_spec {
    uint64_t id;
    uint64_t parent_id;
    uint32_t order;
    enum ui_accessibility_role role;
    uint32_t states;
    uint32_t actions;
    struct rect2i bounds;
    const char *name;
    const char *description;
    const char *value;
    size_t caret_offset;
    size_t selection_start;
    size_t selection_end;
};

struct ui_accessibility_tree {
    struct ui_accessibility_node *nodes;
    size_t count;
    size_t capacity;
    size_t *id_index;
    size_t id_index_capacity;
};

bool ui_accessibility_tree_add(struct ui_accessibility_tree *tree,
                               const struct ui_accessibility_node_spec *spec);

void ui_accessibility_tree_clear(struct ui_accessibility_tree *tree);
void ui_accessibility_tree_destroy(struct ui_accessibility_tree *tree);

const struct ui_accessibility_node *
ui_accessibility_tree_find(const struct ui_accessibility_tree *tree,
                           uint64_t id);

bool ui_accessibility_tree_build(const struct ui_widget *root,
                                 struct ui_accessibility_tree *tree);

struct ui_accessibility_action_request {
    uint64_t node_id;
    enum ui_accessibility_action action;
    const char *value;
    size_t caret_offset;
    size_t selection_start;
    size_t selection_end;
};

bool ui_accessibility_dispatch_action(
    struct ui_widget *root,
    const struct ui_accessibility_action_request *request);

typedef void (*ui_accessibility_action_sink_fn)(
    void *userdata, const struct ui_accessibility_action_request *request);

struct ui_accessibility_provider_ops {
    void *(*attach)(void *native_handle,
                    ui_accessibility_action_sink_fn action_sink,
                    void *action_userdata);
    bool (*publish_snapshot)(void *instance,
                             const struct ui_accessibility_tree *snapshot);
    void (*detach)(void *instance);
    bool (*is_active)(void *instance);
};

struct ui_accessibility_provider {
    const struct ui_accessibility_provider_ops *ops;
    void *instance;
};

bool ui_accessibility_provider_attach(
    struct ui_accessibility_provider *provider,
    const struct ui_accessibility_provider_ops *ops, void *native_handle,
    ui_accessibility_action_sink_fn action_sink, void *action_userdata);
bool ui_accessibility_provider_publish_snapshot(
    const struct ui_accessibility_provider *provider,
    const struct ui_accessibility_tree *snapshot);
bool ui_accessibility_provider_is_active(
    const struct ui_accessibility_provider *provider);
void ui_accessibility_provider_detach(
    struct ui_accessibility_provider *provider);

#endif
