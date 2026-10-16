#ifndef WSAP_UI_WIDGET_IMPL_H
#define WSAP_UI_WIDGET_IMPL_H

/* The API for writing a widget type. Screens use ui/ui.h instead. */

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

/* Allocates a stable nonzero ID for an accessibility child that is not a
 * ui_widget. Call once when the child is created. */
uint64_t ui_widget_accessibility_id_allocate(void);

enum ui_widget_invalidation {
    UI_WIDGET_INVALIDATE_NONE = 0,
    UI_WIDGET_INVALIDATE_MEASURE = 1 << 0,
    UI_WIDGET_INVALIDATE_LAYOUT = 1 << 1,
    UI_WIDGET_INVALIDATE_PAINT = 1 << 2,
};

struct ui_widget_ops {
    /* The type name the inspector reports. */
    const char *name;
    /* Update content and child links before style and layout. */
    void (*prepare)(struct ui_widget *widget);
    /* Report the natural content size. Without measure, the size is the
     * largest child that fills the parent. */
    struct vector2i (*measure)(const struct ui_widget *widget,
                               struct ui_constraints constraints);
    /* Place the children with ui_widget_place(). Without layout, each
     * child goes where its placement says. */
    void (*layout)(struct ui_widget *widget, struct rect2i bounds);
    /* Return true to stop the event at this widget. */
    bool (*event)(struct ui_widget *widget, const struct ui_event *event);
    /* The hit region in surface space, when it is not the bounds. */
    bool (*hit_test)(const struct ui_widget *widget, struct vector2i pos);
    /* A region drawn over the children, in surface space. When it returns
     * true, the widget takes the pointer before its children. Pair it with
     * paint_front. */
    bool (*hit_front)(const struct ui_widget *widget, struct vector2i pos);
    /* Runs once per frame while the widget is visible. Return the time of
     * the next tick the widget needs: 0 for none, or now_ms or earlier for
     * the next frame. The host wakes the surface at that time. */
    uint64_t (*tick)(struct ui_widget *widget, uint64_t now_ms);
    void (*destroy)(struct ui_widget *widget);
    /* Draw this widget's own content. The framework paints the children. */
    void (*paint)(const struct ui_widget *widget,
                  struct ui_paint_context *paint);
    /* Draw over the children. The framework calls it after them, in the
     * same local frame as paint. */
    void (*paint_front)(const struct ui_widget *widget,
                        struct ui_paint_context *paint);
    /* The style, the scale, or the sheet changed. Read widget->style,
     * widget->style_scale, and ui_widget_part_style() here. */
    void (*style_changed)(struct ui_widget *widget);
    /* Scroll a focused descendant into view. */
    void (*reveal)(struct ui_widget *widget, struct ui_widget *descendant);
    /* The pointer shape at pos, in surface space. Without it, or when it
     * returns UI_CURSOR_AUTO, the CSS cursor and then the parent decide. */
    enum ui_cursor (*cursor)(const struct ui_widget *widget,
                             struct vector2i pos);
    /* True when a press at pos keeps focus where it is. */
    bool (*keeps_press_focus)(const struct ui_widget *widget,
                              struct vector2i pos);
    /* Inspection only. Report the named sub-rects (tabs, items) that the
     * widget lays out without child widgets, in surface space. */
    void (*debug_parts)(const struct ui_widget *widget,
                        struct ui_debug_parts *out);
    /* Describe this widget in an accessibility snapshot. Return false to
     * omit the widget. Strings are borrowed; the tree copies them. */
    bool (*accessibility)(const struct ui_widget *widget,
                          struct ui_accessibility_node_spec *out);
    /* Describe stable virtual children, such as tabs drawn by one widget. */
    size_t (*accessibility_child_count)(const struct ui_widget *widget);
    bool (*accessibility_child)(const struct ui_widget *widget, size_t index,
                                struct ui_accessibility_node_spec *out);
    /* Handle an action for this widget or one of its virtual children. */
    bool (*accessibility_action)(struct ui_widget *widget, uint64_t node_id,
                                 const struct ui_accessibility_action_request *request);
    /* Clip child paint and pointer targeting to the bounds. */
    bool clip_children;
    /* Receive UI_KEY_RELEASE key events. */
    bool key_release;
    /* Measure again at the width that Clay assigns. */
    bool width_sensitive;
    /* Zero permits any number of children. */
    size_t child_limit;
};

/* A setter marks the work its change needs. MEASURE implies LAYOUT, and
 * LAYOUT implies PAINT. An attached widget then requests a frame. */
void ui_widget_invalidate(struct ui_widget *widget, unsigned flags);

void ui_widget_set_focusable(struct ui_widget *widget, bool focusable);
/* A visible modal subtree takes all pointer input and keeps focus inside
 * it. When it hides, the earlier focus comes back. */
void ui_widget_set_modal(struct ui_widget *widget, bool modal);
/* Pointer targeting skips the subtree. */
void ui_widget_set_pointer_transparent(struct ui_widget *widget,
                                       bool transparent);
void ui_widget_set_style_pseudo(struct ui_widget *widget, unsigned pseudo_flags);

/* Send an action up from source. True when a handler took it. */
bool ui_widget_notify(struct ui_widget *source, struct ui_action action);

/* Focus and pointer capture on the widget's surface. A captured widget
 * gets every pointer event until it releases the capture. */
void ui_widget_focus(struct ui_widget *widget);
bool ui_widget_has_focus(const struct ui_widget *widget);
void ui_widget_capture_pointer(struct ui_widget *widget);
/* Releases the capture when this widget holds it. */
void ui_widget_release_pointer(struct ui_widget *widget);
bool ui_widget_is_captured(const struct ui_widget *widget);

/* Measure applies the generic style sizes to the measure callback's
 * result: an explicit width or height replaces the natural size, and
 * min-width and min-height raise the floor. Maximum sizes set the limit. */
struct vector2i ui_widget_measure(struct ui_widget *widget,
                                  struct ui_constraints constraints);
/* Lays the widget out at bounds when they changed or layout is pending. */
void ui_widget_place(struct ui_widget *widget, struct rect2i bounds);
/* Places child where its placement says inside parent_bounds, as the
 * default layout does. A layout callback uses it for overlay children. */
void ui_widget_place_child(struct ui_widget *child, struct rect2i parent_bounds);

/* The style of a named part, such as "tab active", resolved against the
 * surface's sheet below this widget. Call it from style_changed. */
struct css_style ui_widget_part_style(const struct ui_widget *widget,
                                      const char *class_name,
                                      unsigned pseudo_flags);

/* The same style as ui_widget_part_style(widget, ui_part_name(part),
 * pseudo_flags), cached on the widget. Each (part, pseudo_flags) pair
 * resolves once per key. The key is the surface's style context
 * generation and the widget's style generation. Entries go stale when:
 *   - the sheet, the defaults, or the scale change (they bump the
 *     context generation);
 *   - the style identity of this widget or of an ancestor changes
 *     (ui_widget_set_style_identity walks the subtree);
 *   - the widget or an ancestor moves in the tree (insert or remove);
 *   - the widget moves to another surface or leaves its surface.
 * Editing the text of a borrowed class string in place does not stale an
 * entry. Set a new class string with ui_widget_set_style_identity().
 * The style returns by value. The caller owns the copy, and later style
 * changes do not alter it. A NULL widget gives an all-zero style. When
 * the cache cannot grow, the style is resolved and returned without
 * caching. The result is correct either way. Aborts with a message when
 * part is not a handle that ui_part_intern() returned. */
struct css_style ui_widget_part(const struct ui_widget *widget,
                                ui_part_id part, unsigned pseudo_flags);

#endif
