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
    enum ui_place_kind kind;
    struct rect2i anchor;
    int gap;
    float y_fraction;
};

struct ui_part_slot {
    struct ui_part_slot *next;
    struct css_style style;
    uint64_t context_generation;
    uint64_t style_generation;
    ui_part_id part;
    unsigned pseudo;
    bool valid;
};

struct ui_widget {
    const struct ui_widget_ops *ops;
    void *userdata;
    struct ui_widget *parent;
    struct ui_widget *first_child;
    struct ui_widget *last_child;
    struct ui_widget *prev_sibling;
    struct ui_widget *next_sibling;
    struct rect2i bounds;
    struct rect2f layout_bounds;
    struct vector2i measured_size;
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

void ui_widget_init(struct ui_widget *widget,
                    const struct ui_widget_ops *ops, void *userdata);
void ui_widget_destroy(struct ui_widget *widget);
uint64_t ui_widget_accessibility_id(const struct ui_widget *widget);

bool ui_widget_set_accessible_name(struct ui_widget *widget, const char *name);
bool ui_widget_set_accessible_description(struct ui_widget *widget,
                                          const char *description);
bool ui_widget_set_accessible_invalid(struct ui_widget *widget, bool invalid);
void ui_widget_set_accessibility_ignored(struct ui_widget *widget,
                                         bool ignored);

bool ui_widget_add_child(struct ui_widget *parent, struct ui_widget *child);
bool ui_widget_add_children(struct ui_widget *parent,
                            struct ui_widget * const children[], size_t count);
bool ui_widget_insert_child(struct ui_widget *parent, struct ui_widget *child,
                            struct ui_widget *before);
bool ui_widget_remove_child(struct ui_widget *parent, struct ui_widget *child);
size_t ui_widget_child_count(const struct ui_widget *widget);
struct ui_widget *ui_widget_child_at(struct ui_widget *widget, size_t index);
struct ui_surface *ui_widget_surface(const struct ui_widget *widget);

void ui_widget_set_hidden(struct ui_widget *widget, bool hidden);
bool ui_widget_is_hidden(const struct ui_widget *widget);
void ui_widget_set_placement(struct ui_widget *widget,
                             struct ui_placement placement);
void ui_widget_set_fixed_size(struct ui_widget *widget, struct vector2i size);

void ui_widget_set_action_handler(struct ui_widget *widget, ui_action_fn fn,
                                  void *userdata);

void ui_widget_set_event_handler(struct ui_widget *widget, ui_event_fn fn,
                                 void *userdata);

void ui_widget_set_key_capture(struct ui_widget *widget, ui_capture_fn fn,
                               void *userdata);

bool ui_widget_event(struct ui_widget *widget, const struct ui_event *event);

void ui_widget_set_style_identity(struct ui_widget *widget,
                                  const char *class_name,
                                  const char *id_name,
                                  const char *tag_name);

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
