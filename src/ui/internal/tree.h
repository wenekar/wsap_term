#ifndef WSAP_UI_INTERNAL_TREE_H
#define WSAP_UI_INTERNAL_TREE_H

/* Tree machinery that only src/ui uses. */

#include "ui/widget_impl.h"

struct ui_scene;

void ui_widget_prepare(struct ui_widget *widget);
void ui_widget_layout(struct ui_widget *widget, struct rect2i bounds);
/* Set surface on the subtree. A widget on another surface is first
 * removed from it. */
void ui_widget_attach(struct ui_widget *widget, struct ui_surface *surface);
/* Clears the subtree's focus, capture, hover, and modal on its surface,
 * and its surface pointer. Does not unlink it from its parent. */
void ui_widget_detach(struct ui_widget *widget);
/* Deliver once to the target, then to each ancestor until handled. */
bool ui_widget_bubble(struct ui_widget *target, const struct ui_event *event);
struct ui_widget *ui_widget_hit_test(struct ui_widget *root,
                                     struct vector2i pos);
/* The topmost visible modal in the tree, or NULL. */
struct ui_widget *ui_widget_modal(struct ui_widget *root);
bool ui_widget_visible(const struct ui_widget *widget);
bool ui_widget_is_ancestor(const struct ui_widget *ancestor,
                           const struct ui_widget *widget);
/* Ticks the visible subtree. Returns the earliest deadline, or 0. */
uint64_t ui_widget_tick(struct ui_widget *widget, uint64_t now_ms);
void ui_widget_clear_invalidation(struct ui_widget *widget, unsigned flags);
/* The sheet the widget's surface styles with. */
const struct css_sheet *ui_widget_sheet(const struct ui_widget *widget);

/* Part style cache for ui_widget_part(). Stale marks the widget's cached
 * entries, so the next call resolves them again. The subtree form marks
 * the widget and all its descendants. Free releases the cache. Destroy
 * calls it. */
void ui_widget_part_cache_stale(struct ui_widget *widget);
void ui_widget_part_cache_stale_subtree(struct ui_widget *widget);
void ui_widget_part_cache_free(struct ui_widget *widget);

bool ui_widget_build_scene(const struct ui_widget *root,
                           struct ui_scene *scene);
bool ui_widget_append_scene(const struct ui_widget *root,
                            struct ui_scene *scene);

/* Surface side of the tree calls. */
void ui_surface_forget(struct ui_surface *surface, struct ui_widget *subtree);
void ui_surface_sync_modal(struct ui_surface *surface);

#endif
