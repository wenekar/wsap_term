#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font/font.h"
#include "loop/latency_probe.h"
#include "yasl/yasl.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/metal.h"
#include "ui/yasl/native_input.h"
#include "util/name.h"

struct vector2i window_default_size(const struct window_config *config)
{
    const struct font_metrics *m = font_instance_metrics(font_instance_default());
    return (struct vector2i){
        .x = config->size.x ? config->size.x : WINDOW_DEFAULT_COLS * m->cell_w,
        .y = config->size.y ? config->size.y : WINDOW_DEFAULT_ROWS * m->cell_h,
    };
}

uint32_t window_scale_120(const struct window *w)
{
    if (!w || !w->native.surface)
        return 120;
    if (w->scale_120)
        return w->scale_120;
    double scale = yasl_surface_get_info(w->native.surface).scale;
    return (uint32_t)(scale * 120.0 + 0.5);
}

void window_redraw(struct window *w)
{
    if (!w || !w->native.surface || w->closed)
        return;
    if (w->drawing) {
        yasl_surface_request_frame(w->native.surface);
        return;
    }
    w->needs_draw = true;
    window_host_schedule(w->host);
}

uint64_t window_frame_time_ms(struct window *w)
{
    (void)w;
    return yasl_time_ns() / 1000000;
}

void window_set_title(struct window *w, const char *title)
{
    char *copy = NULL;
    if (title && !(copy = strdup(title)))
        return;
    free(w->title);
    w->title = copy;
    if (w->native.surface)
        yasl_surface_set_title(w->native.surface, w->title ? w->title : app_name());
}

const char *window_title(const struct window *w)
{
    return w ? w->title : NULL;
}

void window_set_background(struct window *w, uint32_t bg)
{
    if (!w)
        return;
    if (w->has_bg && w->bg == bg)
        return;
    w->bg = bg;
    w->has_bg = true;
    if (w->native.surface)
        yasl_surface_set_background_color(w->native.surface, bg);
}

uint32_t window_background(const struct window *w)
{
    return w ? w->bg : 0;
}

static bool fifo_push(struct window_fifo *f, const struct input_event *ev)
{
    if (f->len == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 256;
        struct input_event *grown = malloc(cap * sizeof(*grown));
        if (!grown)
            return false;
        for (size_t i = 0; i < f->len; i++)
            grown[i] = f->ev[(f->head + i) % f->cap];
        free(f->ev);
        f->ev = grown;
        f->head = 0;
        f->cap = cap;
    }
    f->ev[(f->head + f->len++) % f->cap] = *ev;
    return true;
}

static bool fifo_pop(struct window_fifo *f, struct input_event *out)
{
    if (!f->len)
        return false;
    *out = f->ev[f->head];
    f->head = (f->head + 1) % f->cap;
    f->len--;
    return true;
}

bool window_inject_input(struct window *w, const struct input_event *ev)
{
    if (fifo_push(&w->inject, ev)) {
        window_host_schedule(w->host);
        return true;
    }
    struct input_event owned = *ev;
    input_event_release(&owned);
    return false;
}

bool window_inject_pop(struct window *w, struct input_event *ev)
{
    return fifo_pop(&w->inject, ev);
}

void window_native_destroy(struct window_native *native)
{
    framebuffer_history_release(&native->history);
    free(native->pixels);
    yasl_surface_destroy(native->surface);
    *native = (struct window_native){0};
}

static bool native_report(struct window_native *n, bool ok, const char *what, int err)
{
    if (n && !ok && !n->failing)
        fprintf(stderr, "window: %s: %s\n", what, err ? strerror(err) : "failed");
    if (n)
        n->failing = !ok;
    return ok;
}

static void native_compose(struct window *w, struct window_native *n,
                           struct framebuffer target)
{
    n->damage = (struct damage_list){0};
    n->generation++;
    target.stale = true;
    target.damage = &n->damage;
    target.history = &n->history;
    struct window_frame frame = {
        .surface = n->surface,
        .generation = n->generation,
        .target = target,
    };
    window_compose_frame(w, &frame);
}

static uint32_t *native_pixels(struct window_native *n, struct vector2i size)
{
    if (size.x <= 0 || size.y <= 0)
        return NULL;
    if (n->pixels && vector2i_eq(n->pixels_size, size))
        return n->pixels;
    free(n->pixels);
    n->pixels = malloc((size_t)size.x * (size_t)size.y * sizeof(*n->pixels));
    n->pixels_size = n->pixels ? size : (struct vector2i){0};
    return n->pixels;
}

struct metal_draw {
    struct window *w;
    struct window_native *n;
};

static void metal_compose(const struct window_metal_frame *f, void *ud)
{
    struct metal_draw *d = ud;
    uint32_t *pixels = native_pixels(d->n, f->size);
    if (!native_report(d->n, pixels != NULL, "metal frame pixels", ENOMEM))
        return;
    native_compose(d->w, d->n, (struct framebuffer){
        .pixels = pixels,
        .size = f->size,
        .drawable = f->drawable,
        .transaction = f->transaction,
    });
}

void window_native_draw(struct window *w, struct yasl_surface *s)
{
    struct window_native *n = s ? yasl_surface_get_userdata(s) : NULL;
    if (!n || n->window != w || w->closed)
        return;
    bool main = s == w->native.surface;
    if (main) {
        frame_stats_tick(&w->stats);
        render_begin(&w->stats);
        if (!w->suspended)
            w->frame++;
    }
    w->drawing = true;
    if (n->metal_layer) {
        struct metal_draw d = { .w = w, .n = n };
        if (!window_metal_draw(n->metal_layer, s, metal_compose, &d)) {
            native_report(n, false, "metal drawable", 0);
            window_redraw(w);
        }
    } else {
        struct vector2i size = {0};
        uint32_t *pixels = yasl_cpu_pixels(s, &size.x, &size.y);
        native_report(n, pixels != NULL, "yasl_cpu_pixels", errno);
        native_compose(w, n, (struct framebuffer){
            .pixels = pixels,
            .size = size,
        });
    }
    w->drawing = false;
    if (main)
        render_end_and_count_frame(&w->stats);
}

static void host_on_event(void *userdata, const yasl_event *ev)
{
    window_host_dispatch_event(userdata, ev);
}

static void on_refresh(yasl_surface *s)
{
    struct window_native *native = yasl_surface_get_userdata(s);
    struct window *w = native ? native->window : NULL;
    if (!w || w->closed || w->suspended)
        return;
    if (s == w->native.surface && window_mounted(w)) {
        struct yasl_surface_info info = yasl_surface_get_info(s);
        struct window_geometry g = {
            .size = { .x = info.pixel_width, .y = info.pixel_height },
            .scale_120 = (uint32_t)(info.scale * 120.0 + 0.5),
        };
        if (!vector2i_eq(g.size, ui_surface_viewport(&w->ui.screen)) ||
            g.scale_120 != w->scale_120) {
            w->scale_120 = g.scale_120;
            window_ui_configure(w, g);
        }
    }
    window_native_draw(w, s);
}

struct window *window_create(struct window_host *host, const struct window_spec *spec)
{
    struct window *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    if (spec->title && !(w->title = strdup(spec->title))) {
        free(w);
        return NULL;
    }
    w->compose = spec->compose;
    w->compose_ud = spec->compose_ud;
    if (!window_host_add(host, w)) {
        free(w->title);
        free(w);
        return NULL;
    }
    w->host = host;
    w->native.window = w;
    w->stats = (struct frame_stats){
        .started_ns = yasl_time_ns(),
        .frame_min_ms = 1e18,
    };
    return w;
}

void window_destroy(struct window *w)
{
    struct window_host *host = w->host;
    if (window_mounted(w))
        window_ui_destroy(w);
    window_host_remove(host, w);
    window_native_destroy(&w->native);
    struct input_event ev;
    while (fifo_pop(&w->inject, &ev))
        input_event_release(&ev);
    input_queue_reset(&w->input);
    free(w->inject.ev);
    free(w->ime_preedit);
    free(w->title);
    free(w);
}

void window_close(struct window *w)
{
    if (!w || w->closed)
        return;
    struct window_host *host = w->host;
    w->closed = true;
    window_host_remove(host, w);
    w->next_closing = host->closing;
    host->closing = w;
    window_host_schedule(host);
    window_host_report_empty(host, w);
    if (window_mounted(w))
        window_ui_closing(w);
    window_native_destroy(&w->native);
}

struct window *window_open(struct window_host *host, const struct window_spec *spec)
{
    const struct window_config *config = host->config;
    struct window *w = window_create(host, spec);
    if (!w)
        return NULL;

    struct vector2i size = window_default_size(config);
    if (spec->size.x > 0)
        size.x = spec->size.x;
    if (spec->size.y > 0)
        size.y = spec->size.y;

    int init = yasl_init(&(struct yasl_options){
        .app_id = app_name(),
        .loop = host->loop,
        .on_event = host_on_event,
        .userdata = host,
    });
    if (init != 0 && init != -EALREADY)
        goto fail;
    struct yasl_surface_options options = {
        .title = w->title ? w->title : app_name(),
        .width = size.x,
        .height = size.y,
        .type = spec->graphics_device ? YASL_SURFACE_NATIVE : YASL_SURFACE_CPU,
        .userdata = &w->native,
        .refresh = on_refresh,
        .tearing = true,
    };
    w->native.surface = yasl_surface_create(&options);
    void *layer = NULL;
    if (w->native.surface && options.type == YASL_SURFACE_NATIVE &&
        !(layer = window_metal_attach(w->native.surface, spec->graphics_device))) {
        fprintf(stderr, "window: no metal layer, drawing on the CPU: %s\n",
                strerror(errno));
        yasl_surface_destroy(w->native.surface);
        options.type = YASL_SURFACE_CPU;
        w->native.surface = yasl_surface_create(&options);
    }
    if (!w->native.surface)
        goto fail;
    w->native.metal_layer = layer;
    if (w->has_bg)
        yasl_surface_set_background_color(w->native.surface, w->bg);

    if (config->fullscreen && !spec->windowed)
        yasl_surface_request(w->native.surface, YASL_REQUEST_FULLSCREEN);

    struct yasl_surface_info info = yasl_surface_get_info(w->native.surface);
    w->scale_120 = (uint32_t)(info.scale * 120.0 + 0.5);

    w->suspended = (info.state & YASL_STATE_SUSPENDED) != 0;
    w->focused = (info.state & YASL_STATE_ACTIVATED) != 0;
    if (w->focused)
        latency_probe_set_focus(true);
    latency_probe_fullscreen((info.state & YASL_STATE_FULLSCREEN) != 0);

    window_redraw(w);
    return w;

fail:
    window_destroy(w);
    return NULL;
}

void window_compose_frame(struct window *w, struct window_frame *frame)
{
    if (w && !w->closed && w->compose)
        w->compose(w, frame, w->compose_ud);
    else if (w && frame) {
        struct damage_list damage = {0};
        damage_add_all(&damage);
        (void)window_present(w, frame, &damage);
    }
}

bool window_present(struct window *w, struct window_frame *frame,
                    const struct damage_list *damage)
{
    if (!w || w->closed || !frame || !frame->surface)
        return false;
    struct window_native *n = yasl_surface_get_userdata(frame->surface);
    if (!n || n->window != w)
        return false;
    if (frame->target.drawable)
        return native_report(n, frame->target.presented, "metal present", 0);
    int status;
    if (damage && !damage->all && damage->n > 0) {
        struct yasl_damage yd[DAMAGE_RECTS_MAX];
        for (int i = 0; i < damage->n; i++) {
            yd[i] = (struct yasl_damage){
                .x = damage->rects[i].x,
                .y = damage->rects[i].y,
                .width = damage->rects[i].width,
                .height = damage->rects[i].height,
            };
        }
        status = yasl_cpu_present_damage(frame->surface, yd, damage->n);
    } else {
        status = yasl_cpu_present(frame->surface);
    }
    return native_report(n, status == 0, "yasl_cpu_present", -status);
}

struct window_host *window_host(const struct window *w)
{
    return w->host;
}

uint32_t window_double_click_ms(const struct window *w)
{
    return w->double_click_ms ? w->double_click_ms : (uint32_t)yasl_get_double_click_ms();
}

struct yasl_surface *window_surface(const struct window *w)
{
    return w ? w->native.surface : NULL;
}

struct frame_stats *window_stats(struct window *w)
{
    return w ? &w->stats : NULL;
}

bool window_closed(const struct window *w)
{
    return !w || w->closed;
}

struct vector2i window_size(const struct window *w)
{
    if (!w || !w->native.surface)
        return (struct vector2i){0};
    struct yasl_surface_info info = yasl_surface_get_info(w->native.surface);
    return (struct vector2i){ .x = info.pixel_width, .y = info.pixel_height };
}

bool window_suspended(const struct window *w)
{
    return w->suspended;
}

uint32_t window_frame_count(const struct window *w)
{
    return w->frame;
}

struct window_pointer window_pointer(const struct window *w)
{
    return w->pointer;
}

void window_repaint_all(struct window *w)
{
    if (w)
        w->repaint = true;
}

bool window_take_repaint(struct window *w)
{
    bool repaint = w && w->repaint;
    if (w)
        w->repaint = false;
    return repaint;
}

const struct input_keyboard_state *window_keyboard(const struct window *w)
{
    return &w->keyboard;
}

void window_copy(struct window *w, enum yasl_selection sel, const char *text)
{
    (void)w;
    yasl_clipboard_set(sel, text);
}

void window_paste(struct window *w, enum yasl_selection sel)
{
    if (w && w->native.surface)
        yasl_clipboard_request(w->native.surface, sel);
}

int window_modifiers(const struct window *w)
{
    return w ? w->modifiers : 0;
}

struct window_logical_point window_logical(const struct window *w,
                                           struct vector2i device)
{
    double scale = window_scale_120(w) / 120.0;
    return (struct window_logical_point){ .x = device.x / scale, .y = device.y / scale };
}

bool window_place_under_pointer(struct window *w, struct window_logical_point at)
{
    return w && w->native.surface &&
           yasl_surface_place_under_pointer(w->native.surface, at.x, at.y) == 0;
}

bool window_drag_carry(struct window *w, struct window_logical_point at)
{
    return w && w->native.surface && yasl_drag_carry(w->native.surface, at.x, at.y) == 0;
}

void window_set_drag_target(struct window *w, const struct window_drag_target *t,
                            void *ud)
{
    w->drag_target = t;
    w->drag_ud = ud;
}

const char *window_ime_preedit(const struct window *w)
{
    return w ? w->ime_preedit : NULL;
}

int window_ime_preedit_begin(const struct window *w)
{
    return w ? w->ime_begin : 0;
}

int window_ime_preedit_end(const struct window *w)
{
    return w ? w->ime_end : 0;
}
