/* Tests of the Wayland monitor queries: the wl_output state and the event that
 * reports a change. They call the protocol callbacks with no compositor. */
#define _GNU_SOURCE
#include "platform/wayland/internal.h"
#include <assert.h>

#include "../../src/platform/wayland/shell.c"

static struct wl_conn_output *add_output(struct wayland_display *c, uint32_t name, int32_t scale)
{
    struct wl_conn_output *o = calloc(1, sizeof(*o));
    assert(o);
    o->conn = c;
    o->name = name;
    o->scale = o->pending_scale = scale;
    struct wl_conn_output **tail = &c->outputs;
    while (*tail) tail = &(*tail)->next;
    *tail = o;
    return o;
}

static int poll_monitors(struct yasl_private_display *display)
{
    int count = 0;
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(display, &event)) {
        assert(event.type == YASL_PRIVATE_EVENT_MONITORS);
        count++;
    }
    return count;
}

int main(void)
{
    struct yasl_private_backend_ops ops = {0};
    struct wayland_display display = {.base = {.ops = &ops}};
    assert(yasl_private_events_open(&display.base));

    /* No output is a clean answer: a compositor with no monitor reports none. */
    struct yasl_monitor_info info[4];
    memset(info, 0, sizeof(info));
    assert(conn_display_monitors(&display.base, info, 4) == 0);

    struct wl_conn_output *o = add_output(&display, 1, 2);
    output_geometry(o, NULL, 1920, 100, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                    "make", "model", WL_OUTPUT_TRANSFORM_90);
    output_mode(o, NULL, WL_OUTPUT_MODE_CURRENT, 3840, 2160, 0);
    output_name(o, NULL, "DP-1");
    o->pending_refresh_ns = 1000000000000ull / 60000ull;
    output_done(o, NULL);

    /* One output changed, so the display reports the change once. */
    assert(poll_monitors(&display.base) == 1);

    assert(conn_display_monitors(&display.base, info, 4) == 1);
    struct yasl_monitor_info *m = &info[0];
    assert(strcmp(m->name, "DP-1") == 0);
    /* The bounds are logical and divide the mode size by the scale. */
    assert(m->x == 1920 && m->y == 100);
    assert(m->width == 1920 && m->height == 1080);
    assert(m->work_x == m->x && m->work_width == m->width);
    assert(m->physical_width_mm == 600 && m->physical_height_mm == 340);
    assert(m->scale == 2.0);
    assert(m->dpi > 160.0 && m->dpi < 164.0);
    assert(m->refresh_hz > 59.9 && m->refresh_hz < 60.1);
    assert(m->orientation == YASL_ORIENTATION_90);
    assert(!m->primary);

    /* A capacity of one reports both monitors but writes one entry. */
    struct wl_conn_output *second = add_output(&display, 2, 1);
    output_geometry(second, NULL, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                    "make", "model", WL_OUTPUT_TRANSFORM_NORMAL);
    output_mode(second, NULL, WL_OUTPUT_MODE_CURRENT, 1280, 1024, 0);
    second->pending_refresh_ns = 1000000000000ull / 75000ull;
    output_done(second, NULL);
    assert(poll_monitors(&display.base) == 1);
    struct yasl_monitor_info one[1];
    assert(conn_display_monitors(&display.base, one, 1) == 2);
    assert(one[0].width == 1920);
    assert(conn_display_monitors(&display.base, NULL, 0) == 2);

    /* A done event without a change sends nothing. */
    output_done(o, NULL);
    assert(poll_monitors(&display.base) == 0);

    /* A rotation is a change, and the event reports it once. */
    output_geometry(o, NULL, 1920, 100, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                    "make", "model", WL_OUTPUT_TRANSFORM_NORMAL);
    output_done(o, NULL);
    assert(poll_monitors(&display.base) == 1);
    assert(conn_display_monitors(&display.base, info, 4) == 2);
    assert(info[0].orientation == YASL_ORIENTATION_0);

    yasl_private_events_close(&display.base);
    return 0;
}
