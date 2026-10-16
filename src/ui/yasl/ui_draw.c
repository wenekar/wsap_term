#include <stdlib.h>

#include "ui/damage.h"
#include "paint/scene.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/inspector.h"

struct ui_scene *window_ui_scene_scratch(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return NULL;
    if (!ui->scene_scratch) {
        ui->scene_scratch = calloc(1, sizeof(*ui->scene_scratch));
        if (!ui->scene_scratch)
            return NULL;
    }
    ui_scene_init(ui->scene_scratch);
    return ui->scene_scratch;
}

/* Builds the scene, with the inspector overlay on top. NULL when the
 * scene could not be built. */
static struct ui_scene *build_ui(struct window *w, struct ui_surface *ui,
                                 int surface_id)
{
    struct ui_scene *scene = window_ui_scene_scratch(w);
    if (!scene || !ui_surface_build_scene(ui, scene))
        return NULL;
    window_debug_end_scene(w, surface_id, scene);
    return ui_scene_is_valid(scene) ? scene : NULL;
}

void window_ui_prepare_frame(struct window *w, uint64_t now_ms)
{
    struct ui_surface *ui = &ui_of(w)->screen;
    (void)ui_surface_take_frame_request(ui);
    bool animating = ui_surface_tick(&ui_of(w)->screen, now_ms);
    /* The tick may open a submenu: show it before building the scene. */
    window_menu_sync(w);
    if (animating)
        window_redraw(w);
}

bool window_ui_build_frame(struct window *w, struct yasl_surface *surface,
                           uint64_t now_ms, struct window_ui_frame *out)
{
    if (!w || !surface || !out)
        return false;
    *out = (struct window_ui_frame){0};
    bool main = surface == window_surface(w);
    int surface_id = WINDOW_DEBUG_MAIN;
    struct ui_surface *ui = window_ui_surface_of(w, surface);
    if (!ui)
        return false;
    if (!main) {
        bool found = false;
        struct window_ui *state = window_ui_of(w);
        for (int i = 0; i < state->menu_popups; i++) {
            if (state->menu_popup[i].surface == surface) {
                surface_id = i + 1;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        (void)ui_surface_take_frame_request(ui);
        (void)ui_surface_tick(ui, now_ms);
    }
    out->font = window_ui_of(w)->ui_font;
    /* A popup shows what lies behind its shadow margin, so it stays clear. */
    out->background = main ? window_ui_of(w)->background : 0;
    out->gpu_off = window_debug_wants_pixels(w, surface_id);
    if (window_suspended(w) && main) {
        out->suspended = true;
        out->repaint = true;
        return true;
    }
    struct ui_scene *scene = build_ui(w, ui, surface_id);
    if (main) {
        /* What is under the pointer may have changed without it moving. */
        window_ui_update_cursor(w);
        ui_damage_track(&window_ui_of(w)->ui_damage, scene, &out->damage);
        window_accessibility_publish(w);
    }
    out->scene = scene;
    out->repaint = main && (window_take_repaint(w) || !scene);
    return true;
}

void window_ui_frame_done(struct window *w, struct window_frame *frame,
                          uint64_t now_ms)
{
    if (!w || !frame)
        return;
    int surface_id = WINDOW_DEBUG_MAIN;
    if (frame->surface != window_surface(w)) {
        struct window_ui *ui = window_ui_of(w);
        for (int i = 0; i < ui->menu_popups; i++)
            if (ui->menu_popup[i].surface == frame->surface)
                surface_id = i + 1;
    }
    window_debug_presented(w, surface_id, frame);
    window_ui_arm(w, now_ms);
}
