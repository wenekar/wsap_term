#ifndef WSAP_UI_INTERNAL_TREE_H
#define WSAP_UI_INTERNAL_TREE_H

#include "ui/widget_ops.h"

struct ui_scene;

void ui_widget_prepare(struct ui_widget *widget);
void ui_widget_layout(struct ui_widget *widget, struct rect2i bounds);
void ui_widget_attach(struct ui_widget *widget, struct ui_surface *surface);
void ui_widget_detach(struct ui_widget *widget);
bool ui_widget_bubble(struct ui_widget *target, const struct ui_event *event);
/* Bubbles `event` from `target` up to `last` at most. A NULL `last` walks to
 * the root. */
bool ui_widget_bubble_to(struct ui_widget *target, const struct ui_widget *last,
                         const struct ui_event *event);
bool ui_widget_deliver(struct ui_widget *widget, const struct ui_event *event);
struct ui_widget *ui_widget_hit_test(struct ui_widget *root,
                                     struct vector2i pos);
struct ui_widget *ui_widget_modal(struct ui_widget *root);
bool ui_widget_visible(const struct ui_widget *widget);
bool ui_widget_is_ancestor(const struct ui_widget *ancestor,
                           const struct ui_widget *widget);
uint64_t ui_widget_tick(struct ui_widget *widget, uint64_t now_ms);
/* Moves the colors and the opacity of `widget` and its children toward the
 * CSS style. Returns the time of the next step of a transition, no later than
 * the end of that transition, or 0 when nothing moves. */
uint64_t ui_widget_animate_style(struct ui_widget *widget, uint64_t now_ms);
const struct css_sheet *ui_widget_sheet(const struct ui_widget *widget);

void ui_widget_part_cache_stale(struct ui_widget *widget);
void ui_widget_part_cache_stale_subtree(struct ui_widget *widget);
void ui_widget_part_cache_free(struct ui_widget *widget);

bool ui_widget_append_scene(const struct ui_widget *root,
                            struct ui_scene *scene);

void ui_surface_forget(struct ui_surface *surface, struct ui_widget *subtree);
/* Removes `overlay` from the overlay list of `surface`. It does not detach
 * the widget, and it keeps its storage. */
bool ui_surface_drop_overlay(struct ui_surface *surface,
                             struct ui_widget *overlay);
void ui_surface_sync_modal(struct ui_surface *surface);

#endif
