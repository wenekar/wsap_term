/* Supply native window-state properties without requiring an X server. */
#define XGetWindowProperty state_property
#include "../../src/platform/x11/window.c"
#undef XGetWindowProperty

static Atom properties[3];
static unsigned long property_count;
static int notifications;
static uint32_t reported_state;

int state_property(Display *display, Window window, Atom property,
        long offset, long length, Bool delete_property, Atom type,
        Atom *actual, int *format, unsigned long *count, unsigned long *left,
        unsigned char **data)
{
    assert(display && window == 42 && property == 10);
    assert(!offset && length >= 3 && !delete_property && type == XA_ATOM);
    *actual = XA_ATOM;
    *format = 32;
    *count = property_count;
    *left = 0;
    *data = NULL;
    if (property_count) {
        *data = malloc(property_count * sizeof(*properties));
        assert(*data);
        memcpy(*data, properties, property_count * sizeof(*properties));
    }
    return Success;
}

static void collect(struct yasl_private_display *display)
{
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(display, &event)) {
        assert(event.type == YASL_PRIVATE_EVENT_WINDOW_STATE);
        assert(yasl_private_surface_state(event.surface) == event.state.flags);
        reported_state = event.state.flags;
        notifications++;
    }
}

/* Build an XSETTINGS blob in either byte order. */
struct blob {
    unsigned char bytes[256];
    size_t len;
    bool msb;
};

static void put(struct blob *b, uint32_t v, int size)
{
    for (int i = 0; i < size; i++)
        b->bytes[b->len++] = (unsigned char)(v >> (8 * (b->msb ? size - 1 - i : i)));
}

static void put_name(struct blob *b, unsigned type, const char *name)
{
    size_t n = strlen(name);
    b->bytes[b->len++] = (unsigned char)type;
    b->bytes[b->len++] = 0;
    put(b, (uint32_t)n, 2);
    memcpy(b->bytes + b->len, name, n);
    b->len += (n + 3) & ~(size_t)3;
    put(b, 7, 4); /* Last-change serial. */
}

static void test_xsettings(void)
{
    for (int order = 0; order < 2; order++) {
        struct blob b = {.msb = order == 1};
        b.bytes[0] = b.msb ? MSBFirst : LSBFirst;
        b.len = 4;
        put(&b, 1, 4);
        put(&b, 4, 4);
        put_name(&b, 1, "Net/ThemeName");
        put(&b, 5, 4);
        memcpy(b.bytes + b.len, "Adwai", 5);
        b.len += 8;
        put_name(&b, 2, "Net/Color");
        put(&b, 0x1111, 2); put(&b, 0x2222, 2); put(&b, 0x3333, 2); put(&b, 0xffff, 2);
        put_name(&b, 0, "Net/DoubleClickTimeX");
        put(&b, 999, 4);
        put_name(&b, 0, "Net/DoubleClickTime");
        put(&b, 250, 4);

        int32_t ms = 0;
        assert(xsettings_int(b.bytes, b.len, "Net/DoubleClickTime", &ms) && ms == 250);
        assert(!xsettings_int(b.bytes, b.len, "Net/DoubleClick", &ms));
        /* A string with that name is not an integer setting. */
        assert(!xsettings_int(b.bytes, b.len, "Net/ThemeName", &ms));
        /* The value is last, so any truncation fails cleanly. */
        for (size_t len = 0; len < b.len; len++)
            assert(!xsettings_int(b.bytes, len, "Net/DoubleClickTime", &ms));
    }
}

int main(void)
{
    test_xsettings();
    struct x11_display display = {.base.ops = &yasl_private_x11_backend,
        .net_state = 10, .fullscreen = 20, .maximized_h = 21, .maximized_v = 22};
    display.native = (Display *)&display;
    struct x11_surface surface = {.base = {.display = &display.base,
        .state = YASL_PRIVATE_STATE_ACTIVATED}, .window = 42};
    assert(yasl_private_events_open(&display.base));
    assert(yasl_private_events_register(&surface.base));

    properties[0] = display.fullscreen;
    property_count = 1;
    update_state(&surface);
    collect(&display.base);
    assert(notifications == 1);
    assert(reported_state == (YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_ACTIVATED));
    update_state(&surface);
    collect(&display.base);
    assert(notifications == 1);

    /* Both maximized axes are required; activation survives property changes. */
    properties[0] = display.maximized_h;
    update_state(&surface);
    collect(&display.base);
    assert(notifications == 2 && reported_state == YASL_PRIVATE_STATE_ACTIVATED);
    properties[1] = display.maximized_v;
    property_count = 2;
    update_state(&surface);
    collect(&display.base);
    assert(notifications == 3);
    assert(reported_state == (YASL_PRIVATE_STATE_MAXIMIZED | YASL_PRIVATE_STATE_ACTIVATED));
    properties[2] = display.fullscreen;
    property_count = 3;
    update_state(&surface);
    collect(&display.base);
    assert(notifications == 4);
    assert(reported_state == (YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_MAXIMIZED | YASL_PRIVATE_STATE_ACTIVATED));

    property_count = 0;
    update_state(&surface);
    collect(&display.base);
    assert(notifications == 5 && reported_state == YASL_PRIVATE_STATE_ACTIVATED);
    update_state(&surface);
    collect(&display.base);
    assert(notifications == 5);
    yasl_private_events_unregister(&surface.base);
    yasl_private_events_close(&display.base);
    return 0;
}
