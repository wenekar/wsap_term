/* Raw motion and size-limit hints without an X server. The window-manager
 * calls are stubs, and the raw motion event needs only its own record.
 * window.c comes first, so its feature-test macros reach the system headers. */
#define XGetWMNormalHints test_get_normal_hints
#define XSetWMNormalHints test_set_normal_hints
#define XFlush test_flush
#include "../../src/platform/x11/window.c"
#undef XGetWMNormalHints
#undef XSetWMNormalHints
#undef XFlush

static XSizeHints hints;
static long hints_supplied;
static int hints_sets;

int test_get_normal_hints(Display *display, Window window, XSizeHints *out, long *supplied)
{
    assert(display && window == 42 && out && supplied);
    *out = hints;
    *supplied = hints_supplied;
    return 1;
}

void test_set_normal_hints(Display *display, Window window, XSizeHints *value)
{
    assert(display && window == 42 && value);
    hints = *value;
    hints_sets++;
}

int test_flush(Display *display)
{
    assert(display);
    return 1;
}

/* A single axis alone is a real delta. raw_values holds only the set bits, so
 * the second motion axis must not read the first value. */
static void test_raw_motion(void)
{
    unsigned char mask[4] = {0};
    double values[2] = {0, 0};
    XIRawEvent raw = {0};
    raw.valuators.mask = mask;
    raw.valuators.mask_len = sizeof(mask);
    raw.raw_values = values;
    XGenericEventCookie cookie = {.evtype = XI_RawMotion, .data = &raw};
    struct x11_display display = {0};

    XISetMask(mask, 1); /* The vertical axis alone. */
    values[0] = 7.5;
    xi_event(&display, &cookie);
    assert(display.raw_valid);
    assert(display.raw_delta.x == 0 && display.raw_delta.y == 7.5);

    display.raw_valid = false;
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, 0); /* The horizontal axis alone. */
    values[0] = -3;
    xi_event(&display, &cookie);
    assert(display.raw_valid);
    assert(display.raw_delta.x == -3 && display.raw_delta.y == 0);

    /* A third valuator must not shift the two motion axes. */
    display.raw_valid = false;
    XISetMask(mask, 2);
    values[0] = 2;
    values[1] = 99;
    xi_event(&display, &cookie);
    assert(display.raw_valid);
    assert(display.raw_delta.x == 2 && display.raw_delta.y == 0);

    /* Each axis reads the value at its own position among the set bits. */
    display.raw_valid = false;
    XISetMask(mask, 1);
    values[0] = 4;
    values[1] = 5;
    xi_event(&display, &cookie);
    assert(display.raw_valid);
    assert(display.raw_delta.x == 4 && display.raw_delta.y == 5);
}

/* A zero maximum removes the limit of that axis, and the other axis keeps its
 * own limit. XSizeHints has one flag for both, so a free axis gets the largest
 * value instead of zero. */
static void test_size_limits(void)
{
    struct x11_display display = {0};
    display.native = (Display *)&display;
    struct x11_surface surface = {.base = {.display = &display.base, .scale_120 = 240},
                                  .window = 42};

    hints = (XSizeHints){0};
    hints_supplied = 0;
    hints_sets = 0;
    assert(surface_set_size_limits(&surface.base,
            (struct yasl_private_vector2i){10, 20},
            (struct yasl_private_vector2i){100, 0}));
    assert(hints_sets == 1);
    assert((hints.flags & (PMinSize | PMaxSize)) == (PMinSize | PMaxSize));
    assert(hints.min_width == 20 && hints.min_height == 40);
    assert(hints.max_width == 200 && hints.max_height == X11_SIZE_LIMIT_MAX);

    assert(surface_set_size_limits(&surface.base,
            (struct yasl_private_vector2i){0, 0},
            (struct yasl_private_vector2i){0, 50}));
    assert(hints.max_width == X11_SIZE_LIMIT_MAX && hints.max_height == 100);

    /* No maximum clears the maximum flag. */
    assert(surface_set_size_limits(&surface.base,
            (struct yasl_private_vector2i){0, 0},
            (struct yasl_private_vector2i){0, 0}));
    assert(!(hints.flags & PMaxSize));
}

/* RandR reports device pixels; the monitor API reports logical units. */
static void test_monitor_units(void)
{
    XRRMonitorInfo monitor = {.x = 1920, .y = 0, .width = 3840, .height = 2160,
                              .mwidth = 600, .mheight = 340};
    struct yasl_monitor_info info = {0};
    monitor_extent(&info, &monitor, 240); /* 2x */
    assert(info.x == 960 && info.y == 0);
    assert(info.width == 1920 && info.height == 1080);
    assert(info.work_x == 960 && info.work_y == 0);
    assert(info.work_width == 1920 && info.work_height == 1080);

    monitor_extent(&info, &monitor, YASL_PRIVATE_SCALE_UNIT); /* 1x */
    assert(info.x == 1920 && info.width == 3840 && info.height == 2160);

    /* A scale of zero falls back to 1x. */
    monitor_extent(&info, &monitor, 0);
    assert(info.width == 3840);
}

int main(void)
{
    test_raw_motion();
    test_size_limits();
    test_monitor_units();
    return 0;
}
