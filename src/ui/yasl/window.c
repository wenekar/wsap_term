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

static struct window_native *native_find(struct window *w, const struct yasl_surface *s);
static void stats_collect_presented(struct window *w);

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
    if (!w || !w->surface)
        return 120;
    if (w->scale_120)
        return w->scale_120;
    double scale = yasl_surface_get_info(w->surface).scale;
    return (uint32_t)(scale * 120.0 + 0.5);
}

/* Marks one native surface as due in the next host pass. A redraw asked for
 * during a draw stays for the next pass, so no intent is lost. */
void window_native_request_draw(struct window *w, struct yasl_surface *s)
{
    struct window_native *n = native_find(w, s);
    if (n)
        n->needs_draw = true;
}

void window_redraw(struct window *w)
{
    if (!w || !w->surface || w->closed)
        return;
    if (w->unpaced) {
        /* The configuration asked for tearing, so a frame does not wait for
         * the display. */
        window_native_request_draw(w, w->surface);
        window_host_schedule(w->host);
        return;
    }
    /* The request paces the draw: the display answers it, so the window cannot
     * draw faster than the display. Requests coalesce, and a request made
     * during a draw is kept. Input is read at once; only the draw waits. */
    yasl_surface_request_frame(w->surface);
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
    if (w->surface)
        yasl_surface_set_title(w->surface, w->title ? w->title : app_name());
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
    if (w->surface)
        yasl_surface_set_background_color(w->surface, bg);
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

static struct window_native *native_find(struct window *w, const struct yasl_surface *s)
{
    for (int i = 0; i < w->native_count; i++)
        if (w->natives[i].surface == s)
            return &w->natives[i];
    return NULL;
}

bool window_native_add(struct window *w, struct yasl_surface *s)
{
    if (w->native_count == WINDOW_NATIVE_MAX)
        return false;
    w->natives[w->native_count++] = (struct window_native){ .surface = s };
    return true;
}

void window_native_remove(struct window *w, struct yasl_surface *s)
{
    struct window_native *n = native_find(w, s);
    if (!n)
        return;
    framebuffer_history_release(&n->history);
    present_watch_unref(n->watch);
    free(n->pixels);
    *n = w->natives[--w->native_count];
    w->natives[w->native_count] = (struct window_native){0};
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
    n->generation++;
    /* Each frame is fresh for now, so the renderer paints every pixel. */
    target.fresh = true;
    target.history = &n->history;
    /* The frame goes to a drawable, whether the surface holds it already or
     * asks for it while the renderer encodes. */
    n->frame_drawable = target.drawable != NULL || target.acquire != NULL;
    n->frame_submitted = false;
    struct window_frame frame = {
        .surface = n->surface,
        .generation = n->generation,
        .target = target,
    };
    window_compose_frame(w, &frame);
}

/*
 * The CPU pixels of a native frame. The buffer grows by half and never
 * shrinks. A live resize changes the size at every frame, and each frame
 * would otherwise free and allocate a whole canvas.
 *
 * The renderer asks for these pixels only when the CPU paints the frame.
 * A frame that the GPU takes needs no canvas, so a Metal window keeps no
 * canvas in the heap while the GPU draws its frames.
 */
static uint32_t *native_pixels(struct window_native *n, struct vector2i size)
{
    if (size.x <= 0 || size.y <= 0)
        return NULL;
    size_t need = (size_t)size.x * (size_t)size.y;
    if (n->pixels && need <= n->pixels_cap)
        return n->pixels;
    size_t cap = n->pixels_cap + n->pixels_cap / 2;
    if (cap < need)
        cap = need;
    /* The old pixels are stale, so do not copy them with realloc. */
    free(n->pixels);
    n->pixels = malloc(cap * sizeof(*n->pixels));
    n->pixels_cap = n->pixels ? cap : 0;
    return n->pixels;
}

struct metal_draw {
    struct window *w;
    struct window_native *n;
};

/* Asks for the next frame of one surface. The display paces the answer, even
 * when the window draws unpaced, so a failing draw cannot spin. */
static void window_retry(struct window *w, struct yasl_surface *s)
{
    if (w && !w->closed && s)
        yasl_surface_request_frame(s);
}

/* The CPU canvas of one Metal frame, when the renderer paints it there. The
 * allocation happens on this call, so a frame that the GPU takes makes no
 * allocation. */
static uint32_t *metal_cpu_pixels(void *ud, struct vector2i size)
{
    struct window_native *n = ud;
    uint32_t *pixels = native_pixels(n, size);
    (void)native_report(n, pixels != NULL, "metal frame pixels", ENOMEM);
    return pixels;
}

static void metal_compose(const struct window_metal_frame *f, void *ud)
{
    struct metal_draw *d = ud;
    /* The frame carries no canvas. render_draw() asks for one only when the
     * CPU paints the frame, so a GPU frame keeps no pixels in the heap. */
    native_compose(d->w, d->n, (struct framebuffer){
        .size = f->size,
        .drawable = f->drawable,
        .acquire = f->acquire,
        .acquire_ud = f->acquire_ud,
        .transaction = f->transaction,
        .acquire_pixels = metal_cpu_pixels,
        .acquire_pixels_ud = d->n,
    });
}

void window_native_draw(struct window *w, struct yasl_surface *s)
{
    struct window_native *n = native_find(w, s);
    if (!n || w->closed)
        return;
    bool main = s == w->surface;
    if (main) {
        frame_stats_tick(&w->stats);
        render_begin(&w->stats);
        if (!w->suspended)
            w->frame++;
    }
    n->frame_drawable = false;
    n->frame_submitted = false;
    bool attempted = true;
    if (n->metal_layer) {
        if (!n->watch)
            n->watch = present_watch_new();
        struct metal_draw d = { .w = w, .n = n };
        if (!window_metal_draw(n->metal_layer, s,
                               (struct window_metal_watch){
                                   .presents = n->watch,
                                   .sample = main,
                               },
                               metal_compose, &d)) {
            native_report(n, false, "metal drawable", 0);
            attempted = false;
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
    /* Three outcomes need the frame again: the GPU dropped it under load, no
     * drawable was free, or the draw reported no submit. A submitted frame is
     * not on screen yet, so the window never waits for it here. Ask for the
     * next frame of this surface. The request coalesces and waits for the
     * display, even when ordinary output of the window is unpaced, so a busy
     * GPU cannot spin. A closed or suspended window asks for nothing. */
    if (n->frame_drawable && !n->frame_submitted && !w->suspended)
        window_retry(w, s);
    if (main) {
        /* A pass that the layer could not start is neither submitted nor
         * dropped: there was no frame. */
        if (attempted) {
            if (n->frame_submitted)
                w->stats.frames_submitted++;
            else
                w->stats.frames_dropped++;
        }
        stats_collect_presented(w);
        render_end_and_count_frame(&w->stats);
    }
}

static void host_on_event(void *userdata, const yasl_event *ev)
{
    window_host_dispatch_event(userdata, ev);
}

static void on_refresh(yasl_surface *s)
{
    struct window *w = yasl_surface_get_userdata(s);
    if (!w || w->closed || w->suspended)
        return;
    if (s == w->surface && window_mounted(w)) {
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
    while (w->native_count)
        window_native_remove(w, w->natives[w->native_count - 1].surface);
    if (w->surface)
        yasl_surface_destroy(w->surface);
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
    window_native_remove(w, w->surface);
    yasl_surface_destroy(w->surface);
    w->surface = NULL;
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
        .userdata = w,
        .refresh = on_refresh,
        /* Vsync is on unless the configuration asks for tearing. The platform
         * permits unpaced presentation only when this is set. */
        .tearing = config->tearing,
    };
    w->surface = yasl_surface_create(&options);
    w->unpaced = config->tearing;
    void *layer = NULL;
    if (w->surface && options.type == YASL_SURFACE_NATIVE &&
        !(layer = window_metal_attach(w->surface, spec->graphics_device))) {
        fprintf(stderr, "window: no metal layer, drawing on the CPU: %s\n",
                strerror(errno));
        yasl_surface_destroy(w->surface);
        options.type = YASL_SURFACE_CPU;
        w->surface = yasl_surface_create(&options);
    }
    if (!w->surface || !window_native_add(w, w->surface))
        goto fail;
    w->natives[w->native_count - 1].metal_layer = layer;
    if (w->has_bg)
        yasl_surface_set_background_color(w->surface, w->bg);

    if (config->fullscreen && !spec->windowed)
        yasl_surface_request(w->surface, YASL_REQUEST_FULLSCREEN);

    struct yasl_surface_info info = yasl_surface_get_info(w->surface);
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
        (void)window_present(w, frame, &damage, false);
    }
}

static struct yasl_damage yasl_damage_of(struct rect2i r)
{
    return (struct yasl_damage){ .x = r.x, .y = r.y, .width = r.width, .height = r.height };
}

bool window_present(struct window *w, struct window_frame *frame,
                    const struct damage_list *damage, bool presented)
{
    if (!w || w->closed || !frame || !frame->surface)
        return false;
    struct window_native *n = native_find(w, frame->surface);
    if (frame->target.drawable || frame->target.acquire) {
        /* A drop is not an error: the GPU reports that it drew and presented
         * nothing. A true result means that the GPU submitted the frame, not
         * that the frame reached the screen. The display reports that later,
         * through the presentation handler. The window asks for the next frame
         * when the result is false. */
        if (n)
            n->frame_submitted = presented;
        return presented;
    }
    int status;
    if (damage && !damage->all && damage->n < 0) {
        /* An overflowed list presents its bounds. */
        struct yasl_damage yd = yasl_damage_of(damage->bounds);
        status = yasl_cpu_present_damage(frame->surface, &yd, 1);
    } else if (damage && !damage->all && damage->n > 0) {
        struct yasl_damage yd[DAMAGE_RECTS_MAX];
        for (int i = 0; i < damage->n; i++)
            yd[i] = yasl_damage_of(damage->rects[i]);
        status = yasl_cpu_present_damage(frame->surface, yd, damage->n);
    } else {
        status = yasl_cpu_present(frame->surface);
    }
    if (n)
        n->frame_submitted = status == 0;
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
    return w ? w->surface : NULL;
}

/*
 * Moves the reports that the display sent since the last read into the counts
 * of the window. A report arrives on a thread of the display, at any time, so a
 * count that a report would otherwise wait for another draw to collect is
 * current when it is read.
 */
static void stats_collect_presented(struct window *w)
{
    struct window_native *n = w->surface ? native_find(w, w->surface) : NULL;
    if (n && n->watch)
        w->stats.frames_presented += present_watch_take_presented(n->watch);
}

struct frame_stats *window_stats(struct window *w)
{
    if (!w)
        return NULL;
    stats_collect_presented(w);
    return &w->stats;
}

bool window_closed(const struct window *w)
{
    return !w || w->closed;
}

struct vector2i window_size(const struct window *w)
{
    if (!w || !w->surface)
        return (struct vector2i){0};
    struct yasl_surface_info info = yasl_surface_get_info(w->surface);
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
    if (w && w->surface)
        yasl_clipboard_request(w->surface, sel);
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
    return w && w->surface &&
           yasl_surface_place_under_pointer(w->surface, at.x, at.y) == 0;
}

bool window_drag_carry(struct window *w, struct window_logical_point at)
{
    return w && w->surface && yasl_drag_carry(w->surface, at.x, at.y) == 0;
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
