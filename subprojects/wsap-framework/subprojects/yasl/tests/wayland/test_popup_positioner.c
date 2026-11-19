/* Capture explicit xdg_positioner placement without requiring a compositor. */
#include "platform/wayland/internal.h"
#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>

struct recorded_positioner {
    int width, height;
    int rect_x, rect_y, rect_width, rect_height;
    uint32_t anchor, gravity, constraints;
    bool reactive;
};

static struct recorded_positioner record;
static uint64_t fake_positioner_storage[8];
static uint32_t reposition_token;
static uint32_t fake_popup_version(struct xdg_popup *popup)
{ (void)popup; return XDG_POPUP_REPOSITION_SINCE_VERSION; }
static void fake_reposition(struct xdg_popup *popup, struct xdg_positioner *pos, uint32_t token)
{ (void)popup; (void)pos; reposition_token = token; }
static void fake_destroy_positioner(struct xdg_positioner *pos)
{ (void)pos; }
#define FAKE_POSITIONER ((struct xdg_positioner *)fake_positioner_storage)
static struct xdg_positioner *fake_create(struct xdg_wm_base *base)
{ (void)base; record = (struct recorded_positioner){0}; return FAKE_POSITIONER; }
static void fake_size(struct xdg_positioner *pos, int32_t width, int32_t height)
{ assert(pos == FAKE_POSITIONER); record.width = width; record.height = height; }
static void fake_rect(struct xdg_positioner *pos, int32_t x, int32_t y, int32_t width, int32_t height)
{ assert(pos == FAKE_POSITIONER); record.rect_x=x; record.rect_y=y; record.rect_width=width; record.rect_height=height; }
static void fake_anchor(struct xdg_positioner *pos, uint32_t value)
{ assert(pos == FAKE_POSITIONER); record.anchor = value; }
static void fake_gravity(struct xdg_positioner *pos, uint32_t value)
{ assert(pos == FAKE_POSITIONER); record.gravity = value; }
static void fake_constraints(struct xdg_positioner *pos, uint32_t value)
{ assert(pos == FAKE_POSITIONER); record.constraints = value; }
static uint32_t fake_version(struct xdg_positioner *pos)
{ assert(pos == FAKE_POSITIONER); return XDG_POSITIONER_SET_REACTIVE_SINCE_VERSION; }
static void fake_reactive(struct xdg_positioner *pos)
{ assert(pos == FAKE_POSITIONER); record.reactive = true; }

#define xdg_wm_base_create_positioner fake_create
#define xdg_positioner_set_size fake_size
#define xdg_positioner_set_anchor_rect fake_rect
#define xdg_positioner_set_anchor fake_anchor
#define xdg_positioner_set_gravity fake_gravity
#define xdg_positioner_set_constraint_adjustment fake_constraints
#define xdg_positioner_get_version fake_version
#define xdg_positioner_set_reactive fake_reactive
#define xdg_popup_get_version fake_popup_version
#define xdg_popup_reposition fake_reposition
#define xdg_positioner_destroy fake_destroy_positioner
#include "../../src/platform/wayland/popup.c"

int main(void)
{
    struct wayland_display display = {.wm_base = (struct xdg_wm_base *)&record};
    struct wayland_surface parent = {
        .scale_120 = 120,
        .content = {.x = 3, .y = 4},
    };
    struct wayland_surface popup = {
        .conn = &display,
        .parent = &parent,
        .scale_120 = 120,
        .content = {.width = 48, .height = 36},
    };
    struct yasl_private_popup_spec spec = {
        .size = {64, 48},
        .content_size = {48, 36},
        .anchor = {13, 24, 8, 6},
        .anchor_edges = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
        .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
        .constraints = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
    };

    struct xdg_positioner *positioner = popup_positioner(&popup, &spec);
    assert(positioner == FAKE_POSITIONER);
    assert(record.width == 48 && record.height == 36);
    assert(record.rect_x == 10 && record.rect_y == 20 &&
           record.rect_width == 8 && record.rect_height == 6);
    assert(record.anchor == XDG_POSITIONER_ANCHOR_TOP_RIGHT);
    assert(record.gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    assert(record.constraints == (XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y));
    assert(record.reactive);

    spec.anchor_edges = XDG_POSITIONER_ANCHOR_NONE;
    spec.gravity = XDG_POSITIONER_GRAVITY_RIGHT;
    spec.constraints = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
                       XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
    assert(popup_spec_valid(&parent, &spec));
    positioner = popup_positioner(&popup, &spec);
    assert(positioner == FAKE_POSITIONER);
    assert(record.anchor == XDG_POSITIONER_ANCHOR_NONE);
    assert(record.gravity == XDG_POSITIONER_GRAVITY_RIGHT);
    assert(record.constraints == spec.constraints);

    /* A reposition takes effect after its configure, including a constrained size. */
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *wl = wl_display_connect_to_fd(sockets[0]);
    assert(wl);
    popup.surface = (struct wl_surface *)wl_proxy_create((struct wl_proxy *)wl, &wl_surface_interface);
    popup.xdg_surface = (struct xdg_surface *)wl_proxy_create((struct wl_proxy *)wl, &xdg_surface_interface);
    popup.popup = (struct xdg_popup *)wl_proxy_create((struct wl_proxy *)wl, &xdg_popup_interface);
    assert(popup.surface && popup.xdg_surface && popup.popup);
    popup.configured = true;
    popup.base.presenter = YASL_PRIVATE_PRESENTER_CPU;
    display.surfaces = &popup;
    wayland_surface_set_callbacks(&popup, NULL, NULL);
    popup.pending_scale_120 = 120;
    popup.width = popup.logical_width = 64;
    popup.height = popup.logical_height = 48;
    spec.size = (struct yasl_private_vector2i){70, 54};
    spec.content_size = (struct yasl_private_vector2i){54, 42};
    assert(wayland_surface_popup_reposition(&popup, &spec));
    uint32_t first_token = reposition_token;
    spec.size = (struct yasl_private_vector2i){84, 58};
    spec.content_size = (struct yasl_private_vector2i){68, 46};
    assert(wayland_surface_popup_reposition(&popup, &spec));
    uint32_t second_token = reposition_token;
    assert(wayland_surface_geometry(&popup).size.x == 64);
    popup_listener.repositioned(&popup, popup.popup, first_token);
    popup_listener.configure(&popup, popup.popup, 0, 0, 54, 42);
    shell_surface_listener.configure(&popup, popup.xdg_surface, 41);
    wayland_display_flush(&display);
    assert(wayland_surface_geometry(&popup).size.x == 70);
    popup_listener.repositioned(&popup, popup.popup, second_token);
    popup_listener.configure(&popup, popup.popup, 0, 0, 50, 30);
    assert(wayland_surface_geometry(&popup).size.x == 70);
    shell_surface_listener.configure(&popup, popup.xdg_surface, 42);
    wayland_display_flush(&display);
    struct yasl_private_geometry geometry = wayland_surface_geometry(&popup);
    assert(geometry.size.x == 66 && geometry.size.y == 42);

    parent.scale_120 = 180;
    spec.size = (struct yasl_private_vector2i){96, 72};
    spec.content_offset = (struct yasl_private_vector2i){9, 6};
    spec.content_size = (struct yasl_private_vector2i){72, 48};
    assert(wayland_surface_popup_reposition(&popup, &spec));
    popup_listener.repositioned(&popup, popup.popup, reposition_token);
    popup_listener.configure(&popup, popup.popup, 0, 0, 30, 20);
    shell_surface_listener.configure(&popup, popup.xdg_surface, 43);
    wayland_display_flush(&display);
    geometry = wayland_surface_geometry(&popup);
    assert(geometry.scale_120 == 180 && geometry.size.x == 69 && geometry.size.y == 54);

    /* The compositor accepted the logical size. Keep the requested pixel size. */
    popup.viewport = (struct wp_viewport *)wl_proxy_create((struct wl_proxy *)wl, &wp_viewport_interface);
    assert(popup.viewport);
    spec.size = (struct yasl_private_vector2i){395, 300};
    spec.content_offset = (struct yasl_private_vector2i){15, 15};
    spec.content_size = (struct yasl_private_vector2i){365, 270};
    assert(wayland_surface_popup_reposition(&popup, &spec));
    popup_listener.repositioned(&popup, popup.popup, reposition_token);
    popup_listener.configure(&popup, popup.popup, 0, 0, 243, 180);
    shell_surface_listener.configure(&popup, popup.xdg_surface, 44);
    wayland_display_flush(&display);
    geometry = wayland_surface_geometry(&popup);
    assert(geometry.size.x == 395 && geometry.size.y == 300);
    wl_proxy_destroy((struct wl_proxy *)popup.viewport);
    wl_proxy_destroy((struct wl_proxy *)popup.popup);
    wl_proxy_destroy((struct wl_proxy *)popup.xdg_surface);
    wl_proxy_destroy((struct wl_proxy *)popup.surface);
    wl_display_disconnect(wl);
    close(sockets[1]);

    spec.content_size.x = 0;
    spec.content_offset.x = spec.size.x;
    assert(!popup_spec_valid(&parent, &spec));
    spec.content_offset.x = 0;
    spec.anchor_edges = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT + 1;
    assert(!popup_spec_valid(&parent, &spec));
    spec.anchor_edges = XDG_POSITIONER_ANCHOR_NONE;
    spec.constraints = 1u << 9;
    assert(!popup_spec_valid(&parent, &spec));
    return 0;
}
