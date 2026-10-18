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
    struct window_native *native = yasl_surface_get_userdata(surface);
    if (!native || native->window != w || !window_ui_of(w))
        return false;
    int surface_id = native->id;
    bool main = surface_id == WINDOW_DEBUG_MAIN;
    struct ui_surface *ui = window_ui_surface_at(w, surface_id);
    if (!main) {
        (void)ui_surface_take_frame_request(ui);
        (void)ui_surface_tick(ui, now_ms);
    }
    out->font = window_ui_of(w)->ui_font;
    out->background = main ? window_ui_of(w)->background : 0;
    out->gpu_off = window_debug_wants_pixels(w, surface_id);
    if (window_suspended(w) && main) {
        out->suspended = true;
        out->repaint = true;
        return true;
    }
    struct ui_scene *scene = build_ui(w, ui, surface_id);
    if (main) {
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
    struct window_native *native = frame->surface
        ? yasl_surface_get_userdata(frame->surface) : NULL;
    if (native && native->window == w)
        window_debug_presented(w, native->id, frame);
    window_ui_arm(w, now_ms);
}
