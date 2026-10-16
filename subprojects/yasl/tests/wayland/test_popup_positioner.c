/* Capture explicit xdg_positioner placement without requiring a compositor. */
#include "platform/wayland/internal.h"
#include <assert.h>

struct recorded_positioner {
    int width, height;
    int rect_x, rect_y, rect_width, rect_height;
    uint32_t anchor, gravity, constraints;
    bool reactive;
};

static struct recorded_positioner record;
static uint64_t fake_positioner_storage[8];
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

    spec.anchor_edges = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT + 1;
    assert(!popup_spec_valid(&parent, &spec));
    spec.anchor_edges = XDG_POSITIONER_ANCHOR_NONE;
    spec.constraints = 1u << 9;
    assert(!popup_spec_valid(&parent, &spec));
    return 0;
}
