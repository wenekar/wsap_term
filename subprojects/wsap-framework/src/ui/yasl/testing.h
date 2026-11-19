#ifndef WINDOW_TESTING_H
#define WINDOW_TESTING_H

#include <stdbool.h>

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

#endif
