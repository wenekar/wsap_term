#ifndef WINDOW_DEBUG_H
#define WINDOW_DEBUG_H

#include <stdbool.h>

#include "input/input.h"

struct yasl_loop;
struct window_frame;
struct ui_scene;
struct window;
struct window_host;

#define WINDOW_DEBUG_MAIN 0

struct window_debug_hooks {
    bool (*option_set)(void *ud, const char *key, const char *value);
    void *ud;
};

void window_debug_set_hooks(const struct window_debug_hooks *hooks);

bool window_debug_server_start(struct yasl_loop *loop, struct window_host *host,
                               const char *path);
void window_debug_server_stop(void);

void window_debug_end_scene(struct window *w, int surface,
                            struct ui_scene *scene);
void window_debug_presented(struct window *w, int surface,
                            const struct window_frame *frame);
bool window_debug_wants_pixels(const struct window *w, int surface);

void window_debug_toggle_overlay(struct window *w);

void window_debug_destroy(struct window *w);

char *window_debug_eval(const char *line);

#endif
