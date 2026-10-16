#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loop/latency_probe.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "util/time.h"
#include "yasl/yasl.h"

void window_host_init(struct window_host *host, struct yasl_loop *loop,
                      const struct window_config *config,
                      const struct window_host_hooks *hooks)
{
    *host = (struct window_host){
        .loop = loop,
        .config = config,
        .hooks = hooks ? *hooks : (struct window_host_hooks){0},
    };
}

void window_host_destroy(struct window_host *host)
{
    host->destroying = true;
    while (host->count > 0)
        window_close(host->windows[host->count - 1]);
    window_host_reap(host);
    yasl_loop_del_timer(host->loop, host->service);
    yasl_loop_del_timer(host->loop, host->wake);
    int status = yasl_shutdown();
    if (status < 0)
        fprintf(stderr, "window host: yasl_shutdown: %s\n", strerror(-status));
    free(host->windows);
    *host = (struct window_host){0};
}

bool window_host_add(struct window_host *host, struct window *w)
{
    if (host->count == host->cap) {
        int cap = host->cap ? host->cap * 2 : 4;
        struct window **ws = realloc(host->windows, (size_t)cap * sizeof(*ws));
        if (!ws)
            return false;
        host->windows = ws;
        host->cap = cap;
    }
    host->windows[host->count++] = w;
    if (!host->primary)
        host->primary = w;
    return true;
}

void window_host_remove(struct window_host *host, struct window *w)
{
    for (int i = 0; i < host->count; i++) {
        if (host->windows[i] == w) {
            memmove(&host->windows[i], &host->windows[i + 1],
                    (size_t)(host->count - i - 1) * sizeof(*host->windows));
            host->windows[--host->count] = NULL;
            break;
        }
    }
    if (host->primary == w)
        host->primary = host->count ? host->windows[0] : NULL;
}

void window_host_reap(struct window_host *host)
{
    while (host->closing) {
        struct window *w = host->closing;
        host->closing = w->next_closing;
        window_destroy(w);
    }
}

static void draw_windows(struct window_host *host)
{
    for (int i = 0; i < host->count; i++) {
        struct window *w = host->windows[i];
        if (!w->surface || w->closed || w->suspended || !w->needs_draw)
            continue;
        w->needs_draw = false;
        window_native_draw(w, w->surface);
        for (int j = 1; j < w->native_count && !w->closed; j++)
            if (w->natives[j].surface)
                window_native_draw(w, w->natives[j].surface);
        if (i >= host->count || host->windows[i] != w)
            i = -1;
    }
}

static uint64_t host_service(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct window_host *host = ud;
    host->service = 0;
    window_host_reap(host);
    for (int i = host->count - 1; i >= 0; i--) {
        struct window *w = host->windows[i];
        if (w->surface && !w->closed)
            window_drain_input(w);
    }
    draw_windows(host);
    return 0;
}

void window_host_schedule(struct window_host *host)
{
    if (host->destroying || host->service)
        return;
    host->service = yasl_loop_add_timer(host->loop, 0, host_service, host);
}

static void arm_wake(struct window_host *host);

static uint64_t host_wake(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct window_host *host = ud;
    host->wake = 0;
    host->wake_at_ms = 0;
    uint64_t now_ms = yasl_time_ns() / 1000000;
    for (int i = 0; i < host->count; i++) {
        struct window *w = host->windows[i];
        if (w->wake_ms && w->wake_ms <= now_ms) {
            w->wake_ms = 0;
            window_redraw(w);
        }
    }
    arm_wake(host);
    return 0;
}

static void arm_wake(struct window_host *host)
{
    uint64_t next = 0;
    for (int i = 0; i < host->count; i++) {
        uint64_t at = host->windows[i]->wake_ms;
        if (at && (!next || at < next))
            next = at;
    }
    if (host->wake && next == host->wake_at_ms)
        return;
    yasl_loop_del_timer(host->loop, host->wake);
    host->wake = 0;
    host->wake_at_ms = 0;
    if (!next || host->destroying)
        return;
    uint64_t now_ms = yasl_time_ns() / 1000000;
    host->wake = yasl_loop_add_timer(host->loop, next > now_ms ? MS_TO_NS(next - now_ms) : 0,
                                     host_wake, host);
    if (host->wake)
        host->wake_at_ms = next;
}

void window_wake_at(struct window *w, uint64_t at_ms)
{
    w->wake_ms = at_ms;
    arm_wake(w->host);
}

bool window_host_command(struct window *w, enum input_shortcut action)
{
    struct window_host *host = w->host;
    return host->hooks.command && host->hooks.command(host->hooks.ud, w, action);
}

void window_host_hold(struct window_host *host)
{
    host->holds++;
}

void window_host_release(struct window_host *host)
{
    if (host->holds == 0)
        return;
    host->holds--;
    window_host_report_empty(host, NULL);
}

bool window_host_destroying(const struct window_host *host)
{
    return host->destroying;
}

static struct window *window_of_surface(struct window_host *host,
                                        const struct yasl_surface *s, bool *main)
{
    for (int i = 0; s && i < host->count; i++) {
        struct window *w = host->windows[i];
        for (int j = 0; j < w->native_count; j++) {
            if (w->natives[j].surface == s) {
                *main = s == w->surface;
                return w;
            }
        }
    }
    return NULL;
}

static void window_geometry_event(struct window *w, const struct yasl_event *ev)
{
    uint32_t scale_120 = (uint32_t)(ev->window.scale * 120.0 + 0.5);
    w->scale_120 = scale_120;
    struct window_geometry g = {
        .size = { .x = ev->window.pixel_width, .y = ev->window.pixel_height },
        .scale_120 = scale_120,
    };
    latency_probe_fullscreen((ev->window.state & YASL_STATE_FULLSCREEN) != 0);
    bool focused = (ev->window.state & YASL_STATE_ACTIVATED) != 0;
    if (focused != w->focused) {
        latency_probe_set_focus(focused);
        w->focused = focused;
        if (!focused)
            input_keyboard_reset(&w->keyboard);
        else
            w->double_click_ms = (uint32_t)yasl_get_double_click_ms();
        input_queue_add(&w->input, &(struct input_event){
            .kind = INPUT_FOCUS, .focus = { .focused = focused },
        });
    }
    bool suspended = (ev->window.state & YASL_STATE_SUSPENDED) != 0;
    if (suspended != w->suspended) {
        w->suspended = suspended;
        if (!w->suspended)
            window_redraw(w);
        if (window_mounted(w))
            window_ui_state_changed(w);
    }
    if (window_mounted(w)) {
        window_ui_configure(w, g);
    } else {
        window_repaint_all(w);
        window_redraw(w);
    }
}

static void dispatch_event(struct window_host *host, const struct yasl_event *ev)
{
    switch (ev->type) {
    case YASL_EVENT_QUIT:
        if (host->hooks.quit)
            host->hooks.quit(host->hooks.ud);
        return;
    case YASL_EVENT_REOPEN:
        if (host->hooks.reopen)
            host->hooks.reopen(host->hooks.ud);
        return;
    case YASL_EVENT_DISPLAY_LOST:
        if (ev->action == YASL_LOST_FINAL && host->hooks.quit)
            host->hooks.quit(host->hooks.ud);
        return;
    case YASL_EVENT_DISPLAY_RESTORED:
        for (int i = 0; i < host->count; i++) {
            window_repaint_all(host->windows[i]);
            window_redraw(host->windows[i]);
        }
        return;
    default:
        break;
    }

    bool main = false;
    struct window *w = window_of_surface(host, ev->surface, &main);
    if (!w)
        return;
    switch (ev->type) {
    case YASL_EVENT_CLOSE:
        if (main)
            window_close(w);
        else
            window_menu_close(w);
        break;
    case YASL_EVENT_WINDOW:
        if (main)
            window_geometry_event(w, ev);
        break;
    case YASL_EVENT_REFRESH:
        w->needs_draw = true;
        break;
    default:
        window_native_input(w, ev);
        break;
    }
}

void window_host_dispatch_event(struct window_host *host, const struct yasl_event *ev)
{
    dispatch_event(host, ev);
    window_host_schedule(host);
}

void window_host_report_empty(struct window_host *host, struct window *last)
{
    if (!host->destroying && host->count == 0 && host->holds == 0 && host->hooks.empty)
        host->hooks.empty(host->hooks.ud, last);
}
