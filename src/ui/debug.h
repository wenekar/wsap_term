#ifndef WSAP_UI_DEBUG_H
#define WSAP_UI_DEBUG_H

#include <stdbool.h>
#include <stddef.h>

#include "ui/paint.h"
#include "paint/scene.h"
#include "ui/widget.h"
#include "util/geom.h"
#include "util/json.h"

struct ui_surface;

struct ui_debug_part {
    const char *name;
    int index;
    struct rect2i rect;
    char *text;
};

struct ui_debug_parts {
    struct ui_debug_part *items;
    int n, cap;
};

void ui_debug_parts_free(struct ui_debug_parts *parts);

void ui_debug_part(struct ui_debug_parts *out, const char *name, int index,
                   struct rect2i rect);
void ui_debug_part_text(struct ui_debug_parts *out, const char *name,
                        int index, struct rect2i rect,
                        const char *text, size_t len);
void ui_debug_collect_parts(const struct ui_widget *widget,
                            struct ui_debug_parts *out);

struct ui_debug_view {
    const char *name;
    const struct ui_widget *root;
    const struct ui_surface *surface;
    const struct ui_scene *scene;
};

const char *ui_debug_type_name(const struct ui_widget *widget);

const struct ui_widget *ui_debug_find(const struct ui_widget *root,
                                      const char *selector);
bool ui_debug_path(const struct ui_widget *root,
                   const struct ui_widget *widget, char *buf, size_t cap);

const struct ui_widget *ui_debug_widget_at(const struct ui_widget *root,
                                           struct vector2i p);

const struct ui_widget *ui_debug_op_owner(const struct ui_debug_view *view,
                                          int op_index);

struct rect2i ui_debug_text_box(const struct ui_scene *scene,
                                const struct ui_scene_op *op);

void ui_debug_write_tree(struct json_writer *j,
                         const struct ui_debug_view *view,
                         const struct ui_widget *start, int max_depth);
void ui_debug_write_widget(struct json_writer *j,
                           const struct ui_debug_view *view,
                           const struct ui_widget *widget);
void ui_debug_write_hit(struct json_writer *j,
                        const struct ui_debug_view *view, struct vector2i p);
void ui_debug_write_scene(struct json_writer *j,
                          const struct ui_debug_view *view,
                          const struct ui_widget *filter);

enum ui_debug_lint_kind {
    UI_DEBUG_LINT_V_CENTER,
    UI_DEBUG_LINT_OVERFLOW,
    UI_DEBUG_LINT_ROW,
};

struct ui_debug_lint {
    enum ui_debug_lint_kind kind;
    int op;
    int other_op;
    const struct ui_widget *owner;
    struct rect2i text_box;
    struct rect2i surface;
    int offset;
};

int ui_debug_lint_scene(const struct ui_debug_view *view,
                        struct ui_debug_lint *out, int cap);
void ui_debug_write_lint(struct json_writer *j,
                         const struct ui_debug_view *view);

enum ui_debug_overlay_flags {
    UI_DEBUG_OVERLAY_BOUNDS = 1 << 0,
    UI_DEBUG_OVERLAY_BOX    = 1 << 1,
    UI_DEBUG_OVERLAY_TEXT   = 1 << 2,
    UI_DEBUG_OVERLAY_PARTS  = 1 << 3,
    UI_DEBUG_OVERLAY_LABEL  = 1 << 4,
    UI_DEBUG_OVERLAY_LINT   = 1 << 5,
};

#define UI_DEBUG_OVERLAY_DEFAULT                                         \
    (UI_DEBUG_OVERLAY_BOUNDS | UI_DEBUG_OVERLAY_BOX |                     \
     UI_DEBUG_OVERLAY_PARTS | UI_DEBUG_OVERLAY_LABEL |                    \
     UI_DEBUG_OVERLAY_LINT)

struct ui_debug_overlay {
    unsigned flags;
    const struct ui_widget *selected;
    struct vector2i pointer;
    bool has_pointer;
};

bool ui_debug_overlay_append(struct ui_scene *scene,
                             const struct ui_debug_view *view,
                             const struct ui_debug_overlay *overlay);

#endif
