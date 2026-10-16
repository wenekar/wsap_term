#ifndef WSAP_UI_INTERNAL_TREE_H
#define WSAP_UI_INTERNAL_TREE_H

#include "ui/widget_impl.h"

struct ui_scene;

void ui_widget_prepare(struct ui_widget *widget);
void ui_widget_layout(struct ui_widget *widget, struct rect2i bounds);
void ui_widget_attach(struct ui_widget *widget, struct ui_surface *surface);
void ui_widget_detach(struct ui_widget *widget);
bool ui_widget_bubble(struct ui_widget *target, const struct ui_event *event);
struct ui_widget *ui_widget_hit_test(struct ui_widget *root,
                                     struct vector2i pos);
struct ui_widget *ui_widget_modal(struct ui_widget *root);
bool ui_widget_visible(const struct ui_widget *widget);
bool ui_widget_is_ancestor(const struct ui_widget *ancestor,
                           const struct ui_widget *widget);
uint64_t ui_widget_tick(struct ui_widget *widget, uint64_t now_ms);
void ui_widget_clear_invalidation(struct ui_widget *widget, unsigned flags);
const struct css_sheet *ui_widget_sheet(const struct ui_widget *widget);

void ui_widget_part_cache_stale(struct ui_widget *widget);
void ui_widget_part_cache_stale_subtree(struct ui_widget *widget);
void ui_widget_part_cache_free(struct ui_widget *widget);

bool ui_widget_build_scene(const struct ui_widget *root,
                           struct ui_scene *scene);
bool ui_widget_append_scene(const struct ui_widget *root,
                            struct ui_scene *scene);

void ui_surface_forget(struct ui_surface *surface, struct ui_widget *subtree);
void ui_surface_sync_modal(struct ui_surface *surface);

#endif
