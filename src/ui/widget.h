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
    struct {
        uint16_t style_pseudo;
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
    } impl;
    struct css_style style;
    struct css_scale_context style_scale;
};

/* Initializes `widget` with operations table `ops` and user data `userdata`. */
void ui_widget_init(struct ui_widget *widget,
                    const struct ui_widget_ops *ops, void *userdata);
/* Destroys `widget` and releases all associated resources. */
void ui_widget_destroy(struct ui_widget *widget);
/* Returns the unique accessibility node identifier of `widget`. */
uint64_t ui_widget_accessibility_id(const struct ui_widget *widget);

/* Sets the accessible name string of `widget`. */
bool ui_widget_set_accessible_name(struct ui_widget *widget, const char *name);
/* Sets the accessible description text of `widget`. */
bool ui_widget_set_accessible_description(struct ui_widget *widget,
                                          const char *description);
/* Marks the accessibility state of `widget` as invalid or valid. */
bool ui_widget_set_accessible_invalid(struct ui_widget *widget, bool invalid);
/* Sets whether accessibility tools ignore `widget`. */
void ui_widget_set_accessibility_ignored(struct ui_widget *widget,
                                         bool ignored);

/* Appends `child` as the last child of `parent`. */
bool ui_widget_add_child(struct ui_widget *parent, struct ui_widget *child);
/* Appends `count` children to `parent`. */
bool ui_widget_add_children(struct ui_widget *parent,
                            struct ui_widget * const children[], size_t count);
/* Inserts `child` into `parent` immediately before `before`. */
bool ui_widget_insert_child(struct ui_widget *parent, struct ui_widget *child,
                            struct ui_widget *before);
/* Removes `child` from `parent`. */
bool ui_widget_remove_child(struct ui_widget *parent, struct ui_widget *child);
/* Returns the count of immediate children in `widget`. */
size_t ui_widget_child_count(const struct ui_widget *widget);
/* Returns the child widget at `index`, or NULL if out of bounds. */
struct ui_widget *ui_widget_child_at(struct ui_widget *widget, size_t index);
/* Returns the host surface of `widget`, or NULL if unmounted. */
struct ui_surface *ui_widget_surface(const struct ui_widget *widget);

/* Sets the visibility of `widget`. */
void ui_widget_set_hidden(struct ui_widget *widget, bool hidden);
/* Returns true when `widget` is marked hidden. */
bool ui_widget_is_hidden(const struct ui_widget *widget);
/* Returns true when `widget` was destroyed. Read this only while the storage
 * of `widget` is alive, as inside a dispatch after a handler returns. */
bool ui_widget_is_destroyed(const struct ui_widget *widget);
/* Sets the placement descriptor of `widget`. */
void ui_widget_set_placement(struct ui_widget *widget,
                             struct ui_placement placement);
/* Returns the placement descriptor of `widget`. */
struct ui_placement ui_widget_placement(const struct ui_widget *widget);
/* Sets an explicit fixed layout size for `widget`. */
void ui_widget_set_fixed_size(struct ui_widget *widget, struct vector2i size);

/* Sets whether `widget` modal mode restricts input outside its subtree. */
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

/* Dispatches input `event` to `widget`. */
bool ui_widget_event(struct ui_widget *widget, const struct ui_event *event);

/* Sets CSS class, id, and tag names. Keep the strings alive while in use. */
void ui_widget_set_style_identity(struct ui_widget *widget,
                                  const char *class_name,
                                  const char *id_name,
                                  const char *tag_name);

/* Sets the inspector key. Keep key alive until the widget is destroyed. */
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

/* Returns cell dimension `cell` in whole pixels, or `fallback` below one. */
static inline int ui_cell_dim(float cell, int fallback)
{
    return isfinite(cell) && cell >= 1.0f && cell < (float)INT_MAX
        ? (int)lroundf(cell) : fallback;
}

/* The text style of this widget, in pixels. */
static inline struct ui_text_style ui_widget_text_style(
    const struct css_scale_context *scale)
{
    return (struct ui_text_style){
        .advance = ui_cell_dim(scale->cell_w, UI_DEFAULT_CELL_W),
        .cell_h = ui_cell_dim(scale->cell_h, UI_DEFAULT_CELL_H),
    };
}

/* One text cell of this widget, in pixels. */
static inline int ui_widget_cell_w(const struct ui_widget *widget)
{
    return ui_widget_text_style(&widget->style_scale).advance;
}

/* One text cell height of this widget, in pixels. */
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
