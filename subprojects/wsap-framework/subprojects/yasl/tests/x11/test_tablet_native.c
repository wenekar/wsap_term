/* Real Xorg driver -> XI2 -> YASL queue -> the portable example -> CPU pixels. */
#define main tablet_example_main
#include "../../examples/tablet.c"
#undef main
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "inputtest-protocol.h"

static void send_all(int fd, const void *message, size_t n)
{
    const char *p = message;
    while (n) {
        ssize_t sent = write(fd, p, n);
        if (sent < 0 && errno == EINTR) continue;
        assert(sent > 0);
        p += sent;
        n -= (size_t)sent;
    }
}

static void receive_all(int fd, void *message, size_t n)
{
    char *p = message;
    while (n) {
        ssize_t got = read(fd, p, n);
        if (got < 0 && errno == EINTR) continue;
        assert(got > 0);
        p += got;
        n -= (size_t)got;
    }
}

static int connect_tool(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    xf86ITEventClientVersion version = {{sizeof(version), XF86IT_EVENT_CLIENT_VERSION}, 1, 1};
    send_all(fd, &version, sizeof(version));
    xf86ITResponseServerVersion response;
    receive_all(fd, &response, sizeof(response));
    assert(response.header.type == XF86IT_RESPONSE_SERVER_VERSION);
    assert(response.header.length == sizeof(response) && response.major == 1 && response.minor >= 1);
    return fd;
}

struct canvas_state {
    struct yasl_pen_event pen;
    double x, y;
    uint32_t *pixels;
    int width, height, painted_width, painted_height;
    bool down;
};

static void render_event(yasl_surface *surface, struct canvas_state *state, const yasl_event *event)
{
    if (event->type == YASL_EVENT_PEN) {
        state->pen = event->pen;
        state->x = event->x;
        state->y = event->y;
        state->down = event->pen.in_contact;
    }
    free(state->pixels);
    uint32_t *pixels = canvas(surface, &state->width, &state->height,
        &state->painted_width, &state->painted_height, false);
    assert(pixels);
    if (event->type == YASL_EVENT_PEN && state->down &&
        (event->action == YASL_PEN_DOWN || event->action == YASL_PEN_MOVE)) {
        double scale = yasl_surface_get_info(surface).scale;
        stamp(pixels, state->width, state->height, event->x * scale, event->y * scale,
            (1.5 + 12 * event->pen.pressure) * scale, event->pen.tool == YASL_PEN_ERASER);
    }
    uint32_t *snapshot = malloc((size_t)state->width * state->height * sizeof(*snapshot));
    assert(snapshot);
    memcpy(snapshot, pixels, (size_t)state->width * state->height * sizeof(*snapshot));
    assert(yasl_cpu_present(surface) == 0);
    state->pixels = snapshot;
}

static void drain(yasl_surface *surface, struct canvas_state *state)
{
    yasl_event event;
    int result;
    while ((result = yasl_wait_event(&event, 0)) > 0)
        render_event(surface, state, &event);
    assert(result == 0);
}

static void pump(int fd, yasl_surface *surface, struct canvas_state *state)
{
    xf86ITEventWaitForSync sync = {{sizeof(sync), XF86IT_EVENT_WAIT_FOR_SYNC}};
    send_all(fd, &sync, sizeof(sync));
    xf86ITResponseHeader response;
    receive_all(fd, &response, sizeof(response));
    assert(response.type == XF86IT_RESPONSE_SYNC_FINISHED && response.length == sizeof(response));
    /* The driver acknowledgment guarantees XI2 processing has finished. */
    struct yasl_native native;
    assert(yasl_surface_get_native(surface, &native) == 0);
    XSync(native.x11_display, False);
    drain(surface, state);
    /* Public polling submits the CPU frame before the native readback. */
    XSync(native.x11_display, False);
}

static void set_eraser_property(Display *display)
{
    int count = 0;
    XIDeviceInfo *devices = XIQueryDevice(display, XIAllDevices, &count);
    assert(devices);
    bool found = false;
    for (int i = 0; i < count; i++) {
        /* The name deliberately doesn't contain "eraser": exercise the
         * driver's tool property, independently of the name fallback. */
        if (strcmp(devices[i].name, "YASL Test Reverse Tool")) continue;
        Atom property = XInternAtom(display, "Wacom Tool Type", False);
        uint32_t eraser = (uint32_t)XInternAtom(display, "ERASER", False);
        XIChangeProperty(display, devices[i].deviceid, property, XA_ATOM, 32,
                         PropModeReplace, (unsigned char *)&eraser, 1);
        found = true;
    }
    XIFreeDeviceInfo(devices);
    assert(found);
    XSync(display, False);
}

int main(int argc, char **argv)
{
    assert(argc == 3); /* pen socket, eraser socket */
    struct canvas_state app = {0};
    assert(yasl_init(&(struct yasl_options){.backend = YASL_BACKEND_X11}) == 0);
    yasl_surface *surface = yasl_surface_create(&(struct yasl_surface_options){.title="YASL native pressure test", .width=500, .height=400});
    assert(surface && (yasl_get_features() & YASL_FEATURE_TABLET));
    struct yasl_native native;
    assert(yasl_surface_get_native(surface, &native) == 0);
    Display *x = native.x11_display;
    Window window = (Window)native.x11_window;
    XMoveWindow(x, window, 0, 0);
    XSetInputFocus(x, window, RevertToParent, CurrentTime);
    XSync(x, False);
    drain(surface, &app);
    set_eraser_property(x);
    int pen_fd = connect_tool(argv[1]), eraser_fd = connect_tool(argv[2]);
    xf86ITEventMotion motion = {.header = {sizeof(motion), XF86IT_EVENT_MOTION}, .is_absolute = 1};
    motion.valuators.mask[0] = (1 << 0) | (1 << 1) | (1 << 4);
    motion.valuators.valuators[0] = 150.0 * 65535 / (DisplayWidth(x, DefaultScreen(x)) - 1);
    motion.valuators.valuators[1] = 200.0 * 65535 / (DisplayHeight(x, DefaultScreen(x)) - 1);
    xf86ITEventProximity proximity = {.header = {sizeof(proximity), XF86IT_EVENT_PROXIMITY},
        .is_prox_in = 1, .valuators = motion.valuators};
    send_all(pen_fd, &proximity, sizeof(proximity));
    send_all(pen_fd, &motion, sizeof(motion));
    pump(pen_fd, surface, &app);
    assert(!app.down && app.pen.tool == YASL_PEN_PEN);
    assert(app.x == 150 && app.y == 200);
    assert(app.pixels[(size_t)200 * app.width + 150] == PAPER);
    xf86ITEventButton button = {.header = {sizeof(button), XF86IT_EVENT_BUTTON},
        .is_absolute = 1, .button = 1, .is_press = 1, .valuators = motion.valuators};
    button.valuators.valuators[4] = 250; /* Driver pressure range is 0..1000. */
    send_all(pen_fd, &button, sizeof(button));
    pump(pen_fd, surface, &app);
    assert(app.down && fabs(app.pen.pressure - 0.25) < 0.001);
    assert(app.pixels[(size_t)200 * app.width + 150] == INK &&
            app.pixels[(size_t)200 * app.width + 160] == PAPER);
    motion.valuators.mask[0] = 1 << 4; /* No position change. */
    motion.valuators.valuators[4] = 1000;
    send_all(pen_fd, &motion, sizeof(motion));
    pump(pen_fd, surface, &app);
    assert(app.down && app.pen.pressure == 1.0);
    assert(app.pixels[(size_t)200 * app.width + 160] == INK);
    /* Check the actual window's pixels too, after the example presents. */
    XImage *image = XGetImage(x, window, 160, 200, 1, 1, AllPlanes, ZPixmap);
    assert(image && (XGetPixel(image, 0, 0) & 0xffffff) == (INK & 0xffffff));
    XDestroyImage(image);
    button.is_press = 0;
    button.valuators.mask[0] = 0; /* Release omits all valuators. */
    send_all(pen_fd, &button, sizeof(button));
    pump(pen_fd, surface, &app);
    assert(!app.down && !app.pen.pressure);

    send_all(eraser_fd, &proximity, sizeof(proximity));
    button.is_press = 1;
    button.valuators = proximity.valuators;
    button.valuators.valuators[4] = 1000;
    send_all(eraser_fd, &button, sizeof(button));
    pump(eraser_fd, surface, &app);
    assert(app.down && app.pen.tool == YASL_PEN_ERASER && app.pen.pressure == 1.0);
    assert(app.pixels[(size_t)200 * app.width + 150] == PAPER &&
            app.pixels[(size_t)200 * app.width + 160] == PAPER);
    button.is_press = 0;
    send_all(eraser_fd, &button, sizeof(button));
    pump(eraser_fd, surface, &app);
    assert(!app.down && !app.pen.pressure);
    close(pen_fd);
    close(eraser_fd);
    free(app.pixels);
    yasl_shutdown();
    puts("Native Xorg pen pressure, stationary pressure, release, and eraser drawing passed.");
    return 0;
}
