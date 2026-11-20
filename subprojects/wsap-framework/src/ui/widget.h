#ifndef WSAP_UI_WIDGET_H
#define WSAP_UI_WIDGET_H

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paint/scene.h"
#include "ui/action.h"
#include "ui/css.h"
#include "ui/event.h"
#include "ui/style_context.h"
#include "util/geom.h"

#define ui_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define ui_container_of_const(ptr, type, member) \
    ((const type *)((const char *)(ptr) - offsetof(type, member)))

#define UI_WIDGET(w) (&(w)->widget)

struct ui_surface;
struct ui_widget_ops;
struct ui_widget;

typedef bool (*ui_event_fn)(struct ui_widget *widget,
                            const struct ui_event *event, void *userdata);

typedef bool (*ui_capture_fn)(struct ui_widget *widget,
                              const struct ui_event *event, void *userdata);

enum ui_place_kind : uint8_t {
    UI_PLACE_FILL = 0,
    UI_PLACE_BELOW,
    UI_PLACE_RIGHT,
    UI_PLACE_AT,
    UI_PLACE_CENTER,
};

struct ui_placement {
    struct rect2i anchor;
    int gap;
    float y_fraction;
    enum ui_place_kind kind;
};

struct ui_part_slot {
    struct ui_part_slot *next;
    uint64_t context_generation;
    uint64_t style_generation;
    ui_part_id part;
    uint32_t pseudo : 31;
    uint32_t valid : 1;
    struct css_style style;
};

/* The paint colors of one widget, moved across a CSS transition. The channels
 * are the background, the border, and the text colors. */
enum ui_style_channel : uint8_t {
    UI_STYLE_CHANNEL_BG = 0,
    UI_STYLE_CHANNEL_BORDER,
    UI_STYLE_CHANNEL_TEXT,
    UI_STYLE_CHANNEL_COUNT,
};

struct ui_widget_style_anim {
    uint32_t from[UI_STYLE_CHANNEL_COUNT];
    uint32_t to[UI_STYLE_CHANNEL_COUNT];
    uint32_t current[UI_STYLE_CHANNEL_COUNT];
    float opacity_from;
    float opacity_to;
    float opacity_current;
    uint64_t start_ms;
    uint32_t duration_ms;
    bool valid;
};

struct ui_widget {
    const struct ui_widget_ops *ops;
    struct ui_widget *first_child;
    struct ui_widget *last_child;
    struct ui_widget *prev_sibling;
    struct ui_widget *next_sibling;
    struct ui_widget *parent;
    struct rect2i bounds;
    void *userdata;
    struct rect2f layout_bounds;
    struct vector2i measured_size;
    /* The command scope of the widget. Zero is the app scope.
     * The key route reads the binding of this scope before an app binding. */
    uint8_t command_scope;
    struct {
        uint16_t style_pseudo;
        /* The hover and focus flags that the surface set last time. */
        uint16_t style_pseudo_auto;
        uint8_t invalidation;
        bool accessibility_ignored : 1;
        bool accessibility_invalid : 1;
        bool style_valid : 1;
        bool destroyed : 1;
        bool hidden : 1;
        bool focusable : 1;
        bool modal : 1;
        bool pointer_transparent : 1;
        bool keeps_press_focus : 1;
        struct vector2i fixed_size;
        struct ui_placement placement;
        struct ui_surface *surface;
        struct ui_action_handler actions;
        ui_event_fn event_fn;
        void *event_userdata;
        ui_capture_fn key_capture;
        void *key_capture_userdata;
        /* Warns an owner that another owner destroys the widget. The owner
         * clears the reference and keeps its own storage. */
        void (*destroy_observer)(struct ui_widget *widget, void *userdata);
        void *destroy_observer_userdata;
        uint64_t accessibility_id;
        char *accessible_name;
        char *accessible_description;
        const char *style_class;
        const char *style_id;
        const char *style_tag;
        const char *debug_key;
        uint64_t style_generation;
        struct ui_part_slot *part_slots;
        uint64_t deadline;
        size_t child_count;
        struct ui_widget_style_anim style_anim;
    } impl;
    struct css_style style;
    struct css_scale_context style_scale;
};

/* Initializes `widget` with operations table `ops` and user data `userdata`.
 * The call overwrites the storage. */
void ui_widget_init(struct ui_widget *widget,
                    const struct ui_widget_ops *ops, void *userdata);
/* Destroys `widget` and its subtree, and releases their resources. */
void ui_widget_destroy(struct ui_widget *widget);
/* Returns the unique accessibility node identifier of `widget`. */
uint64_t ui_widget_accessibility_id(const struct ui_widget *widget);
/* Resets the accessibility id sequence. Call it after the last widget is gone. */
void ui_widget_shutdown(void);

bool ui_widget_set_accessible_name(struct ui_widget *widget, const char *name);
bool ui_widget_set_accessible_description(struct ui_widget *widget,
                                          const char *description);
bool ui_widget_set_accessible_invalid(struct ui_widget *widget, bool invalid);
void ui_widget_set_accessibility_ignored(struct ui_widget *widget,
                                         bool ignored);

/* Appends `child` as the last child of `parent`. */
bool ui_widget_add_child(struct ui_widget *parent, struct ui_widget *child);
/* Appends `count` children to `parent`. A failed call removes the children
 * that it appended before the failure. */
bool ui_widget_add_children(struct ui_widget *parent,
                            struct ui_widget * const children[], size_t count);
/* Inserts `child` into `parent` immediately before `before`. */
bool ui_widget_insert_child(struct ui_widget *parent, struct ui_widget *child,
                            struct ui_widget *before);
/* Removes `child` from `parent`.
 *
 * Several calls send events to a subtree when the surface stops using it:
 * this call, ui_widget_destroy, ui_widget_set_hidden, and an insert that
 * moves a widget to another surface. The surface forgets the subtree, and
 * the widgets that held the focus, the hover, the capture, or the drag get
 * FOCUS_OUT, the pointer LEAVE, CAPTURE_LOST, or DRAG_LEAVE. A handler of
 * these events can detach or destroy any widget, its own included. It must
 * not free the storage of a widget in the subtree before the call returns.
 * The call still reads the subtree after the events, and the handlers still
 * use the surface of their widget. Free the storage after the call, or with
 * ui_surface_post when the surface is busy. */
bool ui_widget_remove_child(struct ui_widget *parent, struct ui_widget *child);
/* Returns the count of immediate children in `widget`. */
size_t ui_widget_child_count(const struct ui_widget *widget);
/* Returns the child widget at `index`, or NULL if out of bounds. */
struct ui_widget *ui_widget_child_at(struct ui_widget *widget, size_t index);
/* Returns the host surface of `widget`, or NULL if unmounted. */
struct ui_surface *ui_widget_surface(const struct ui_widget *widget);

void ui_widget_set_hidden(struct ui_widget *widget, bool hidden);
bool ui_widget_is_hidden(const struct ui_widget *widget);
/* Returns true when `widget` was destroyed. Use this only while the storage
 * of `widget` stays alive, for example inside a dispatch after a handler
 * returns. */
bool ui_widget_is_destroyed(const struct ui_widget *widget);
void ui_widget_set_placement(struct ui_widget *widget,
                             struct ui_placement placement);
struct ui_placement ui_widget_placement(const struct ui_widget *widget);
void ui_widget_set_fixed_size(struct ui_widget *widget, struct vector2i size);

/* Sets whether modal mode restricts input to the subtree of `widget`. */
void ui_widget_set_modal(struct ui_widget *widget, bool modal);
/* Sets whether `widget` passes pointer events through to the widgets
 * underneath it. */
void ui_widget_set_pointer_transparent(struct ui_widget *widget,
                                       bool transparent);
/* Returns true when `widget` or a descendant of it needs a measure or a
 * layout pass. */
bool ui_widget_needs_layout(const struct ui_widget *widget);
/* Returns true when `widget` needs a measure pass. */
bool ui_widget_needs_measure(const struct ui_widget *widget);
/* Returns true when the style of `widget` is resolved and still matches the
 * current sheet of its surface. A widget outside a surface needs no sheet
 * match, so it is current once resolved. A false result means that the
 * surface resolves the style again. */
bool ui_widget_is_style_current(const struct ui_widget *widget);

/* Registers action callback `fn` and `userdata` for `widget`. */
void ui_widget_set_action_handler(struct ui_widget *widget, ui_action_fn fn,
                                  void *userdata);

/* Registers event callback `fn` and `userdata` for `widget`. */
void ui_widget_set_event_handler(struct ui_widget *widget, ui_event_fn fn,
                                 void *userdata);

/* Registers key capture callback `fn` and `userdata` for `widget`. */
void ui_widget_set_key_capture(struct ui_widget *widget, ui_capture_fn fn,
                               void *userdata);

/* Registers a function that ui_widget_destroy calls for `widget`. The call
 * runs while the widget storage is still readable, after its children are
 * destroyed and its ops destroy callback runs. The function must not use the
 * widget as a live widget. Pass NULL to remove the observer. */
void ui_widget_set_destroy_observer(struct ui_widget *widget,
                                    void (*fn)(struct ui_widget *widget,
                                               void *userdata),
                                    void *userdata);

/* Dispatches `event` in the tree of `widget`: a pointer event goes to the
 * widget under the pointer, and any other event goes to the modal widget when
 * one is open, or else to `widget`. */
bool ui_widget_event(struct ui_widget *widget, const struct ui_event *event);

/* Sets CSS class, id, and tag names. The widget borrows the strings; keep them
 * alive until it is destroyed or gets new names. */
void ui_widget_set_style_identity(struct ui_widget *widget,
                                  const char *class_name,
                                  const char *id_name,
                                  const char *tag_name);

/* Sets the inspector key. The widget borrows `key`; keep it alive until the
 * widget is destroyed. */
void ui_widget_set_debug_key(struct ui_widget *widget, const char *key);

/* Converts surface coordinates to widget local coordinates. */
static inline struct vector2i ui_widget_to_local(const struct ui_widget *widget,
                                                 struct vector2i surface)
{
    return vector2i_sub(surface, widget->bounds.pos);
}

/* Converts a widget local rectangle to surface coordinates. */
static inline struct rect2i ui_widget_rect_to_surface(const struct ui_widget *widget,
                                                      struct rect2i local)
{
    return rect2i_translate(local, widget->bounds.pos);
}

/* Returns cell dimension `cell` in whole pixels, or `fallback` when `cell` is
 * not finite, is below one, or does not fit an int. */
static inline int ui_cell_dim(float cell, int fallback)
{
    return isfinite(cell) && cell >= 1.0f && cell < (float)INT_MAX
        ? (int)lroundf(cell) : fallback;
}

/* Returns the text style for `scale`, in pixels. */
static inline struct ui_text_style ui_widget_text_style(
    const struct css_scale_context *scale)
{
    return (struct ui_text_style){
        .advance = ui_cell_dim(scale->cell_w, UI_DEFAULT_CELL_W),
        .cell_h = ui_cell_dim(scale->cell_h, UI_DEFAULT_CELL_H),
    };
}

/* Returns the width of one text cell of `widget`, in pixels. */
static inline int ui_widget_cell_w(const struct ui_widget *widget)
{
    return ui_widget_text_style(&widget->style_scale).advance;
}

/* Returns the height of one text cell of `widget`, in pixels. */
static inline int ui_widget_cell_h(const struct ui_widget *widget)
{
    return ui_widget_text_style(&widget->style_scale).cell_h;
}

/* Border width of this widget, in pixels. Zero without a border color. */
static inline int ui_widget_border_width(const struct ui_widget *widget)
{
    return widget ? ui_css_border_width(&widget->style, &widget->style_scale) : 0;
}

#endif
