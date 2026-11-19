#include "platform/wayland/internal.h"
#include <assert.h>
#include <sys/socket.h>

#include "../../src/platform/wayland/shell.c"

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
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *wl = wl_display_connect_to_fd(sockets[0]);
    assert(wl);
    struct xdg_surface *xdg = (struct xdg_surface *)
        wl_proxy_create((struct wl_proxy *)wl, &xdg_surface_interface);
    assert(xdg);
    uint32_t id = wl_proxy_get_id((struct wl_proxy *)xdg);
    xdg_surface_configure(&gpu, xdg, 1234);
    assert(wl_display_flush(wl) >= 0);
    uint32_t message[3];
    assert(recv(sockets[1], message, sizeof(message), MSG_WAITALL) == sizeof(message));
    assert(message[0] == id && (message[1] & 0xffff) == XDG_SURFACE_ACK_CONFIGURE &&
           (message[1] >> 16) == sizeof(message) && message[2] == 1234);
    assert(!gpu.has_pending_serial && !gpu.size_pending);
    assert(gpu.configured && gpu.width == 96 && gpu.height == 96);

    /* CPU presentation keeps its existing deferred ack with the SHM commit. */
    struct wayland_surface cpu = {.base.presenter = YASL_PRIVATE_PRESENTER_CPU};
    xdg_surface_configure(&cpu, (struct xdg_surface *)&cpu, 5678);
    assert(cpu.has_pending_serial && cpu.want_draw);

    /* Version 1 sends mode changes without a following done event. */
    struct wl_output *output = (struct wl_output *)
        wl_proxy_create((struct wl_proxy *)wl, &wl_output_interface);
    struct wl_surface *native = (struct wl_surface *)
        wl_proxy_create((struct wl_proxy *)wl, &wl_surface_interface);
    assert(output && native);
    struct wayland_display connection = {.base.ops = &yasl_private_wayland_backend};
    struct wl_conn_output monitor = {.conn = &connection, .proxy = output,
        .mask = 1, .scale = 1, .pending_scale = 1};
    struct wayland_surface window = {.base = {.presenter = YASL_PRIVATE_PRESENTER_CPU},
        .conn = &connection, .surface = native, .outputs = 1, .pending_scale_120 = 120};
    connection.outputs = &monitor;
    connection.surfaces = &window;
    output_listener.mode(&monitor, output, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 100000);
    assert(wayland_surface_timing(&window).refresh_ns == 10000000);
    wl_surface_destroy(native);
    wl_output_destroy(output);
    xdg_surface_destroy(xdg);
    wl_display_disconnect(wl);
    close(sockets[1]);
    /* Vulkan capability requires linux-dmabuf or wl_drm on Wayland. */
    struct wayland_display cap_conn = {0};
    assert(!(wayland_display_caps(&cap_conn) & YASL_PRIVATE_CAP_VULKAN));
    registry_global(&cap_conn, NULL, 1, "zwlr_export_dmabuf_manager_v1", 1);
    assert(!(wayland_display_caps(&cap_conn) & YASL_PRIVATE_CAP_VULKAN));
    registry_global(&cap_conn, NULL, 2, "zwp_linux_dmabuf_v1", 3);
    assert(wayland_display_caps(&cap_conn) & YASL_PRIVATE_CAP_VULKAN);
    struct wayland_display drm_conn = {0};
    registry_global(&drm_conn, NULL, 3, "wl_drm", 2);
    assert(wayland_display_caps(&drm_conn) & YASL_PRIVATE_CAP_VULKAN);

    yasl_private_events_unregister(&surface.base);
    yasl_private_events_close(&display);
    return 0;
}
