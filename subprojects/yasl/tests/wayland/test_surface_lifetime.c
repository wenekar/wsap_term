#include "../../src/platform/wayland/clipboard.c"
#include <assert.h>
#include <sys/socket.h>

static void receive(const char *text, size_t len, bool uri, struct wl_surface *surface,
                    uint64_t request, enum yasl_private_transfer_status status, void *ud)
{
    (void)text; (void)len; (void)uri; (void)surface; (void)request; (void)status;
    (*(int *)ud)++;
}

static void test_pending_paste(bool destroy)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *wl = wl_display_connect_to_fd(sockets[0]);
    assert(wl);
    struct wl_data_offer *offer_proxy = (struct wl_data_offer *)
        wl_proxy_create((struct wl_proxy *)wl, &wl_data_offer_interface);
    struct wl_data_device *device = (struct wl_data_device *)
        wl_proxy_create((struct wl_proxy *)wl, &wl_data_device_interface);
    assert(offer_proxy && device);
    struct offer offer = {.proxy = offer_proxy, .mime = "text/plain"};
    int callbacks = 0;
    struct clipboard clipboard = {
        .display = wl, .loop = yasl_loop_create(), .receive = receive, .ud = &callbacks,
        .device = device, .selection = &offer,
    };
    assert(clipboard.loop);
    struct seat seat = {.clipboard = &clipboard};
    struct wayland_display display = {.seats = &seat};
    struct wayland_surface *window = calloc(1, sizeof(*window));
    assert(window);
    window->conn = &display;
    window->surface = (struct wl_surface *)wl_proxy_create((struct wl_proxy *)wl, &wl_surface_interface);
    assert(window->surface);
    display.surfaces = window;
    assert(clipboard_request_paste(&clipboard, YASL_PRIVATE_CLIPBOARD, window->surface, 1));
    assert(callbacks == 0);
    if (destroy)
        wayland_surface_destroy(window);
    close(sockets[1]);
    for (int i = 0; i < 10 && !callbacks; i++)
        assert(yasl_loop_step(clipboard.loop, 0) >= 0);
    assert(callbacks == (destroy ? 0 : 1));
    if (!destroy)
        wayland_surface_destroy(window);
    wl_proxy_destroy((struct wl_proxy *)device);
    wl_proxy_destroy((struct wl_proxy *)offer_proxy);
    yasl_loop_destroy(clipboard.loop);
    wl_display_disconnect(wl);
}

int main(void)
{
    test_pending_paste(false);
    test_pending_paste(true);
    return 0;
}
