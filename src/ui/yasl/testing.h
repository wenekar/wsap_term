#ifndef WINDOW_TESTING_H
#define WINDOW_TESTING_H

/* Test-only seams of the window. Production code does not use them. */

#include <stdbool.h>

#include "yasl/yasl.h"

struct input_event;
struct yasl_surface;
struct ui_menu;
struct ui_menu_layer;
struct ui_scene;
struct window;

/* The menu of the window's UI. */
struct ui_menu *window_ui_menu(struct window *w);
struct ui_menu_layer *window_ui_menu_layer(struct window *w);
/* How many menu levels show in native popups. */
int window_ui_menu_popups(const struct window *w);
/* A scene to build into, reset; valid until the next call. */
struct ui_scene *window_ui_scene_scratch(struct window *w);
/* Routes ev the way the window routes native input to its UI. */
void window_ui_test_route(struct window *w, struct input_event *ev);

#endif
