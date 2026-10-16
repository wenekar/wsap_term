#ifndef WSAP_UI_DEBUG_H
#define WSAP_UI_DEBUG_H

/*
 * UI inspector. Everything here reads a widget tree and a built scene,
 * whose scopes record which widget emitted which ops; nothing here
 * changes UI state. The overlay appends ops to an already built scene,
 * after the UI's own ops, and never makes that scene invalid.
 *
 * Geometry reported by the scene functions is what the renderer paints:
 * a text op draws at its rect origin in a cell box of
 * cells * advance by cell_h, whatever its rect size is. The lint and the
 * overlay use that cell box, not the rect the widget asked for.
 *
 * All positions are surface coordinates.
 */

#include <stdbool.h>
#include <stddef.h>

#include "ui/paint.h"
#include "paint/scene.h"
#include "ui/widget.h"
#include "util/geom.h"
#include "util/json.h"

struct ui_surface;


/* A named sub-rect that a widget lays out without a child widget, such
 * as a tab, a close button, or a menu item. index is -1 when the name is
 * unique. */
struct ui_debug_part {
    const char *name;
    int index;
    struct rect2i rect;
    char *text;
};

/* All zero is empty. The parts own their text. */
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
/* Runs the widget's debug_parts op into out (cleared first). */
void ui_debug_collect_parts(const struct ui_widget *widget,
                            struct ui_debug_parts *out);

/* One inspectable UI: a tree and, when available, the last scene built
 * from it. surface supplies focus and capture. */
struct ui_debug_view {
    const char *name;
    const struct ui_widget *root;
    const struct ui_surface *surface;
    const struct ui_scene *scene;
};

const char *ui_debug_type_name(const struct ui_widget *widget);

/* Resolves a selector against the tree under root:
 *   "/"            the root
 *   "/1/0"         child-index path from the root
 *   "0x55d0..."    a widget address; only accepted when it is in the tree
 *   "tabbar"       the first widget (preorder) whose type name matches
 *   ".cls" "#id"   the first widget with that style class or id
 * Returns NULL when nothing matches. */
const struct ui_widget *ui_debug_find(const struct ui_widget *root,
                                      const char *selector);
/* Writes the child-index path of widget ("/" for root). False when the
 * widget is not under root or the path does not fit. */
bool ui_debug_path(const struct ui_widget *root,
                   const struct ui_widget *widget, char *buf, size_t cap);

/* The deepest widget whose bounds contain p, preferring later-painted
 * siblings — the widget the user sees at p. */
const struct ui_widget *ui_debug_widget_at(const struct ui_widget *root,
                                           struct vector2i p);

/* The widget that emitted scene op index op_index, or NULL. */
const struct ui_widget *ui_debug_op_owner(const struct ui_debug_view *view,
                                          int op_index);

/* The box the renderer paints a text op into. */
struct rect2i ui_debug_text_box(const struct ui_scene *scene,
                                const struct ui_scene_op *op);

/* JSON writers. Each writes exactly one JSON value. Paths are always
 * relative to view->root. */
/* The subtree under start (NULL: the root); max_depth < 0 is unlimited. */
void ui_debug_write_tree(struct json_writer *j,
                         const struct ui_debug_view *view,
                         const struct ui_widget *start, int max_depth);
void ui_debug_write_widget(struct json_writer *j,
                           const struct ui_debug_view *view,
                           const struct ui_widget *widget);
void ui_debug_write_hit(struct json_writer *j,
                        const struct ui_debug_view *view, struct vector2i p);
/* Ops emitted by filter's subtree, or every op when filter is NULL. */
void ui_debug_write_scene(struct json_writer *j,
                          const struct ui_debug_view *view,
                          const struct ui_widget *filter);

/*
 * Alignment lint over the built scene. For each text op it finds the
 * surface the text sits on — the nearest earlier filled op that contains
 * the text's cell box center, else the owning widget's bounds — and
 * reports:
 *   "v-center"   the cell box is off vertical center by more than 1 px
 *   "overflow"   the cell box extends past that surface
 *   "row"        text ops of one widget that share a row (overlap by
 *                half a cell or more) start at different y
 * A finding carries the op index, the owner, both boxes and the offset.
 */
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

/* Fills up to cap findings and returns the total number found. */
int ui_debug_lint_scene(const struct ui_debug_view *view,
                        struct ui_debug_lint *out, int cap);
void ui_debug_write_lint(struct json_writer *j,
                         const struct ui_debug_view *view);

enum ui_debug_overlay_flags {
    /* Outline every painted widget. */
    UI_DEBUG_OVERLAY_BOUNDS = 1 << 0,
    /* Margin, padding and content bands of the focus widget. */
    UI_DEBUG_OVERLAY_BOX    = 1 << 1,
    /* Cell boxes of text ops, plus their requested rects when larger. */
    UI_DEBUG_OVERLAY_TEXT   = 1 << 2,
    /* Debug parts of the focus widget. */
    UI_DEBUG_OVERLAY_PARTS  = 1 << 3,
    /* Name and geometry label next to the focus widget. */
    UI_DEBUG_OVERLAY_LABEL  = 1 << 4,
    /* Mark lint findings. */
    UI_DEBUG_OVERLAY_LINT   = 1 << 5,
};

#define UI_DEBUG_OVERLAY_DEFAULT                                         \
    (UI_DEBUG_OVERLAY_BOUNDS | UI_DEBUG_OVERLAY_BOX |                     \
     UI_DEBUG_OVERLAY_PARTS | UI_DEBUG_OVERLAY_LABEL |                    \
     UI_DEBUG_OVERLAY_LINT)

/* The focus widget is selected, else the widget under the pointer. */
struct ui_debug_overlay {
    unsigned flags;
    const struct ui_widget *selected;
    struct vector2i pointer;
    bool has_pointer;
};

/* Appends overlay ops after the scene's own ops. Stops early, leaving
 * the scene valid, when op or text capacity runs out; returns false
 * then. */
bool ui_debug_overlay_append(struct ui_scene *scene,
                             const struct ui_debug_view *view,
                             const struct ui_debug_overlay *overlay);

#endif
