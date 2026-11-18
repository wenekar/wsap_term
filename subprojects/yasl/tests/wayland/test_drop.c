/* External drags, driven from the compositor end of a socketpair. A drag takes
 * the first type that the surface lists and the drag offers. A drag with no
 * such type is refused: the client accepts no type, asks for no data, and the
 * application gets no event. An accepted drag reports hover with its type, and
 * its drop delivers the bytes of that type as one DROP event. */
#include "platform/wayland/internal.h"
#include "wire.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Compositor events and client requests, by opcode from wayland.xml. */
#define DEVICE_DATA_OFFER 0
#define DEVICE_ENTER 1
#define DEVICE_MOTION 3
#define DEVICE_DROP 4
#define OFFER_OFFER 0
#define OFFER_ACCEPT 0
#define OFFER_RECEIVE 1
/* The first object that the compositor creates takes the first server-side id. */
#define OFFER_ID 0xff000000u
/* Size of the image drop. It holds NUL bytes and is larger than one pipe buffer. */
#define IMAGE_BYTES (300u * 1024)

struct drag {
    int server;
    struct wl_display *display;
    struct yasl_loop *loop;
    struct wayland_display d;
    struct seat seat;
    struct wayland_surface w;
    uint32_t device, surface;
    struct wire_inbox in;
};

static uint8_t image[IMAGE_BYTES];
static const char *const image_types[] = {"image/*", "text/uri-list", "text/plain;charset=utf-8"};

static void setup(struct drag *t)
{
    *t = (struct drag){0};
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    t->server = sockets[1];
    t->display = wl_display_connect_to_fd(sockets[0]);
    assert(t->display);
    struct wl_data_device_manager *manager = (struct wl_data_device_manager *)
        wl_proxy_create((struct wl_proxy *)t->display, &wl_data_device_manager_interface);
    struct wl_seat *seat = (struct wl_seat *)
        wl_proxy_create((struct wl_proxy *)t->display, &wl_seat_interface);
    struct wl_surface *surface = (struct wl_surface *)
        wl_proxy_create((struct wl_proxy *)t->display, &wl_surface_interface);
    t->loop = yasl_loop_create();
    assert(manager && seat && surface && t->loop);
    t->d = (struct wayland_display){.base.ops = &yasl_private_wayland_backend,
        .wl = t->display, .data_manager = manager, .loop = t->loop};
    t->seat = (struct seat){.conn = &t->d, .proxy = seat};
    t->w = (struct wayland_surface){.base.display = &t->d.base, .conn = &t->d,
        .scale_factor = 1, .surface = surface};
    wl_surface_set_user_data(surface, &t->w);
    seat_clipboard_ensure(&t->seat);
    assert(t->seat.clipboard);
    assert(yasl_private_events_open(&t->d.base));
    assert(yasl_private_events_register(&t->w.base));
    t->device = wl_proxy_get_id((struct wl_proxy *)clipboard_data_device(t->seat.clipboard));
    t->surface = wl_proxy_get_id((struct wl_proxy *)surface);
}

static void teardown(struct drag *t)
{
    yasl_private_surface_set_drop_types(&t->w.base, NULL, 0);
    yasl_private_events_unregister(&t->w.base);
    yasl_private_events_close(&t->d.base);
    clipboard_destroy(t->seat.clipboard);
    wl_proxy_destroy((struct wl_proxy *)t->w.surface);
    wl_proxy_destroy((struct wl_proxy *)t->seat.proxy);
    wl_proxy_destroy((struct wl_proxy *)t->d.data_manager);
    wl_display_disconnect(t->display);
    close(t->server);
    input_queue_clear(&t->w.input);
    yasl_loop_destroy(t->loop);
}

/* Sets the drop types of the test surface, best first. */
static void set_types(struct drag *t, const char *const *types, size_t count)
{
    assert(yasl_private_surface_set_drop_types(&t->w.base, types, count));
}

/* The compositor starts a drag with these mime types. */
static void offer(struct drag *t, const char *const *mimes, size_t count)
{
    uint8_t args[64];
    wire_put_u32(args, OFFER_ID);
    wire_event(t->server, t->device, DEVICE_DATA_OFFER, args, 4, -1);
    for (size_t i = 0; i < count; i++) {
        size_t n = wire_put_string(args, mimes[i]);
        wire_event(t->server, OFFER_ID, OFFER_OFFER, args, n, -1);
    }
}

/* The drag enters the surface at (3, 4). */
static void enter(struct drag *t, uint32_t serial)
{
    uint8_t args[20];
    wire_put_u32(args, serial);
    wire_put_u32(args + 4, t->surface);
    wire_put_u32(args + 8, (uint32_t)wl_fixed_from_int(3));
    wire_put_u32(args + 12, (uint32_t)wl_fixed_from_int(4));
    wire_put_u32(args + 16, OFFER_ID);
    wire_event(t->server, t->device, DEVICE_ENTER, args, sizeof(args), -1);
}

/* The drag moves to (x, y) on the surface. */
static void motion(struct drag *t, int x, int y)
{
    uint8_t args[12];
    wire_put_u32(args, 0); /* time */
    wire_put_u32(args + 4, (uint32_t)wl_fixed_from_int(x));
    wire_put_u32(args + 8, (uint32_t)wl_fixed_from_int(y));
    wire_event(t->server, t->device, DEVICE_MOTION, args, sizeof(args), -1);
}

static void drop(struct drag *t)
{
    wire_event(t->server, t->device, DEVICE_DROP, NULL, 0, -1);
}

/* Reads the mime type of the client's last accept. NULL means no type. */
static bool accepted(const struct wire_inbox *in, const char **mime)
{
    struct wire_message m;
    size_t pos = 0;
    bool found = false;
    while (wire_next(in, &pos, &m)) {
        if (m.object != OFFER_ID || m.opcode != OFFER_ACCEPT)
            continue;
        size_t offset = 4; /* The serial comes first. */
        found = true;
        *mime = wire_u32(m.args + 4) ? wire_get_string(m.args, &offset) : NULL;
    }
    return found;
}

/* Reads the mime type of the client's last receive request. */
static bool received(const struct wire_inbox *in, const char **mime)
{
    struct wire_message m;
    size_t pos = 0;
    bool found = false;
    while (wire_next(in, &pos, &m)) {
        if (m.object != OFFER_ID || m.opcode != OFFER_RECEIVE)
            continue;
        size_t offset = 0;
        found = true;
        *mime = wire_get_string(m.args, &offset);
    }
    return found;
}

/* Reads the next event, which must be a hover of `action` at (x, y). An OVER
 * hover carries `mime`. A LEAVE hover carries no type. */
static void expect_hover(struct drag *t, enum yasl_private_drop_event action,
                         const char *mime, int x, int y)
{
    struct yasl_private_event ev;
    assert(yasl_private_display_poll_event(&t->d.base, &ev));
    assert(ev.type == YASL_PRIVATE_EVENT_DROP_HOVER && ev.surface == &t->w.base);
    assert(ev.drop.action == action);
    assert(ev.drop.pos.x == x && ev.drop.pos.y == y);
    assert(strcmp(ev.drop.mime, mime ? mime : "") == 0);
}

/* Runs the loop until an event is ready, or gives up after a bounded wait. */
static bool wait_event(struct drag *t, struct yasl_private_event *ev)
{
    for (int spins = 0; spins < 100; spins++) {
        if (yasl_private_display_poll_event(&t->d.base, ev))
            return true;
        assert(yasl_loop_step(t->loop, 0) >= 0);
    }
    return false;
}

/* Writes `len` bytes into the pipe that the client reads, then closes the pipe.
 * The client reads only while the loop runs. So the write waits for the loop
 * each time the pipe is full. */
static void send_bytes(struct drag *t, int fd, const void *data, size_t len)
{
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    const uint8_t *bytes = data;
    size_t sent = 0;
    for (int spins = 0; sent < len; spins++) {
        assert(spins < 100000);
        ssize_t n = write(fd, bytes + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
        assert(yasl_loop_step(t->loop, 0) >= 0);
    }
    close(fd);
}

/* Fills the image payload. It starts with a PNG signature, which is not UTF-8,
 * and it holds NUL bytes further on. */
static void fill_image(void)
{
    static const uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    memcpy(image, signature, sizeof(signature));
    for (size_t i = sizeof(signature); i < sizeof(image); i++)
        image[i] = (uint8_t)((i * 2654435761u) >> 13);
}

/* A drag that offers no listed type is refused. It gets no hover and no event. */
static void test_unlisted_drag_is_refused(void)
{
    struct drag t;
    setup(&t);
    const char *const mimes[] = {"text/plain;charset=utf-8"};
    offer(&t, mimes, 1);
    enter(&t, 7);
    wire_pump(t.display);
    struct yasl_private_event ev;
    assert(!yasl_private_display_poll_event(&t.d.base, &ev)); /* No hover for a refused drag. */
    drop(&t);
    wire_pump(t.display);
    wire_read(t.server, &t.in);
    const char *mime = "unset";
    assert(accepted(&t.in, &mime) && mime == NULL);
    assert(t.in.nfds == 0); /* The client asked for no data. */
    assert(!wait_event(&t, &ev)); /* No DROP. */
    teardown(&t);
}

/* The surface ranks its types best first. The drag takes the best type that it
 * offers, even when the drag lists another type first. The image bytes reach
 * the application unchanged. */
static void test_image_drop_keeps_bytes(void)
{
    struct drag t;
    setup(&t);
    set_types(&t, image_types, 3);
    const char *const mimes[] = {"text/uri-list", "image/png"};
    offer(&t, mimes, 2);
    enter(&t, 9);
    wire_pump(t.display);
    expect_hover(&t, YASL_PRIVATE_DROP_OVER, "image/png", 3, 4);
    drop(&t);
    wire_pump(t.display);
    expect_hover(&t, YASL_PRIVATE_DROP_LEAVE, NULL, 3, 4);
    wire_read(t.server, &t.in);
    const char *mime = NULL;
    assert(accepted(&t.in, &mime) && strcmp(mime, "image/png") == 0);
    assert(received(&t.in, &mime) && strcmp(mime, "image/png") == 0);
    assert(t.in.nfds == 1);
    send_bytes(&t, t.in.fds[0], image, sizeof(image));
    struct yasl_private_event ev;
    assert(wait_event(&t, &ev));
    assert(ev.type == YASL_PRIVATE_EVENT_DROP && ev.surface == &t.w.base);
    assert(ev.text.pos.x == 3 && ev.text.pos.y == 4);
    assert(strcmp(ev.text.mime, "image/png") == 0);
    assert(ev.text.len == sizeof(image));
    assert(memcmp(ev.text.data, image, sizeof(image)) == 0);
    assert(!yasl_private_display_poll_event(&t.d.base, &ev));
    teardown(&t);
}

/* A listed text type is delivered with its own type. */
static void test_text_drop_delivers_text(void)
{
    struct drag t;
    setup(&t);
    set_types(&t, image_types, 3);
    const char *const mimes[] = {"text/plain;charset=utf-8"};
    offer(&t, mimes, 1);
    enter(&t, 10);
    wire_pump(t.display);
    expect_hover(&t, YASL_PRIVATE_DROP_OVER, "text/plain;charset=utf-8", 3, 4);
    motion(&t, 7, 9);
    wire_pump(t.display);
    expect_hover(&t, YASL_PRIVATE_DROP_OVER, "text/plain;charset=utf-8", 7, 9);
    drop(&t);
    wire_pump(t.display);
    expect_hover(&t, YASL_PRIVATE_DROP_LEAVE, NULL, 7, 9);
    wire_read(t.server, &t.in);
    const char *mime = NULL;
    assert(accepted(&t.in, &mime) && strcmp(mime, "text/plain;charset=utf-8") == 0);
    assert(received(&t.in, &mime) && strcmp(mime, "text/plain;charset=utf-8") == 0);
    assert(t.in.nfds == 1);
    static const char text[] = "caf\xc3\xa9\n";
    send_bytes(&t, t.in.fds[0], text, sizeof(text) - 1);
    struct yasl_private_event ev;
    assert(wait_event(&t, &ev));
    assert(ev.type == YASL_PRIVATE_EVENT_DROP && ev.surface == &t.w.base);
    assert(ev.text.pos.x == 7 && ev.text.pos.y == 9);
    assert(strcmp(ev.text.mime, "text/plain;charset=utf-8") == 0);
    assert(ev.text.len == sizeof(text) - 1);
    assert(memcmp(ev.text.data, text, ev.text.len) == 0);
    assert(!yasl_private_display_poll_event(&t.d.base, &ev));
    teardown(&t);
}

/* Without a call, a surface takes text/uri-list only, and the drop reports it. */
static void test_uri_list_drop_uses_default(void)
{
    struct drag t;
    setup(&t);
    const char *const mimes[] = {"text/uri-list"};
    offer(&t, mimes, 1);
    enter(&t, 8);
    wire_pump(t.display);
    expect_hover(&t, YASL_PRIVATE_DROP_OVER, "text/uri-list", 3, 4);
    drop(&t);
    wire_pump(t.display);
    expect_hover(&t, YASL_PRIVATE_DROP_LEAVE, NULL, 3, 4);
    wire_read(t.server, &t.in);
    const char *mime = NULL;
    assert(accepted(&t.in, &mime) && strcmp(mime, "text/uri-list") == 0);
    assert(received(&t.in, &mime) && strcmp(mime, "text/uri-list") == 0);
    assert(t.in.nfds == 1);
    static const char list[] = "file:///tmp/a.txt\r\n";
    send_bytes(&t, t.in.fds[0], list, sizeof(list) - 1);
    struct yasl_private_event ev;
    assert(wait_event(&t, &ev));
    assert(ev.type == YASL_PRIVATE_EVENT_DROP && ev.surface == &t.w.base);
    assert(ev.text.pos.x == 3 && ev.text.pos.y == 4);
    assert(strcmp(ev.text.mime, "text/uri-list") == 0);
    assert(ev.text.len == sizeof(list) - 1);
    assert(memcmp(ev.text.data, list, ev.text.len) == 0);
    assert(!yasl_private_display_poll_event(&t.d.base, &ev));
    teardown(&t);
}

int main(void)
{
    fill_image();
    assert(memchr(image, 0, sizeof(image)));
    test_unlisted_drag_is_refused();
    test_image_drop_keeps_bytes();
    test_text_drop_delivers_text();
    test_uri_list_drop_uses_default();
    return 0;
}
