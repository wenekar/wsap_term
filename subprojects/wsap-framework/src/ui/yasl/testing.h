#ifndef WINDOW_TESTING_H
#define WINDOW_TESTING_H

#include <stdbool.h>

#include "ui/yasl/window.h"
#include "yasl/yasl.h"

struct input_event;
struct yasl_surface;
struct ui_menu;
struct ui_menu_layer;
struct ui_scene;
struct window;

struct ui_menu *window_ui_menu(struct window *w);
struct ui_menu_layer *window_ui_menu_layer(struct window *w);
int window_ui_menu_popups(const struct window *w);
struct ui_scene *window_ui_scene_scratch(struct window *w);
void window_ui_test_route(struct window *w, struct input_event *ev);
/* Applies `titlebar` to `w` as a WINDOW event of the platform would. Call it
 * to test a title bar overlay on a platform that has none. */
void window_test_set_titlebar(struct window *w, struct window_titlebar titlebar);
/* Applies the size, scale, and title bar in `info` to `w` as a WINDOW event
 * of the platform would. */
void window_test_apply_geometry(struct window *w, const struct yasl_surface_info *info);

#endif
