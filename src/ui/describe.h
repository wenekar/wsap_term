#ifndef WSAP_UI_DESCRIBE_H
#define WSAP_UI_DESCRIBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/box.h"
#include "ui/event.h"
#include "ui/widgets/button.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/input.h"
#include "ui/widgets/label.h"
#include "ui/widgets/list.h"
#include "ui/widgets/menu.h"
#include "ui/widgets/scrollview.h"

struct ui_view;

enum ui_node_kind : uint8_t {
    UI_NODE_ROW = 0,
    UI_NODE_COLUMN,
    UI_NODE_STACK,
    UI_NODE_LABEL,
    UI_NODE_BUTTON,
    UI_NODE_INPUT,
    UI_NODE_DROPDOWN,
    UI_NODE_SCROLLVIEW,
    UI_NODE_LIST,
    UI_NODE_MENU,
    UI_NODE_WIDGET,
};

/* Use the field that matches the node kind.
 * The slots are borrowed through mount and written only on success.
 * The view owns each returned widget until view destruction. */
struct ui_node_bindings {
    struct ui_box **box;
    struct ui_label **label;
    struct ui_button **button;
    struct ui_input **input;
    struct ui_dropdown **dropdown;
    struct ui_scrollview **scrollview;
    struct ui_list **list;
    struct ui_menu **menu;
};

struct ui_node_desc {
    const char *key;
    const char *class_name;
    const char *id_name;
    const char *tag_name;
    const struct ui_node_desc *children;
    size_t child_count;
    ui_action_fn on_action;
    void *action_userdata;
    ui_event_fn on_event;
    void *event_userdata;
    enum ui_node_kind kind;
    int action_id;
    struct ui_node_bindings bind;

    union {
        struct {
            const char *text;
            const char *link;
            enum ui_label_wrap wrap;
            enum ui_label_align align;
            bool selectable;
            bool disabled;
        } label;
        struct {
            const char *text;
            bool disabled;
        } button;
        struct {
            const char *text;
            const char *placeholder;
            const char *accessible_name;
        } input;
        struct {
            const struct ui_dropdown_item *items;
            size_t count;
            const char *selected_value;
            const char *accessible_name;
        } dropdown;
        struct {
            bool scrollbar;
            int scroll_step;
        } scrollview;
        /* The node class name is the list style class. */
        struct {
            const struct ui_list_item *items;
            int count;
            int selected;
            bool wrap_navigation;
        } list;
        /* A menu opens when the items are set, and closes when they are not.
         * The menu layer is the node widget, so the app shows it as a surface
         * overlay. */
        struct {
            const ui_menu_item *items;
            int count;
            struct vector2i pos;
            ui_menu_submenu_fn on_submenu;
            void *submenu_userdata;
            uint32_t hover_delay_ms;
        } menu;
        /* Supply an initialized, detached widget or subtree. An update can
         * supply a widget that the view already holds, also when the view
         * shows it. The widget can move to another parent node.
         * On success, the view destroys its widget resources.
         * Keep its enclosing storage alive until view destruction.
         * On failure, the caller retains the detached widget.
         * Descriptor children are added after existing widget children.
         * Unset style fields and handlers preserve the widget values.
         * Set these descriptor fields to replace them. */
        struct ui_widget *widget;
    } props;
};

enum ui_view_error {
    UI_VIEW_ERROR_NONE = 0,
    UI_VIEW_ERROR_INVALID_ARGUMENT,
    UI_VIEW_ERROR_INVALID_KIND,
    UI_VIEW_ERROR_INVALID_CHILDREN,
    UI_VIEW_ERROR_DUPLICATE_KEY,
    UI_VIEW_ERROR_OUT_OF_MEMORY,
    UI_VIEW_ERROR_INIT_FAILED,
    /* A handler called the view during an update or a destroy. */
    UI_VIEW_ERROR_BUSY,
};

struct ui_view_diagnostic {
    enum ui_view_error error;
    /* Borrowed from the descriptor tree. NULL when no node applies. */
    const struct ui_node_desc *node;
    /* Static detail text. NULL on success. */
    const char *detail;
};

/* Returns a static description of an error. */
const char *ui_view_error_message(enum ui_view_error error);

/* On failure, diagnostic identifies the descriptor that caused the error.
 * The node pointer stays valid only while the descriptor tree stays valid. */
struct ui_view *ui_view_mount_ex(const struct ui_node_desc *root,
                                 struct ui_view_diagnostic *diagnostic);

struct ui_view *ui_view_mount(const struct ui_node_desc *root,
                              enum ui_view_error *error);

/* Updates the mounted view to match `root`. A node takes over the mounted
 * widget of the node that it matches:
 * - A node with a key matches the node with the same key.
 * - A node without a key matches the node at the same child index.
 * The kind must be the same. A matched widget keeps its focus, its hover, and
 * its control state, and it takes the new text and the new style identity.
 * A node without a match gets a new widget. A mounted node without a match
 * loses its widget. The children follow the descriptor order.
 *
 * A supplied widget node keeps its widget only when the descriptor supplies
 * the same widget. A different widget replaces it.
 *
 * A failed update leaves the view as it was. The update checks the
 * descriptor tree and the placement before it changes a matched widget. After
 * that, only a lack of memory makes it fail. Then a matched widget can hold
 * some of the new properties, and the view stays valid. The view frees the
 * storage of a removed node after the surface is idle, so a handler can call
 * this function. The call fails while the surface is busy only when memory is
 * short.
 *
 * An update can call a node handler, for example when it closes a menu. Such a
 * handler cannot update the view: the call fails with UI_VIEW_ERROR_BUSY. A
 * menu node with the same items as its open menu stays open and sends no
 * CLOSE. Its submenus stay open only when the submenu provider and its
 * userdata also stay the same. */
bool ui_view_update_ex(struct ui_view *view, const struct ui_node_desc *root,
                       struct ui_view_diagnostic *diagnostic);
bool ui_view_update(struct ui_view *view, const struct ui_node_desc *root);

/* Destroys the view and its widgets. A handler that an update calls can
 * destroy the view. The view then goes when the update ends, and the update
 * fails with UI_VIEW_ERROR_BUSY. Do not use the view after that update. */
void ui_view_destroy(struct ui_view *view);

/* Returns the widget of the descriptor root. */
struct ui_widget *ui_view_root(struct ui_view *view);
/* Returns any descriptor widget with this key, or NULL. */
struct ui_widget *ui_view_widget(struct ui_view *view, const char *key);
struct ui_box *ui_view_box(struct ui_view *view, const char *key);
struct ui_label *ui_view_label(struct ui_view *view, const char *key);
struct ui_button *ui_view_button(struct ui_view *view, const char *key);
struct ui_input *ui_view_input(struct ui_view *view, const char *key);
struct ui_dropdown *ui_view_dropdown(struct ui_view *view, const char *key);
struct ui_scrollview *ui_view_scrollview(struct ui_view *view,
                                         const char *key);
struct ui_list *ui_view_list(struct ui_view *view, const char *key);
struct ui_menu *ui_view_menu(struct ui_view *view, const char *key);

#define UI_NODES(...) ((const struct ui_node_desc[]){ __VA_ARGS__ })

/* Initialize children and child_count together in a node descriptor.
 * Define nested UI_CHILDREN descriptors separately before using them. */
#define UI_CHILDREN(...) \
    .children = UI_NODES(__VA_ARGS__), \
    .child_count = sizeof((const struct ui_node_desc[]){ __VA_ARGS__ }) / \
                   sizeof(struct ui_node_desc)

#endif
