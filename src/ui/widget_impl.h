#ifndef WSAP_UI_WIDGET_IMPL_H
#define WSAP_UI_WIDGET_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/constraints.h"
#include "ui/accessibility.h"
#include "ui/css.h"
#include "ui/event.h"
#include "ui/paint.h"
#include "ui/widget.h"
#include "util/geom.h"

struct ui_debug_parts;

uint64_t ui_widget_accessibility_id_allocate(void);

enum ui_widget_invalidation {
    UI_WIDGET_INVALIDATE_NONE = 0,
    UI_WIDGET_INVALIDATE_MEASURE = 1 << 0,
    UI_WIDGET_INVALIDATE_LAYOUT = 1 << 1,
    UI_WIDGET_INVALIDATE_PAINT = 1 << 2,
};

struct ui_widget_ops {
    const char *name;
    void (*prepare)(struct ui_widget *widget);
    struct vector2i (*measure)(const struct ui_widget *widget,
                               struct ui_constraints constraints);
    void (*layout)(struct ui_widget *widget, struct rect2i bounds);
    bool (*event)(struct ui_widget *widget, const struct ui_event *event);
    bool (*hit_test)(const struct ui_widget *widget, struct vector2i pos);
    bool (*hit_front)(const struct ui_widget *widget, struct vector2i pos);
    uint64_t (*tick)(struct ui_widget *widget, uint64_t now_ms);
    void (*destroy)(struct ui_widget *widget);
    void (*paint)(const struct ui_widget *widget,
                  struct ui_paint_context *paint);
    void (*paint_front)(const struct ui_widget *widget,
                        struct ui_paint_context *paint);
    void (*style_changed)(struct ui_widget *widget);
    void (*reveal)(struct ui_widget *widget, struct ui_widget *descendant);
    enum ui_cursor (*cursor)(const struct ui_widget *widget,
                             struct vector2i pos);
    bool (*keeps_press_focus)(const struct ui_widget *widget,
                              struct vector2i pos);
    void (*debug_parts)(const struct ui_widget *widget,
                        struct ui_debug_parts *out);
    bool (*accessibility)(const struct ui_widget *widget,
                          struct ui_accessibility_node_spec *out);
    size_t (*accessibility_child_count)(const struct ui_widget *widget);
    bool (*accessibility_child)(const struct ui_widget *widget, size_t index,
                                struct ui_accessibility_node_spec *out);
    bool (*accessibility_action)(struct ui_widget *widget, uint64_t node_id,
                                 const struct ui_accessibility_action_request *request);
    bool clip_children;
    bool key_release;
    bool width_sensitive;
    size_t child_limit;
};

void ui_widget_invalidate(struct ui_widget *widget, unsigned flags);

void ui_widget_set_focusable(struct ui_widget *widget, bool focusable);
void ui_widget_set_modal(struct ui_widget *widget, bool modal);
void ui_widget_set_pointer_transparent(struct ui_widget *widget,
                                       bool transparent);
void ui_widget_set_style_pseudo(struct ui_widget *widget, unsigned pseudo_flags);

bool ui_widget_notify(struct ui_widget *source, struct ui_action action);

void ui_widget_focus(struct ui_widget *widget);
bool ui_widget_has_focus(const struct ui_widget *widget);
void ui_widget_capture_pointer(struct ui_widget *widget);
void ui_widget_release_pointer(struct ui_widget *widget);
bool ui_widget_is_captured(const struct ui_widget *widget);

struct vector2i ui_widget_measure(struct ui_widget *widget,
                                  struct ui_constraints constraints);
void ui_widget_place(struct ui_widget *widget, struct rect2i bounds);
void ui_widget_place_child(struct ui_widget *child, struct rect2i parent_bounds);

struct css_style ui_widget_part_style(const struct ui_widget *widget,
                                      const char *class_name,
                                      unsigned pseudo_flags);

struct css_style ui_widget_part(const struct ui_widget *widget,
                                ui_part_id part, unsigned pseudo_flags);

#endif
