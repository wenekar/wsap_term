#ifndef WINDOW_DEBUG_INTERNAL_H
#define WINDOW_DEBUG_INTERNAL_H

#include "input/input.h"
#include "input/keybind.h"
#include "ui/debug.h"
#include "paint/scene.h"
#include "util/json.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/inspector.h"

#define DBG_SURFACES (1 + UI_MENU_MAX_LEVELS)

struct window_debug {
    struct ui_scene scene[DBG_SURFACES];
    bool captured[DBG_SURFACES];
    struct ui_debug_overlay overlay;
    struct input_shortcut_state keys;
    struct input_queue scratch;
};

struct dbg_client {
    struct dbg_client *next;
    int fd;
    int watch;
    char *in;
    size_t in_len, in_cap;
    char *out;
    size_t out_len, out_cap;
    bool waiting;
    bool dead;
};

enum dbg_pending_kind {
    DBG_PENDING_FRAME,
    DBG_PENDING_SHOT,
};

extern bool dbg_server_running;
void window_debug_free(struct window *w);
struct window_debug *dbg_state(struct window *w);
bool dbg_surface_exists(const struct window *w, int surface);
struct ui_debug_view dbg_view(struct window *w, int surface);
void dbg_redraw(struct window *w, int surface);
bool dbg_inject(struct window *w, const struct input_event *ev);
bool dbg_inject_scratch(struct window *w);

bool dbg_run_line(struct dbg_client *c, char *line);

const struct window_debug_hooks *dbg_hooks(void);
void dbg_reply_ok(struct dbg_client *c, struct json_writer *result);
void dbg_reply_err(struct dbg_client *c, const char *msg);
bool dbg_pending_add(struct dbg_client *c, struct window *w, int surface,
                     enum dbg_pending_kind kind, const char *path,
                     char *err, size_t err_size);

#endif
