#include "terminal/internal.h"

#include <stdlib.h>

/* Moving tabs between windows. */

struct tab_flight {
    struct tab *tab;
    struct window *carried;
    struct window *origin;
    struct window *over;
    bool taken;
    int origin_index;
    struct vector2i size;
    /* The grab point in logical units, for yasl. */
    struct window_logical_point grab;
    struct window_host *host;
    struct window *(*open_window)(void *ud, struct tab *tab, const char *cwd,
                                  struct vector2i size);
    void *ud;
};

static struct tab_flight *running;

enum flight_end {
    FLIGHT_TAKEN,
    FLIGHT_DROPPED,
    FLIGHT_CANCELLED,
    FLIGHT_ABANDONED,
};

static struct tab *flight_take(struct tab_flight *f)
{
    struct tab *t = f->tab;
    f->tab = NULL;
    if (!t && f->carried && terminal_tab_count(f->carried) > 0)
        t = terminal_take_tab(f->carried, 0);
    f->carried = NULL;
    return t;
}

static bool land(struct window *w, struct tab *t, int slot)
{
    if (t->exited) {
        tab_destroy(t);
        return true;
    }
    return w && !window_closed(w) && terminal_attach(w, t, slot);
}

static void land_alone(struct tab_flight *f, struct tab *t)
{
    if (t->exited) {
        tab_destroy(t);
        return;
    }
    struct window *nw = f->open_window
        ? f->open_window(f->ud, t, NULL, f->size) : NULL;
    if (!nw) {
        tab_destroy(t);
        return;
    }
    if (window_surface(nw))
        window_place_under_pointer(nw, f->grab);
}

static void settle(struct tab_flight *f, enum flight_end how)
{
    switch (how) {
    case FLIGHT_TAKEN:
        break;
    case FLIGHT_ABANDONED:
        if (f->tab)
            tab_destroy(f->tab);
        break;
    case FLIGHT_DROPPED:
        if (f->tab)
            land_alone(f, f->tab);
        break;
    case FLIGHT_CANCELLED: {
        struct tab *t = flight_take(f);
        if (t && !land(f->origin, t, f->origin_index))
            land_alone(f, t);
        break;
    }
    }
}

static void show_incoming(struct window *w, bool active, struct vector2i pos)
{
    if (window_surface(w))
        ui_tabbar_set_external_drag(&terminal_of(w)->tabbar, active, pos);
}

static void set_over(struct tab_flight *f, struct window *w)
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
    struct terminal *tw = terminal_of(w);
    const struct ui_image *thumb = terminal_tab_preview_capture(w, index, true)
        ? &tw->preview_image : NULL;
    struct ui_scene scene = {0};
    struct vector2i size, hotspot;
    if (!ui_tabbar_build_drag_scene(&tw->tabbar, index, thumb, &scene,
                                    &size, &hotspot))
        return NULL;
    uint32_t *pixels = NULL;
    if (size.x > 0 && size.y > 0 &&
        (size_t)size.x <= SIZE_MAX / (size_t)size.y / sizeof(*pixels))
        pixels = calloc((size_t)size.x * (size_t)size.y, sizeof(*pixels));
    if (pixels && (!tw->scene_paint || !tw->scene_paint(
            tw->scene_paint_ud, window_ui_font(w), pixels, size, &scene))) {
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

static bool start_drag(struct tab_flight *f, struct window *origin,
                       const struct yasl_drag_image *image)
{
    if (yasl_drag_start(window_surface(origin), image) != 0)
        return false;
    running = f;
    window_host_hold(f->host);
    return true;
}

static void carry(struct window *w, struct tab_flight *f, int index)
{
    struct window *nw = terminal_detach_tab(w, index);
    if (nw && window_drag_carry(nw, (struct window_logical_point){ .x = f->grab.x })) {
        f->carried = nw;
        return;
    }
    f->tab = nw ? terminal_take_tab(nw, 0) : terminal_take_tab(w, index);
}

bool terminal_tab_drag_begin(struct window *w, int index, struct vector2i grab)
{
    struct terminal *tw = terminal_of(w);
    if (!window_surface(w) || running || index < 0 ||
        index >= tw->tab_count || tw->tab_count < 2)
        return false;
    struct tab_flight *f = calloc(1, sizeof(*f));
    if (!f)
        return false;
    *f = (struct tab_flight){
        .origin = w,
        .origin_index = index,
        .size = window_size(w),
        .grab = window_logical(w, grab),
        .host = window_host(w),
        .open_window = tw->open_window,
        .ud = tw->ud,
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
    bool started = (carrying || pixels) && start_drag(f, w, carrying ? NULL : &image);
    free(pixels);
    if (!started) {
        free(f);
        return false;
    }
    ui_tab_preview_hide(&tw->tab_preview);
    if (carrying)
        carry(w, f, index);
    else
        f->tab = terminal_take_tab(w, index);
    return true;
}

static bool tab_drop(struct window *w, struct tab_flight *f, struct vector2i pos)
{
    if (!window_surface(w))
        return false;
    show_incoming(w, true, pos);
    int slot = ui_tabbar_external_slot(&terminal_of(w)->tabbar);
    if (slot < 0) {
        show_incoming(w, false, pos);
        return false;
    }
    struct tab *t = flight_take(f);
    if (!t) {
        show_incoming(w, false, pos);
        return false;
    }
    bool landed = land(w, t, slot);
    show_incoming(w, false, pos);
    if (landed)
        return true;
    f->tab = t;
    return false;
}

void terminal_tab_drag(struct window *w, enum yasl_drop_action ev,
                       struct vector2i pos, void *ud)
{
    (void)ud;
    struct tab_flight *f = running;
    if (!f || window_closed(w))
        return;
    switch (ev) {
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
        /* CANCEL comes to the origin when the drop landed on no window. */
        if (ev == YASL_DROP_RELEASE && !f->taken)
            f->taken = tab_drop(w, f, pos);
        set_over(f, NULL);
        enum flight_end end = f->taken ? FLIGHT_TAKEN
            : window_host_destroying(f->host) ? FLIGHT_ABANDONED
            : FLIGHT_DROPPED;
        running = NULL;
        settle(f, end);
        struct window_host *host = f->host;
        free(f);
        window_host_release(host);
        break;
    }
}

void terminal_tab_flight_forget(struct window *w)
{
    struct tab_flight *f = running;
    if (!f)
        return;
    if (f->origin == w)
        f->origin = NULL;
    if (f->carried == w)
        f->carried = NULL;
    if (f->over == w)
        f->over = NULL;
}
