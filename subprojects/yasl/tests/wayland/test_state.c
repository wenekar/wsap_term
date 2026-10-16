#include "platform/wayland/internal.h"
#include <assert.h>

static uint32_t acknowledged_serial;
static unsigned acknowledge_count;
static void test_ack_configure(struct xdg_surface *surface, uint32_t serial);
#define xdg_surface_ack_configure test_ack_configure
#include "../../src/platform/wayland/shell.c"
#undef xdg_surface_ack_configure

static void test_ack_configure(struct xdg_surface *surface, uint32_t serial)
{
    (void)surface;
    acknowledged_serial = serial;
    acknowledge_count++;
}

struct notifications {
    int count;
    uint32_t state;
};

static void collect(struct yasl_private_display *display, struct notifications *notifications)
{
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(display, &event)) {
        assert(event.type == YASL_PRIVATE_EVENT_WINDOW_STATE);
        assert(yasl_private_surface_state(event.surface) == event.state.flags);
        notifications->count++;
        notifications->state = event.state.flags;
    }
}

static void configure(struct wayland_surface *surface, uint32_t *states, size_t count)
{
    struct wl_array array = {.size = count * sizeof(*states), .data = states};
    toplevel_configure(surface, NULL, 640, 480, &array);
    assert(surface->req_width == 640 && surface->req_height == 480);
}

int main(void)
{
    struct notifications notifications = {0};
    const struct yasl_private_backend_ops ops = {0};
    struct yasl_private_display display = {.ops = &ops};
    struct wayland_surface surface = {.base = {.display = &display, .state = YASL_PRIVATE_STATE_CSD}};
    assert(yasl_private_events_open(&display));
    assert(yasl_private_events_register(&surface.base));
    assert(yasl_private_surface_state(&surface.base) == YASL_PRIVATE_STATE_CSD);

    uint32_t fullscreen[] = {XDG_TOPLEVEL_STATE_FULLSCREEN,
        XDG_TOPLEVEL_STATE_ACTIVATED, XDG_TOPLEVEL_STATE_FULLSCREEN, 31, UINT32_MAX};
    configure(&surface, fullscreen, sizeof(fullscreen) / sizeof(*fullscreen));
    collect(&display, &notifications);
    assert(notifications.count == 1);
    assert(notifications.state == (YASL_PRIVATE_STATE_CSD | YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_ACTIVATED));
    configure(&surface, fullscreen, sizeof(fullscreen) / sizeof(*fullscreen));
    collect(&display, &notifications);
    assert(notifications.count == 1);

    /* Decoration changes preserve the other flags and do not notify twice. */
    toplevel_decoration_configure(&surface, NULL, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    collect(&display, &notifications);
    assert(notifications.count == 2);
    assert(notifications.state == (YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_ACTIVATED));
    toplevel_decoration_configure(&surface, NULL, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    collect(&display, &notifications);
    assert(notifications.count == 2);

    uint32_t maximized[] = {XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_SUSPENDED};
    configure(&surface, maximized, sizeof(maximized) / sizeof(*maximized));
    collect(&display, &notifications);
    assert(notifications.count == 3);
    assert(notifications.state == (YASL_PRIVATE_STATE_MAXIMIZED | YASL_PRIVATE_STATE_SUSPENDED));
    toplevel_decoration_configure(&surface, NULL, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    collect(&display, &notifications);
    assert(notifications.count == 4);
    assert(notifications.state == (YASL_PRIVATE_STATE_CSD | YASL_PRIVATE_STATE_MAXIMIZED | YASL_PRIVATE_STATE_SUSPENDED));

    /* Each tiled edge maps to the same portable flag; changing edges alone
     * leaves the reported state unchanged. */
    uint32_t edges[] = {XDG_TOPLEVEL_STATE_TILED_LEFT, XDG_TOPLEVEL_STATE_TILED_RIGHT,
        XDG_TOPLEVEL_STATE_TILED_TOP, XDG_TOPLEVEL_STATE_TILED_BOTTOM};
    for (size_t i = 0; i < sizeof(edges) / sizeof(*edges); i++) {
        configure(&surface, &edges[i], 1);
        collect(&display, &notifications);
        assert(notifications.count == 5);
        assert(notifications.state == (YASL_PRIVATE_STATE_CSD | YASL_PRIVATE_STATE_TILED));
    }
    configure(&surface, edges, 0);
    collect(&display, &notifications);
    assert(notifications.count == 6);
    assert(notifications.state == YASL_PRIVATE_STATE_CSD);

    /* The authoritative state advances with the queued notification. */
    configure(&surface, fullscreen, sizeof(fullscreen) / sizeof(*fullscreen));
    assert(yasl_private_surface_state(&surface.base) ==
            (YASL_PRIVATE_STATE_CSD | YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_ACTIVATED));
    collect(&display, &notifications);
    assert(notifications.count == 7);

    /* External presentation has no SHM commit callback to acknowledge xdg
     * configures. Apply geometry and acknowledge before renderer submission. */
    struct wayland_surface gpu = {
        .base.presenter = YASL_PRIVATE_PRESENTER_VULKAN,
        .width = 0, .height = 0,
        .logical_width = 64, .logical_height = 64,
        .req_width = 64, .req_height = 64,
        .scale_120 = YASL_PRIVATE_SCALE_UNIT,
        .pending_scale_120 = 180,
    };
    acknowledge_count = 0;
    xdg_surface_configure(&gpu, (struct xdg_surface *)&gpu, 1234);
    assert(acknowledge_count == 1 && acknowledged_serial == 1234);
    assert(!gpu.has_pending_serial && !gpu.size_pending);
    assert(gpu.configured && gpu.width == 96 && gpu.height == 96);

    /* CPU presentation keeps its existing deferred ack with the SHM commit. */
    struct wayland_surface cpu = {.base.presenter = YASL_PRIVATE_PRESENTER_CPU};
    xdg_surface_configure(&cpu, (struct xdg_surface *)&cpu, 5678);
    assert(acknowledge_count == 1 && cpu.has_pending_serial && cpu.want_draw);
    yasl_private_events_unregister(&surface.base);
    yasl_private_events_close(&display);
    return 0;
}
