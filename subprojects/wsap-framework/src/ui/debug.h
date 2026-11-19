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
    char *text;
    struct rect2i rect;
    int index;
};

struct ui_debug_parts {
    struct ui_debug_part *items;
    int n, cap;
};

/* Frees memory allocated in a debug parts collection. */
void ui_debug_parts_free(struct ui_debug_parts *parts);

/* Appends a named debug part rectangle to `out`. */
void ui_debug_part(struct ui_debug_parts *out, const char *name, int index,
                   struct rect2i rect);
/* Appends a named debug part rectangle with label text to `out`. */
void ui_debug_part_text(struct ui_debug_parts *out, const char *name,
                        int index, struct rect2i rect,
                        const char *text, size_t len);
/* Collects debug inspection parts from `widget` into `out`. */
void ui_debug_collect_parts(const struct ui_widget *widget,
                            struct ui_debug_parts *out);

struct ui_debug_view {
    const char *name;
    const struct ui_widget *root;
    const struct ui_surface *surface;
    const struct ui_scene *scene;
};

/* Returns the type name string of `widget`. */
const char *ui_debug_type_name(const struct ui_widget *widget);

/* Finds a widget by path, type, style class, style ID, or @debug key. */
const struct ui_widget *ui_debug_find(const struct ui_widget *root,
                                      const char *selector);
/* Writes the hierarchy path from `root` to `widget` into `buf`. */
bool ui_debug_path(const struct ui_widget *root,
                   const struct ui_widget *widget, char *buf, size_t cap);

/* Finds the topmost visible widget at point `p` under `root`. */
const struct ui_widget *ui_debug_widget_at(const struct ui_widget *root,
                                           struct vector2i p);

/* Returns the widget that owns scene operation `op_index`. */
const struct ui_widget *ui_debug_op_owner(const struct ui_debug_view *view,
                                          int op_index);

/* Calculates the bounding box of scene text operation `op`. */
struct rect2i ui_debug_text_box(const struct ui_scene *scene,
                                const struct ui_scene_op *op);

/* Writes the widget hierarchy tree starting at `start` to `j` as JSON. */
void ui_debug_write_tree(struct json_writer *j,
                         const struct ui_debug_view *view,
                         const struct ui_widget *start, int max_depth);
/* Writes the properties and layout of `widget` to `j` as JSON. */
void ui_debug_write_widget(struct json_writer *j,
                           const struct ui_debug_view *view,
                           const struct ui_widget *widget);
/* Writes hit test information for point `p` to `j` as JSON. */
void ui_debug_write_hit(struct json_writer *j,
                        const struct ui_debug_view *view, struct vector2i p);
/* Writes scene operations in `view` to `j` as JSON, optionally filtered by `filter`. */
void ui_debug_write_scene(struct json_writer *j,
                          const struct ui_debug_view *view,
                          const struct ui_widget *filter);

enum ui_debug_lint_kind {
    UI_DEBUG_LINT_V_CENTER,
    UI_DEBUG_LINT_OVERFLOW,
    UI_DEBUG_LINT_ROW,
};

struct ui_debug_lint {
    const struct ui_widget *owner;
    struct rect2i text_box;
    struct rect2i surface;
    enum ui_debug_lint_kind kind;
    int op;
    int other_op;
    int offset;
};

/* Inspects scene operations in `view` for alignment and overflow defects. */
int ui_debug_lint_scene(const struct ui_debug_view *view,
                        struct ui_debug_lint *out, int cap);
/* Writes scene lint results for `view` to `j` as JSON. */
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
    const struct ui_widget *selected;
    struct vector2i pointer;
    unsigned flags;
    bool has_pointer;
};

/* Appends debug overlay graphics to `scene`. */
bool ui_debug_overlay_append(struct ui_scene *scene,
                             const struct ui_debug_view *view,
                             const struct ui_debug_overlay *overlay);

#endif
