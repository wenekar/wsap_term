#define _GNU_SOURCE
#include <stdlib.h>

#include "ui/yasl/debug_internal.h"

bool dbg_server_running;

struct window_debug *dbg_state(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return NULL;
    if (!ui->debug)
        ui->debug = calloc(1, sizeof(*ui->debug));
    return ui->debug;
}

bool dbg_surface_exists(const struct window *w, int surface)
{
    if (surface == WINDOW_DEBUG_MAIN)
        return true;
    const struct window_ui *ui = window_ui_of(w);
    int level = surface - 1;
    return ui && level >= 0 && level < ui->menu_popups;
}

struct ui_debug_view dbg_view(struct window *w, int surface)
{
    struct ui_debug_view view = { .name = "window" };
    struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return view;
    if (surface == WINDOW_DEBUG_MAIN) {
        view.root = &ui->root;
        view.surface = &ui->screen;
    } else if (dbg_surface_exists(w, surface)) {
        struct ui_surface *popup = &ui->menu_popup[surface - 1].ui;
        view.name = "popup";
        view.root = ui_surface_root(popup);
        view.surface = popup;
    }
    if (ui->debug && surface >= 0 && surface < DBG_SURFACES &&
        ui->debug->captured[surface])
        view.scene = &ui->debug->scene[surface];
    return view;
}

void dbg_redraw(struct window *w, int surface)
{
    (void)surface;
    window_redraw(w);
}

bool dbg_inject(struct window *w, const struct input_event *ev)
{
    return window_inject_input(w, ev);
}

bool dbg_inject_scratch(struct window *w)
{
    struct window_debug *dbg = dbg_state(w);
    if (!dbg)
        return false;
    bool ok = true;
    struct input_event ev;
    while (input_queue_take(&dbg->scratch, &ev))
        if (!window_inject_input(w, &ev))
            ok = false;
    return ok;
}

static bool capturing(const struct window *w)
{
    const struct window_ui *ui = window_ui_of(w);
    return dbg_server_running || (ui && ui->debug && ui->debug->overlay.flags);
}

void window_debug_end_scene(struct window *w, int surface, struct ui_scene *scene)
{
    if (surface < 0 || surface >= DBG_SURFACES || !capturing(w))
        return;
    struct window_debug *dbg = dbg_state(w);
    if (!dbg)
        return;
    dbg->captured[surface] = ui_scene_copy(&dbg->scene[surface], scene);
    if (!dbg->overlay.flags)
        return;
    struct ui_debug_overlay overlay = dbg->overlay;
    if (surface == WINDOW_DEBUG_MAIN) {
        struct window_pointer pointer = window_pointer(w);
        overlay.pointer = pointer.pos;
        overlay.has_pointer = pointer.inside;
    }
    struct ui_debug_view view = dbg_view(w, surface);
    ui_debug_overlay_append(scene, &view, &overlay);
}

void window_debug_toggle_overlay(struct window *w)
{
    struct window_debug *dbg = dbg_state(w);
    if (!dbg)
        return;
    dbg->overlay.flags = dbg->overlay.flags ? 0 : UI_DEBUG_OVERLAY_DEFAULT;
    for (int s = 0; s < DBG_SURFACES; s++)
        if (dbg_surface_exists(w, s))
            dbg_redraw(w, s);
}

void window_debug_free(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    struct window_debug *dbg = ui ? ui->debug : NULL;
    if (!dbg)
        return;
    for (int i = 0; i < DBG_SURFACES; i++)
        ui_scene_destroy(&dbg->scene[i]);
    free(dbg);
    ui->debug = NULL;
}
