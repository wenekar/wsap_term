#ifndef WSAP_UI_ACCESSIBILITY_H
#define WSAP_UI_ACCESSIBILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/geom.h"

struct ui_widget;
struct ui_surface;

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

/* The text direction of the text of a node. */
enum ui_accessibility_text_direction {
    UI_ACCESSIBILITY_TEXT_DIRECTION_UNKNOWN = 0,
    UI_ACCESSIBILITY_TEXT_DIRECTION_LEFT_TO_RIGHT,
    UI_ACCESSIBILITY_TEXT_DIRECTION_RIGHT_TO_LEFT,
    UI_ACCESSIBILITY_TEXT_DIRECTION_TOP_TO_BOTTOM,
    UI_ACCESSIBILITY_TEXT_DIRECTION_BOTTOM_TO_TOP,
};

/* A text provider answers a range of document text on demand. The provider
 * holds the document and no copy of the text. A large document then costs
 * memory one time. A node shows the range that starts at `text_offset` and
 * holds `text_length` bytes. */
struct ui_accessibility_text_provider {
    void *instance;
    /* Returns the byte length of the whole document. */
    size_t (*length)(void *instance);
    /* Copies at most `capacity` bytes at `offset` to `out`. Returns the byte
     * count of the copy. */
    size_t (*read)(void *instance, size_t offset, char *out, size_t capacity);
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
    const struct ui_accessibility_text_provider *text;
    /* The first byte of the node range in the provider document. */
    size_t text_offset;
    /* The byte count of the node range. SIZE_MAX keeps the rest of the
     * document. */
    size_t text_length;
    enum ui_accessibility_text_direction text_direction;
    /* The number of nodes that name this node as their parent. */
    uint32_t child_count;
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
    const struct ui_accessibility_text_provider *text;
    size_t text_offset;
    size_t text_length;
    enum ui_accessibility_text_direction text_direction;
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

/* Returns the byte length of the text of `node`. A provider node reports the
 * length of its range. */
size_t ui_accessibility_node_text_length(const struct ui_accessibility_node *node);

/* Copies at most `capacity` bytes of the text of `node` at `offset` to `out`.
 * Returns the byte count of the copy. A provider node reads only its range. */
size_t ui_accessibility_node_text_read(const struct ui_accessibility_node *node,
                                       size_t offset, char *out,
                                       size_t capacity);

/* Holds the nodes that a publish must send. */
struct ui_accessibility_publish_set {
    uint64_t *ids;
    size_t count;
    size_t capacity;
    size_t *index;
    size_t index_capacity;
};

/* Fills `set` with the nodes that a publish of `next` must send. A node goes
 * out when it is new, when a field changed, or when its child list changed.
 * A publish without a previous tree sends every node. The provider document
 * bytes do not count as a change. */
bool ui_accessibility_publish_set_build(
    const struct ui_accessibility_tree *previous,
    const struct ui_accessibility_tree *next,
    struct ui_accessibility_publish_set *set);
bool ui_accessibility_publish_set_has(
    const struct ui_accessibility_publish_set *set, uint64_t id);
void ui_accessibility_publish_set_destroy(
    struct ui_accessibility_publish_set *set);

bool ui_accessibility_tree_build(const struct ui_widget *root,
                                 struct ui_accessibility_tree *tree);
/* Builds the tree of `surface`: the root, then each overlay that `surface`
 * holds. A modal overlay scopes the tree to its own subtree. */
bool ui_accessibility_tree_build_surface(const struct ui_surface *surface,
                                         struct ui_accessibility_tree *tree);

struct ui_accessibility_action_request {
    uint64_t node_id;
    /* For SET_VALUE: the new value. A text field removes invalid UTF-8
     * bytes and control characters. The offsets below count bytes of the
     * text that the field keeps. */
    const char *value;
    size_t caret_offset;
    size_t selection_start;
    size_t selection_end;
    enum ui_accessibility_action action;
};

bool ui_accessibility_dispatch_action(
    struct ui_widget *root,
    const struct ui_accessibility_action_request *request);
/* Runs `request` on the first widget of `surface` that answers it. The action
 * reaches the root and each overlay. A modal overlay scopes the action to its
 * own subtree. */
bool ui_accessibility_dispatch_surface_action(
    struct ui_surface *surface,
    const struct ui_accessibility_action_request *request);

/* A provider calls the sink from any thread. A request with no node and no
 * action asks the host to publish a new snapshot. A provider sends it when
 * a client starts to listen. */
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
