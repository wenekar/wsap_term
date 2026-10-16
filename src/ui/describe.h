#ifndef WSAP_UI_DESCRIBE_H
#define WSAP_UI_DESCRIBE_H

#include <stdbool.h>
#include <stddef.h>

#include "ui/box.h"
#include "ui/event.h"
#include "ui/widgets/button.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/input.h"
#include "ui/widgets/label.h"
#include "ui/widgets/scrollview.h"

struct ui_view;

enum ui_node_kind {
    UI_NODE_ROW = 0,
    UI_NODE_COLUMN,
    UI_NODE_STACK,
    UI_NODE_LABEL,
    UI_NODE_BUTTON,
    UI_NODE_INPUT,
    UI_NODE_DROPDOWN,
    UI_NODE_SCROLLVIEW,
};

struct ui_node_layout {
    enum ui_box_align align;
    enum ui_box_justify justify;
    int gap;
    struct insets2i padding;
};

struct ui_node_desc {
    enum ui_node_kind kind;
    const char *key;
    const char *class_name;
    const char *id_name;
    const char *tag_name;
    const struct ui_node_desc *children;
    size_t child_count;

    int action_id;
    ui_action_fn on_action;
    void *action_userdata;
    ui_event_fn on_event;
    void *event_userdata;

    union {
        struct ui_node_layout layout;
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
            const char *overlay_key;
        } dropdown;
        struct {
            bool scrollbar;
            int scroll_step;
        } scrollview;
    } props;
};

enum ui_view_error {
    UI_VIEW_ERROR_NONE = 0,
    UI_VIEW_ERROR_INVALID_ARGUMENT,
    UI_VIEW_ERROR_INVALID_KIND,
    UI_VIEW_ERROR_INVALID_CHILDREN,
    UI_VIEW_ERROR_DUPLICATE_KEY,
    UI_VIEW_ERROR_MISSING_OVERLAY,
    UI_VIEW_ERROR_INVALID_OVERLAY,
    UI_VIEW_ERROR_OUT_OF_MEMORY,
    UI_VIEW_ERROR_INIT_FAILED,
};

struct ui_view *ui_view_mount(const struct ui_node_desc *root,
                              enum ui_view_error *error);

void ui_view_destroy(struct ui_view *view);

struct ui_widget *ui_view_root(struct ui_view *view);
struct ui_box *ui_view_box(struct ui_view *view, const char *key);
struct ui_label *ui_view_label(struct ui_view *view, const char *key);
struct ui_button *ui_view_button(struct ui_view *view, const char *key);
struct ui_input *ui_view_input(struct ui_view *view, const char *key);
struct ui_dropdown *ui_view_dropdown(struct ui_view *view, const char *key);
struct ui_scrollview *ui_view_scrollview(struct ui_view *view,
                                         const char *key);

#define UI_NODES(...) ((const struct ui_node_desc[]){ __VA_ARGS__ })

#endif
