#include <stdlib.h>

#include "font/font.h"
#include "ui/damage.h"
#include "paint/scene.h"
#include "ui/yasl/inspector.h"
#include "ui/yasl/testing.h"
#include "ui/yasl/internal.h"

struct window_host *window_ui_host_last;

struct window_ui *window_ui_of(const struct window *w)
{
    return w->ui.mounted ? ui_of(w) : NULL;
}

/* The UI font follows the surface's scale. */
static void update_ui_font(struct window *w)
{
    struct window_ui *ui = ui_of(w);
    struct window_host *host = window_host(w);
    struct font_instance *fi =
        font_instance_acquire(ui->spec.ui_font_size > 0 ? ui->spec.ui_font_size : host->config->font_size,
                              window_scale_120(w));
    if (!fi)
        return;
    font_instance_unref(ui->ui_font);
    ui->ui_font = fi;
}

struct css_scale_context window_ui_scale(const struct window *w)
{
    const struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return (struct css_scale_context){0};
    const struct font_metrics *m = font_instance_metrics(ui->ui_font);
    return (struct css_scale_context){
        .dpi_scale = (float)window_scale_120(w) / YASL_SCALE_UNIT,
        .cell_w = (float)m->cell_w,
        .cell_h = (float)m->cell_h,
    };
}

bool window_ui_mount(struct window *w, const struct window_ui_spec *spec)
{
    if (window_ui_of(w))
        return false;
    struct window_ui *ui = ui_of(w);
    ui->mounted = true;
    ui->spec = *spec;
    window_ui_host_last = window_host(w);
    ui->ui_font = font_instance_ref(font_instance_default());
    window_ui_root_init(w);
    struct yasl_surface *surface = window_surface(w);
    if (surface) {
        window_ui_relayout(w);
        window_accessibility_create(w);
        window_ui_state_changed(w);
        window_redraw(w);
    }
    return true;
}

const void *window_ui_kind(const struct window *w)
{
    const struct window_ui *ui = window_ui_of(w);
    return ui ? ui->spec.kind : NULL;
}

void window_ui_restyle(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return;
    update_ui_font(w);
    struct css_scale_context scale = window_ui_scale(w);
    ui_surface_set_scale(&ui->screen, scale);
    struct yasl_surface *surface = window_surface(w);
    if (surface)
        ui_surface_resize(&ui->screen, window_size(w));
    ui_surface_set_sheet(&ui->screen, ui->sheet);
    /* A theme preview repaints an open menu in its popups too. */
    for (int i = 0; i < ui->menu_popups; i++) {
        ui_surface_set_sheet(&ui->menu_popup[i].ui, ui->sheet);
        ui_surface_set_scale(&ui->menu_popup[i].ui, scale);
    }
    window_menu_relayout(w);
    if (ui->spec.restyle)
        ui->spec.restyle(w);
    window_set_background(w, ui->background);
    window_repaint_all(w);
}

void window_ui_set_style(struct window *w, const struct css_sheet *sheet,
                         uint32_t background)
{
    struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return;
    ui->sheet = sheet;
    ui->background = background;
    window_ui_restyle(w);
    window_redraw(w);
}

static void ui_resize(struct window *w, struct vector2i size)
{
    struct window_ui *ui = ui_of(w);
    window_repaint_all(w);
    ui_surface_set_scale(&ui->screen, window_ui_scale(w));
    ui_surface_resize(&ui->screen, size);
    if (ui->spec.resize)
        ui->spec.resize(w, size);
    window_redraw(w);
}

void window_ui_relayout(struct window *w)
{
    if (!window_ui_of(w))
        return;
    window_ui_restyle(w);
    struct yasl_surface *surface = window_surface(w);
    if (surface)
        ui_resize(w, window_size(w));
}

void window_ui_configure(struct window *w, struct window_geometry g)
{
    struct window_ui *ui = ui_of(w);
    float scale = (float)g.scale_120 / YASL_SCALE_UNIT;
    float old = ui_surface_scale(&ui->screen).dpi_scale;
    if (old == scale && vector2i_eq(g.size, ui_surface_viewport(&ui->screen)))
        return;
    if (old != scale) {
        /* The menu opened at a point in the old pixels. */
        if (old > 0)
            ui->menu_at = (struct vector2i){
                .x = (int)((float)ui->menu_at.x * scale / old + 0.5f),
                .y = (int)((float)ui->menu_at.y * scale / old + 0.5f),
            };
        window_ui_restyle(w);
    }
    ui_resize(w, g.size);
}

void window_ui_state_changed(struct window *w)
{
    struct window_ui *ui = ui_of(w);
    bool suspended = window_suspended(w);
    if (suspended == ui->suspended)
        return;
    ui->suspended = suspended;
    if (ui->spec.suspended)
        ui->spec.suspended(w, suspended);
}

void window_ui_closing(struct window *w)
{
    /* Popups go before their parent. Both are safe to destroy inside
     * their own callbacks: they call back no more. */
    window_menu_close(w);
    window_accessibility_stop(w);
}

void window_ui_destroy(struct window *w)
{
    struct window_ui *ui = ui_of(w);
    if (window_ui_host_last == window_host(w) && window_host(w)->count == 0)
        window_ui_host_last = NULL;
    window_accessibility_destroy(w);
    window_debug_destroy(w);
    window_menu_destroy(w);
    window_ui_root_destroy(w);
    if (ui->spec.destroy)
        ui->spec.destroy(w);
    font_instance_unref(ui->ui_font);
    ui_scene_destroy(ui->scene_scratch);
    free(ui->scene_scratch);
    ui_damage_tracker_destroy(&ui->ui_damage);
}

struct ui_surface *window_ui_surface(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    return ui ? &ui->screen : NULL;
}

struct ui_widget *window_ui_root(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    return ui ? &ui->root : NULL;
}

struct font_instance *window_ui_font(const struct window *w)
{
    const struct window_ui *ui = window_ui_of(w);
    return ui ? ui->ui_font : NULL;
}

bool window_menu_is_open(const struct window *w)
{
    const struct window_ui *ui = window_ui_of(w);
    return ui && ui_menu_is_open(&ui->menu);
}

void *window_ui_content(const struct window *w)
{
    const struct window_ui *ui = window_ui_of(w);
    return ui ? ui->spec.content : NULL;
}

struct ui_menu *window_ui_menu(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    return ui ? &ui->menu : NULL;
}

struct ui_menu_layer *window_ui_menu_layer(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    return ui ? &ui->menu_layer : NULL;
}

int window_ui_menu_popups(const struct window *w)
{
    const struct window_ui *ui = window_ui_of(w);
    return ui ? ui->menu_popups : 0;
}

void window_ui_test_route(struct window *w, struct input_event *ev)
{
    if (window_ui_of(w))
        window_ui_route(w, ev);
}
