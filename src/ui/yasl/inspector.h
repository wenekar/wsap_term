#ifndef WINDOW_DEBUG_H
#define WINDOW_DEBUG_H

/*
 * Window side of the UI inspector (ui/debug.h). It keeps a copy of the
 * last scene each window surface built, draws the overlay, and serves a
 * line protocol on a Unix socket: inspection, overlay control,
 * screenshots, and input injection. Injected input enters the same
 * window_drain_input() path as native input. docs/ui-debugging.md has
 * the protocol.
 *
 * Surface 0 is the window itself; surface L + 1 is menu popup level L.
 */

#include <stdbool.h>

#include "input/input.h"

struct yasl_loop;
struct window_frame;
struct ui_scene;
struct window;
struct window_host;

#define WINDOW_DEBUG_MAIN 0

/* What the inspector asks of the app. Every hook is optional. */
struct window_debug_hooks {
    /* Sets one option by its config key. */
    bool (*option_set)(void *ud, const char *key, const char *value);
    void *ud;
};

/* Sets the app's hooks. The inspector keeps a copy. NULL clears them.
 * The hooks stay set when the server stops, so window_debug_eval()
 * reaches them too. */
void window_debug_set_hooks(const struct window_debug_hooks *hooks);

/* One server per process, serving host's windows. False (with a message on
 * stderr) when the socket cannot be created. */
bool window_debug_server_start(struct yasl_loop *loop, struct window_host *host,
                               const char *path);
void window_debug_server_stop(void);

/* Frame hooks. end copies the built scene for inspection and then appends
 * the overlay to it; the overlay's ops are damage like any other ops.
 * presented runs after the frame is rendered: it takes pending
 * screenshots and answers frame waits. */
void window_debug_end_scene(struct window *w, int surface,
                            struct ui_scene *scene);
void window_debug_presented(struct window *w, int surface,
                            const struct window_frame *frame);
/* True while a screenshot of the surface waits. A screenshot reads the
 * CPU pixels, so that frame must not be drawn on the GPU. */
bool window_debug_wants_pixels(const struct window *w, int surface);

void window_debug_toggle_overlay(struct window *w);

/* Releases the window's state and fails its pending requests. */
void window_debug_destroy(struct window *w);

/* Runs one protocol line in-process and returns its JSON reply (caller
 * frees). For debuggers attached to a stopped process: it works without
 * the socket server; commands that inject input or wait for a frame are
 * refused, since a stopped process draws nothing. */
char *window_debug_eval(const char *line);

#endif
