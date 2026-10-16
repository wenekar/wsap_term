#ifndef WINDOW_DEBUG_INTERNAL_H
#define WINDOW_DEBUG_INTERNAL_H

/* Shared by the inspector's three parts: per-window capture (debug.c),
 * commands (debug_cmd.c), and the socket server (debug_server.c). */

#include "input/input.h"
#include "input/keybind.h"
#include "ui/debug.h"
#include "paint/scene.h"
#include "util/json.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/inspector.h"

#define DBG_SURFACES (1 + UI_MENU_MAX_LEVELS)

/* The inspector's state for one window. A captured scene owns its
 * buffers: the window builds the next scene over its own. */
struct window_debug {
    struct ui_scene scene[DBG_SURFACES];
    bool captured[DBG_SURFACES];
    struct ui_debug_overlay overlay;
    struct input_shortcut_state keys;
    /* Where key routing and text chunking put events on their way to the
     * window's injected input. */
    struct input_queue scratch;
};

/* One peer of the protocol. fd < 0 is window_debug_eval(): replies stay
 * in out, and commands that need the loop are refused. */
struct dbg_client {
    struct dbg_client *next;
    int fd;
    int watch;
    char *in;
    size_t in_len, in_cap;
    char *out;
    size_t out_len, out_cap;
    /* A reply is pending: later lines wait. */
    bool waiting;
    bool dead;
};

enum dbg_pending_kind {
    DBG_PENDING_FRAME,
    DBG_PENDING_SHOT,
};

/* debug.c */
extern bool dbg_server_running;
void window_debug_free(struct window *w);
struct window_debug *dbg_state(struct window *w);
/* A surface is 0 for the window, L + 1 for menu popup level L: what the
 * socket's -p L names. */
bool dbg_surface_exists(const struct window *w, int surface);
struct ui_debug_view dbg_view(struct window *w, int surface);
void dbg_redraw(struct window *w, int surface);
bool dbg_inject(struct window *w, const struct input_event *ev);
/* Moves what a producer put in the window's scratch queue into its
 * injected input. */
bool dbg_inject_scratch(struct window *w);

/* debug_cmd.c */
/* Runs one line and replies to c. False when the reply is pending. */
bool dbg_run_line(struct dbg_client *c, char *line);

/* debug_server.c */
/* The app's hooks from window_debug_set_hooks(). Every hook is zero
 * when none were set. */
const struct window_debug_hooks *dbg_hooks(void);
void dbg_reply_ok(struct dbg_client *c, struct json_writer *result);
void dbg_reply_err(struct dbg_client *c, const char *msg);
/* Replies when the surface presents its next frame. False, with the
 * reason in err, when it never will. */
bool dbg_pending_add(struct dbg_client *c, struct window *w, int surface,
                     enum dbg_pending_kind kind, const char *path,
                     char *err, size_t err_size);

#endif
