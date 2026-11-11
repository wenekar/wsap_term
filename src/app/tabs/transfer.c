#include <stdlib.h>

#include "app/tabs/internal.h"
#include "ui/widgets/tabbar.h"
#include "ui/yasl/window.h"

/* The model owns the drag in `model->flight`. The dragged tab stays in the
 * model for the whole drag. A carry window can hold it, or no window holds
 * it. The flight outlives the origin window and the carry window. */

enum flight_end {
    FLIGHT_TAKEN,
    FLIGHT_DROPPED,
    FLIGHT_ABANDONED,
};

/* Takes the dragged tab out of its carry window. Returns the tab when no
 * window holds it then, or 0. */
static app_tab_id flight_take(struct app_tab_model *m)
{
    struct app_tab *entry = app_tab_model_get(m, m->flight.tab);
    if (!entry)
        return 0;
    struct window *carried = entry->window;
    if (!carried)
        return entry->id;
    return app_tabs_unbind(carried,
        app_tabs_index_of(app_tabs_of(carried), entry->id));
}

/* A tab whose process ended, or a closing tab, goes instead of a landing. */
static bool spent(const struct app_tab *entry)
{
    return !app_tab_is_live(entry) || terminal_exited(entry->content);
}

static bool land(struct app_tab_model *m, struct window *w, app_tab_id id,
                 int slot)
{
    struct app_tab *entry = app_tab_model_get(m, id);
    if (!entry || spent(entry)) {
        app_tab_model_destroy(m, id);
        return true;
    }
    return w && !window_closed(w) && app_tabs_bind(w, id, slot);
}

static void land_alone(struct app_tab_model *m, const struct app_tab_flight *f,
                       app_tab_id id)
{
    struct app_tab *entry = app_tab_model_get(m, id);
    if (!entry || spent(entry)) {
        app_tab_model_destroy(m, id);
        return;
    }
    struct window *nw = f->open_window
        ? f->open_window(f->ud, id, NULL, f->size) : NULL;
    if (!nw) {
        app_tab_model_destroy(m, id);
        return;
    }
    if (window_surface(nw))
        window_place_under_pointer(nw, f->grab);
}

/* Settles the tab of an ended drag. A window that holds the tab keeps it. */
static void settle(struct app_tab_model *m, const struct app_tab_flight *f,
                   enum flight_end how)
{
    struct app_tab *entry = app_tab_model_get(m, f->tab);
    if (!entry || entry->window)
        return;
    switch (how) {
    case FLIGHT_TAKEN:
        break;
    case FLIGHT_ABANDONED:
        app_tab_model_destroy(m, f->tab);
        break;
    case FLIGHT_DROPPED:
        land_alone(m, f, f->tab);
        break;
    }
}

static void show_incoming(struct window *w, bool active, struct vector2i pos)
{
    struct app_tabs *tabs = app_tabs_of(w);
    if (tabs && window_surface(w))
        ui_tabbar_set_external_drag(&tabs->tabbar, active, pos);
}

static void set_over(struct app_tab_flight *f, struct window *w)
{
    struct window *old = f->over;
    if (old == w)
        return;
    f->over = w;
    if (old && !window_closed(old))
        show_incoming(old, false, (struct vector2i){0});
}

static uint32_t *drag_image_pixels(struct window *w, int index,
                                   struct vector2i *out_size,
                                   struct vector2i *out_hotspot)
{
    struct app_tabs *tabs = app_tabs_of(w);
    const struct ui_image *thumb =
        app_tabs_preview_capture(tabs, index, true)
            ? &tabs->preview_image : NULL;
    struct ui_scene scene = {0};
    struct vector2i size, hotspot;
    if (!ui_tabbar_build_drag_scene(&tabs->tabbar, index, thumb, &scene,
                                    &size, &hotspot))
        return NULL;
    uint32_t *pixels = NULL;
    if (size.x > 0 && size.y > 0 &&
        (size_t)size.x <= SIZE_MAX / (size_t)size.y / sizeof(*pixels))
        pixels = calloc((size_t)size.x * (size_t)size.y, sizeof(*pixels));
    if (pixels && (!tabs->spec.scene_paint || !tabs->spec.scene_paint(
            tabs->spec.scene_paint_ud, window_ui_font(w), pixels, size,
            &scene))) {
        free(pixels);
        pixels = NULL;
    }
    ui_scene_destroy(&scene);
    *out_size = size;
    *out_hotspot = hotspot;
    return pixels;
}

static bool can_carry(const struct window *w)
{
    return window_surface(w) && (yasl_get_features() & YASL_FEATURE_DRAG_CARRY);
}

static bool start_drag(struct app_tab_model *m, const struct app_tab_flight *f,
                       struct window *origin, const struct yasl_drag_image *image)
{
    if (yasl_drag_start(window_surface(origin), image) != 0)
        return false;
    m->flight = *f;
    m->flight.running = true;
    window_host_hold(m->flight.host);
    return true;
}

/* The model holds the tab while the drag runs. Open the new window from
 * the flight, so a failure leaves the tab with the flight. */
static void carry(struct window *w, struct app_tab_model *m, int index)
{
    app_tab_id id = app_tabs_unbind(w, index);
    if (!id)
        return;
    struct app_tab_flight *f = &m->flight;
    f->tab = id;
    struct window *nw = f->open_window
        ? f->open_window(f->ud, id, NULL, f->size) : NULL;
    if (nw && window_drag_carry(nw,
            (struct window_logical_point){ .x = f->grab.x }))
        return;
    if (nw)
        (void)app_tabs_unbind(nw, app_tabs_index_of(app_tabs_of(nw), id));
}

bool app_tabs_drag_begin(struct window *w, int index, struct vector2i grab)
{
    struct app_tabs *tabs = app_tabs_of(w);
    struct app_tab_model *m = tabs ? tabs->spec.model : NULL;
    if (!tabs || !window_surface(w) || m->flight.running || index < 0 ||
        index >= tabs->count || app_tabs_live_count(tabs) < 2)
        return false;
    struct app_tab_flight f = {
        .size = window_size(w),
        .grab = window_logical(w, grab),
        .host = window_host(w),
        .open_window = tabs->spec.open_window,
        .ud = tabs->spec.ud,
    };
    bool carrying = can_carry(w);
    struct vector2i size = {0}, hotspot = {0};
    uint32_t *pixels = carrying ? NULL : drag_image_pixels(w, index, &size, &hotspot);
    struct yasl_drag_image image = {
        .pixels = pixels,
        .pixel_width = size.x,
        .pixel_height = size.y,
        .hotspot_x = hotspot.x,
        .hotspot_y = hotspot.y,
    };
    bool started = (carrying || pixels) &&
        start_drag(m, &f, w, carrying ? NULL : &image);
    free(pixels);
    if (!started)
        return false;
    ui_tab_preview_hide(&tabs->preview);
    if (carrying)
        carry(w, m, index);
    else
        m->flight.tab = app_tabs_unbind(w, index);
    return true;
}

static bool tab_drop(struct window *w, struct app_tab_model *m,
                     struct vector2i pos)
{
    struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs || !window_surface(w))
        return false;
    show_incoming(w, true, pos);
    int slot = ui_tabbar_external_slot(&tabs->tabbar);
    if (slot < 0) {
        show_incoming(w, false, pos);
        return false;
    }
    app_tab_id id = flight_take(m);
    if (!id) {
        show_incoming(w, false, pos);
        return false;
    }
    /* On failure the flight keeps the tab, and no window holds it. */
    bool landed = land(m, w, id, slot);
    show_incoming(w, false, pos);
    return landed;
}

void app_tabs_drag(struct window *w, enum yasl_drop_action event,
                   struct vector2i pos, void *ud)
{
    struct app_tab_model *m = ud;
    if (!m || !m->flight.running || !w || window_closed(w))
        return;
    struct app_tab_flight *f = &m->flight;
    switch (event) {
    case YASL_DROP_OVER:
        set_over(f, w);
        show_incoming(w, true, pos);
        break;
    case YASL_DROP_LEAVE:
        if (f->over == w)
            set_over(f, NULL);
        break;
    case YASL_DROP_RELEASE:
    case YASL_DROP_CANCEL:
        /* A cancelled drag lands like a drop on the desktop. */
        if (event == YASL_DROP_RELEASE && !f->taken)
            f->taken = tab_drop(w, m, pos);
        set_over(f, NULL);
        enum flight_end end = f->taken ? FLIGHT_TAKEN
            : window_host_destroying(f->host) ? FLIGHT_ABANDONED
            : FLIGHT_DROPPED;
        struct app_tab_flight ended = *f;
        m->flight = (struct app_tab_flight){0};
        settle(m, &ended, end);
        window_host_release(ended.host);
        break;
    }
}

void app_tabs_drop_hover(struct window *w, bool over, void *ud)
{
    (void)ud;
    struct terminal *content = app_tabs_active(w);
    if (content)
        terminal_set_drop_hover(content, over);
}

void app_tabs_drag_forget(struct app_tab_model *m, struct window *w)
{
    if (m && m->flight.running && m->flight.over == w)
        m->flight.over = NULL;
}

const struct window_drag_target APP_TABS_DRAG_TARGET = {
    .drag = app_tabs_drag,
    .drop_hover = app_tabs_drop_hover,
};
