#ifndef WSAP_UI_WIDGET_H
#define WSAP_UI_WIDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/action.h"
#include "ui/css.h"
#include "ui/event.h"
#include "ui/style_context.h"
#include "util/geom.h"

#define ui_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define ui_container_of_const(ptr, type, member) \
    ((const type *)((const char *)(ptr) - offsetof(type, member)))

/* Cast a typed widget pointer to its base struct ui_widget pointer. */
#define UI_WIDGET(w) (&(w)->widget)

struct ui_surface;
struct ui_widget_ops;
struct ui_widget;

/* Return true to stop the event at this widget. */
typedef bool (*ui_event_fn)(struct ui_widget *widget,
                            const struct ui_event *event, void *userdata);

/* Return true to handle a keyboard event before it bubbles. */
typedef bool (*ui_capture_fn)(struct ui_widget *widget,
                              const struct ui_event *event, void *userdata);

/* Where a parent without its own layout places a child. Rects are in
 * surface space. A placed child keeps its measured size, clamped to the
 * parent. */
enum ui_place_kind : uint8_t {
    /* Fill the parent. */
    UI_PLACE_FILL = 0,
    /* Below anchor, left edges aligned. Flip above when it does not fit. */
    UI_PLACE_BELOW,
    /* Right of anchor, top edges aligned. Flip left when it does not fit. */
    UI_PLACE_RIGHT,
    /* At anchor.pos, moved inside the parent when it does not fit. */
    UI_PLACE_AT,
    /* Centered across. The top is at y_fraction of the free height, but
     * at least gap from the parent's top and bottom edges. */
    UI_PLACE_CENTER,
};

struct ui_placement {
    enum ui_place_kind kind;
    struct rect2i anchor;
    int gap;
    float y_fraction;
};

/* One cached part style, owned by its widget. Slots never move while the
 * widget lives. See ui_widget_part(). */
struct ui_part_slot {
    struct ui_part_slot *next;
    struct css_style style;
    uint64_t context_generation;
    uint64_t style_generation;
    ui_part_id part;
    unsigned pseudo;
    bool valid;
};

/*
 * A widget is a node in a tree. The caller owns its storage. Change the
 * tree and the widget's flags through the calls below, never through the
 * fields: the calls keep focus, layout, and frame requests right.
 *
 * Authors of new widget types read ops, userdata, the tree links, bounds,
 * measured_size, style, and style_scale. The rest of the API for them is
 * in ui/widget_impl.h. Nothing outside src/ui reads impl.
 */
struct ui_widget {
    const struct ui_widget_ops *ops;
    void *userdata;
    struct ui_widget *parent;
    struct ui_widget *first_child;
    struct ui_widget *last_child;
    struct ui_widget *prev_sibling;
    struct ui_widget *next_sibling;
    /* Surface space. Layout sets it. */
    struct rect2i bounds;
    /* Clay bounds before raster rounding. */
    struct rect2f layout_bounds;
    struct vector2i measured_size;
    /* The resolved style and its scale. Valid after the surface prepares. */
    struct css_style style;
    struct css_scale_context style_scale;
    struct {
        struct ui_surface *surface;
        struct ui_action_handler actions;
        ui_event_fn event_fn;
        void *event_userdata;
        ui_capture_fn key_capture;
        void *key_capture_userdata;
        uint64_t accessibility_id;
        char *accessible_name;
        char *accessible_description;
        bool accessibility_ignored;
        bool accessibility_invalid;
        struct ui_placement placement;
        struct vector2i fixed_size;
        const char *style_class;
        const char *style_id;
        const char *style_tag;
        uint64_t style_generation;
        struct ui_part_slot *part_slots;
        uint64_t deadline;
        size_t child_count;
        unsigned style_pseudo;
        unsigned invalidation;
        bool style_valid;
        bool destroyed;
        bool hidden;
        bool focusable;
        bool modal;
        bool pointer_transparent;
    } impl;
};

/*
 * Lifecycle.
 *
 * ui_widget_destroy() releases what the widget owns and destroys its
 * descendants. It never frees the widget's storage, so widgets can be
 * embedded in other structs or live on the stack. Do not copy a widget
 * after init.
 *
 * Removing a subtree from its surface (remove_child, a new surface root,
 * ui_surface_destroy) clears focus, capture, and hover inside it. The
 * focused widget then gets UI_EVENT_FOCUS_OUT.
 *
 * remove_child and destroy are safe inside an event, action, or tick
 * callback. Keep the storage alive until that dispatch returns.
 */
/* Call once for fresh storage. Destroy before initializing it again. ops
 * and userdata are borrowed until the widget is destroyed. */
void ui_widget_init(struct ui_widget *widget,
                    const struct ui_widget_ops *ops, void *userdata);
void ui_widget_destroy(struct ui_widget *widget);
/* Stable and nonzero for the lifetime of this initialized widget. */
uint64_t ui_widget_accessibility_id(const struct ui_widget *widget);

/* Sets an owned accessible name. NULL clears it. CSS identity and visible
 * placeholder text are never used as fallback names. */
bool ui_widget_set_accessible_name(struct ui_widget *widget, const char *name);
/* Sets help text for the accessible node. NULL clears it. */
bool ui_widget_set_accessible_description(struct ui_widget *widget,
                                          const char *description);
/* Adds or clears the invalid state on the accessible node. */
bool ui_widget_set_accessible_invalid(struct ui_widget *widget, bool invalid);
/* Hides a redundant child from the accessibility tree. */
void ui_widget_set_accessibility_ignored(struct ui_widget *widget,
                                         bool ignored);

/* Children paint in order; the last child is on top. add_child fails when
 * the child already has a parent or the parent is full. */
bool ui_widget_add_child(struct ui_widget *parent, struct ui_widget *child);
/* Add multiple children in order. Returns true when all children are
 * added. If any child fails, no child is added. */
bool ui_widget_add_children(struct ui_widget *parent,
                            struct ui_widget * const children[], size_t count);
/* Links child just before `before` (a child of parent), or last when
 * before is NULL. A child of parent moves without losing focus or
 * capture. */
bool ui_widget_insert_child(struct ui_widget *parent, struct ui_widget *child,
                            struct ui_widget *before);
bool ui_widget_remove_child(struct ui_widget *parent, struct ui_widget *child);
size_t ui_widget_child_count(const struct ui_widget *widget);
struct ui_widget *ui_widget_child_at(struct ui_widget *widget, size_t index);
struct ui_surface *ui_widget_surface(const struct ui_widget *widget);

/* A hidden widget and its subtree do not paint, take input, take focus,
 * tick, or take space in a layout. Hiding clears focus, capture, and
 * hover inside the subtree. */
void ui_widget_set_hidden(struct ui_widget *widget, bool hidden);
bool ui_widget_is_hidden(const struct ui_widget *widget);
void ui_widget_set_placement(struct ui_widget *widget,
                             struct ui_placement placement);
/* A nonzero axis fixes the widget's size on that axis. It wins over the
 * natural size and the style, like an inline style. */
void ui_widget_set_fixed_size(struct ui_widget *widget, struct vector2i size);

/* Actions bubble from the source to its ancestors until a handler returns
 * true. The handler stays set when the widget moves to another surface.
 * The widget borrows userdata until the handler is changed or the widget
 * is destroyed. */
void ui_widget_set_action_handler(struct ui_widget *widget, ui_action_fn fn,
                                  void *userdata);

/* The handler runs after the widget type's event callback returns false.
 * An installed handler receives key releases even when the widget type
 * does not opt in to them.
 * The widget borrows userdata until the handler is changed or the widget
 * is destroyed. The handler may change or destroy widgets. Keep their
 * storage alive until dispatch returns. */
void ui_widget_set_event_handler(struct ui_widget *widget, ui_event_fn fn,
                                 void *userdata);

/* Key capture runs for keys, text, paste, drops, and shortcuts that the
 * surface dispatches. Before they bubble, the capture path runs from the
 * active modal, or the surface root when no modal is open, down to the
 * target. Each widget on it gets its capture function, outermost first.
 * The first one that returns true handles the event. Bubbling then does
 * not run. The capture functions filter the event kinds and releases they
 * do not want. A path deeper than 64 widgets runs no capture. NULL clears
 * the capture. The widget borrows userdata until the capture is changed or
 * the widget is destroyed. */
void ui_widget_set_key_capture(struct ui_widget *widget, ui_capture_fn fn,
                               void *userdata);

/* Delivers an event to the subtree as a surface would, without focus or
 * capture. A pointer event goes to the widget under its position. Other
 * events go to the subtree's visible modal, or to widget itself. They
 * then bubble up. True when a widget handled the event. */
bool ui_widget_event(struct ui_widget *widget, const struct ui_event *event);

/* CSS selectors match class, id, and tag. The strings are borrowed until
 * they are changed or the widget is destroyed. */
void ui_widget_set_style_identity(struct ui_widget *widget,
                                  const char *class_name,
                                  const char *id_name,
                                  const char *tag_name);

/* Coordinates. Bounds and event positions are in surface space. A widget
 * paints in its local frame, whose origin is the top left of its bounds. */
static inline struct vector2i ui_widget_to_local(const struct ui_widget *widget,
                                                 struct vector2i surface)
{
    return vector2i_sub(surface, widget->bounds.pos);
}

static inline struct vector2i ui_widget_to_surface(const struct ui_widget *widget,
                                                   struct vector2i local)
{
    return vector2i_add(local, widget->bounds.pos);
}

static inline struct rect2i ui_widget_rect_to_surface(const struct ui_widget *widget,
                                                      struct rect2i local)
{
    return rect2i_translate(local, widget->bounds.pos);
}

#endif
