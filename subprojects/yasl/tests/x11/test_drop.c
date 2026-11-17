/* Drops from an X11 source onto a YASL window, through the public API.
 * A synthetic XDND source runs on a second connection to the same X server.
 * The test exits with 77 when no X server is available. */
#include <yasl/yasl.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP_STATUS 77
/* Payloads above this size go out as INCR chunks of CHUNK_SIZE bytes. */
#define INCR_THRESHOLD (100u * 1024u)
#define CHUNK_SIZE (64u * 1024u)
#define PAYLOAD_SIZE (600u * 1024u)
#define OVERSIZE_LIMIT (64u * 1024u * 1024u)
#define WAIT_NS (60ull * 1000000000ull)
#define MAX_RECORDS 8

struct drop_record {
    unsigned action;
    char mime[YASL_DROP_MIME_MAX];
    size_t length;
    char *bytes;
};

struct drop_log {
    struct drop_record records[MAX_RECORDS];
    size_t count;
};

/* The synthetic XDND source. It answers XdndStatus, XdndFinished and
 * SelectionRequest, and it writes INCR chunks on each property deletion. */
struct source {
    Display *dpy;
    Window window, target;
    Atom enter, position, drop, status, finished, selection, action_copy, type_list, incr;
    bool status_seen, accepted, finished_seen, finished_ok;
    Atom served_type;
    const unsigned char *data;
    size_t length;
    bool incr_active, incr_finishing;
    Window requestor;
    Atom property;
    size_t offset;
};

struct harness {
    struct source src;
    struct drop_log log;
};

static Atom intern(Display *dpy, const char *name)
{
    return XInternAtom(dpy, name, False);
}

static void send_xdnd(struct source *src, Atom type, long l1, long l2, long l3, long l4)
{
    XEvent ev = {0};
    ev.xclient = (XClientMessageEvent){
        .type = ClientMessage, .display = src->dpy, .window = src->target,
        .message_type = type, .format = 32,
        .data.l = {(long)src->window, l1, l2, l3, l4},
    };
    XSendEvent(src->dpy, src->target, False, NoEventMask, &ev);
    XFlush(src->dpy);
}

/* Starts a drag that offers the named types. More than three types go in
 * XdndTypeList, and the Enter message sets its more-types bit. */
static void send_enter(struct source *src, const Atom *types, size_t count)
{
    long flags = 5L << 24;
    long inline_types[3] = {0, 0, 0};
    if (count > 3) {
        flags |= 1;
        XChangeProperty(src->dpy, src->window, src->type_list, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)types, (int)count);
    } else {
        for (size_t i = 0; i < count; i++) inline_types[i] = (long)types[i];
    }
    send_xdnd(src, src->enter, flags, inline_types[0], inline_types[1], inline_types[2]);
}

/* Moves the pointer to 10,10 inside the target window, in root coordinates. */
static void send_position(struct source *src)
{
    int root_x = 0, root_y = 0;
    Window child = None;
    XTranslateCoordinates(src->dpy, src->target, DefaultRootWindow(src->dpy), 10, 10,
                          &root_x, &root_y, &child);
    long packed = ((long)(root_x & 0xffff) << 16) | (long)(root_y & 0xffff);
    send_xdnd(src, src->position, 0, packed, CurrentTime, (long)src->action_copy);
}

static void send_drop(struct source *src)
{
    send_xdnd(src, src->drop, 0, CurrentTime, 0, 0);
}

static void serve_request(struct source *src, const XSelectionRequestEvent *request)
{
    XEvent reply = {0};
    reply.xselection = (XSelectionEvent){
        .type = SelectionNotify, .display = src->dpy,
        .requestor = request->requestor, .selection = request->selection,
        .target = request->target, .time = request->time, .property = None,
    };
    if (request->selection == src->selection && request->target == src->served_type &&
            request->property) {
        reply.xselection.property = request->property;
        if (src->length > INCR_THRESHOLD) {
            unsigned long total = (unsigned long)src->length;
            XSelectInput(src->dpy, request->requestor, PropertyChangeMask);
            XChangeProperty(src->dpy, request->requestor, request->property, src->incr, 32,
                            PropModeReplace, (const unsigned char *)&total, 1);
            src->incr_active = true;
            src->incr_finishing = false;
            src->requestor = request->requestor;
            src->property = request->property;
            src->offset = 0;
        } else {
            XChangeProperty(src->dpy, request->requestor, request->property, src->served_type, 8,
                            PropModeReplace, src->data, (int)src->length);
        }
    }
    XSendEvent(src->dpy, request->requestor, False, NoEventMask, &reply);
    XFlush(src->dpy);
}

/* Writes the next INCR chunk each time the requestor deletes the property.
 * A zero-length chunk ends the transfer. */
static void next_chunk(struct source *src, const XPropertyEvent *event)
{
    if (!src->incr_active || event->state != PropertyDelete ||
            event->window != src->requestor || event->atom != src->property)
        return;
    if (src->incr_finishing) {
        src->incr_active = false;
        return;
    }
    size_t left = src->length - src->offset;
    size_t n = left < CHUNK_SIZE ? left : CHUNK_SIZE;
    XChangeProperty(src->dpy, src->requestor, src->property, src->served_type, 8,
                    PropModeReplace, src->data + src->offset, (int)n);
    src->offset += n;
    if (n == 0) src->incr_finishing = true;
    XFlush(src->dpy);
}

static void source_pump(struct source *src)
{
    while (XPending(src->dpy)) {
        XEvent ev;
        XNextEvent(src->dpy, &ev);
        switch (ev.type) {
        case ClientMessage:
            if (ev.xclient.message_type == src->status) {
                src->status_seen = true;
                src->accepted = (ev.xclient.data.l[1] & 1) != 0;
            } else if (ev.xclient.message_type == src->finished) {
                src->finished_seen = true;
                src->finished_ok = (ev.xclient.data.l[1] & 1) != 0;
                src->incr_active = false;
            }
            break;
        case SelectionRequest:
            serve_request(src, &ev.xselectionrequest);
            break;
        case PropertyNotify:
            next_chunk(src, &ev.xproperty);
            break;
        default:
            break;
        }
    }
}

static void log_clear(struct drop_log *log)
{
    for (size_t i = 0; i < log->count; i++) free(log->records[i].bytes);
    memset(log, 0, sizeof(*log));
}

static void record(struct drop_log *log, const yasl_event *ev)
{
    if (ev->type != YASL_EVENT_DROP) return;
    assert(log->count < MAX_RECORDS);
    struct drop_record *r = &log->records[log->count++];
    r->action = ev->action;
    if (ev->action == YASL_DROP_OVER || ev->action == YASL_DROP_RELEASE)
        memcpy(r->mime, ev->drop.mime, sizeof(r->mime));
    if (ev->text && ev->length) {
        r->bytes = malloc(ev->length);
        assert(r->bytes);
        memcpy(r->bytes, ev->text, ev->length);
        r->length = ev->length;
    }
}

static const struct drop_record *find_action(const struct drop_log *log, enum yasl_drop_action action)
{
    for (size_t i = 0; i < log->count; i++)
        if (log->records[i].action == action) return &log->records[i];
    return NULL;
}

/* Reads the events that are already queued. Text is copied, since it is valid
 * only until the next wait. */
static void drain(struct harness *h)
{
    yasl_event ev;
    while (yasl_wait_event(&ev, 0) == 1) record(&h->log, &ev);
}

static bool status_ready(const struct harness *h) { return h->src.status_seen; }
static bool finished_ready(const struct harness *h) { return h->src.finished_seen; }

/* Serves the source and reads yasl events until ready() holds. */
static void wait_until(struct harness *h, bool (*ready)(const struct harness *), const char *what)
{
    uint64_t deadline = yasl_time_ns() + WAIT_NS;
    while (!ready(h)) {
        if (yasl_time_ns() >= deadline) {
            fprintf(stderr, "timed out waiting for %s\n", what);
            abort();
        }
        source_pump(&h->src);
        yasl_event ev;
        int got = yasl_wait_event(&ev, 1);
        assert(got >= 0);
        if (got == 1) record(&h->log, &ev);
    }
}

/* Starts a drag over the window with the named types, then waits for XdndStatus. */
static void begin_drag(struct harness *h, const char *const *names, size_t count)
{
    Atom atoms[8];
    assert(count <= 8);
    for (size_t i = 0; i < count; i++) atoms[i] = intern(h->src.dpy, names[i]);
    h->src.status_seen = h->src.accepted = false;
    h->src.finished_seen = h->src.finished_ok = false;
    log_clear(&h->log);
    send_enter(&h->src, atoms, count);
    send_position(&h->src);
    wait_until(h, status_ready, "XdndStatus");
    drain(h);
}

/* Drops the drag, then waits for XdndFinished. */
static void finish_drag(struct harness *h)
{
    send_drop(&h->src);
    wait_until(h, finished_ready, "XdndFinished");
    drain(h);
}

/* Sets the bytes and the type that the next drop delivers. */
static void offer(struct harness *h, const char *type, const void *data, size_t length)
{
    h->src.served_type = intern(h->src.dpy, type);
    h->src.data = data;
    h->src.length = length;
}

static void fill(unsigned char *data, size_t length, unsigned salt)
{
    /* Yields many NUL bytes and bytes above 0x7f, which are not valid UTF-8. */
    for (size_t i = 0; i < length; i++)
        data[i] = (unsigned char)(i * 131u + (i >> 8) * 7u + salt);
}

static void check_refused(struct harness *h, yasl_surface *surface)
{
    /* Without a call, a surface takes text/uri-list only. */
    assert(yasl_surface_set_drop_types(surface, NULL, 0) == 0);
    const char *offered[] = {"UTF8_STRING"};
    begin_drag(h, offered, 1);
    assert(!h->src.accepted);
    assert(!find_action(&h->log, YASL_DROP_OVER));
    finish_drag(h);
    assert(h->src.finished_seen && !h->src.finished_ok);
    assert(!find_action(&h->log, YASL_DROP_RELEASE));
}

static void check_incr_image(struct harness *h, yasl_surface *surface)
{
    /* The app prefers the image types over text/uri-list. A payload above the
     * INCR threshold arrives in chunks, with NUL bytes and bytes that are not UTF-8. */
    const char *types[] = {"image/*", "text/uri-list", "UTF8_STRING"};
    assert(yasl_surface_set_drop_types(surface, types, 3) == 0);
    unsigned char *payload = malloc(PAYLOAD_SIZE);
    assert(payload);
    fill(payload, PAYLOAD_SIZE, 1);
    offer(h, "image/png", payload, PAYLOAD_SIZE);
    const char *offered[] = {"text/uri-list", "image/png"};
    begin_drag(h, offered, 2);
    assert(h->src.accepted);
    assert(find_action(&h->log, YASL_DROP_OVER) &&
           !strcmp(find_action(&h->log, YASL_DROP_OVER)->mime, "image/png"));
    finish_drag(h);
    assert(h->src.finished_ok);
    const struct drop_record *release = find_action(&h->log, YASL_DROP_RELEASE);
    assert(release && !strcmp(release->mime, "image/png"));
    assert(release->length == PAYLOAD_SIZE && release->bytes);
    assert(memcmp(release->bytes, payload, PAYLOAD_SIZE) == 0);
    free(payload);
}

static void check_text(struct harness *h, yasl_surface *surface)
{
    const char *types[] = {"UTF8_STRING"};
    assert(yasl_surface_set_drop_types(surface, types, 1) == 0);
    const char text[] = "caf\xc3\xa9 \xe2\x82\xac";
    offer(h, "UTF8_STRING", text, sizeof(text) - 1);
    const char *offered[] = {"UTF8_STRING"};
    begin_drag(h, offered, 1);
    assert(h->src.accepted);
    finish_drag(h);
    const struct drop_record *release = find_action(&h->log, YASL_DROP_RELEASE);
    assert(h->src.finished_ok && release && !strcmp(release->mime, "UTF8_STRING"));
    assert(release->length == sizeof(text) - 1 && !memcmp(release->bytes, text, release->length));
}

static void check_type_list(struct harness *h, yasl_surface *surface)
{
    /* Five types need XdndTypeList. The first image type that the source offers wins. */
    const char *types[] = {"image/*", "UTF8_STRING"};
    assert(yasl_surface_set_drop_types(surface, types, 2) == 0);
    const unsigned char bytes[] = {0xff, 0xd8, 0x00, 0xff, 'x'};
    offer(h, "image/jpeg", bytes, sizeof(bytes));
    const char *offered[] = {"text/html", "text/plain", "image/jpeg", "image/png", "UTF8_STRING"};
    begin_drag(h, offered, 5);
    assert(h->src.accepted);
    assert(find_action(&h->log, YASL_DROP_OVER) &&
           !strcmp(find_action(&h->log, YASL_DROP_OVER)->mime, "image/jpeg"));
    finish_drag(h);
    const struct drop_record *release = find_action(&h->log, YASL_DROP_RELEASE);
    assert(h->src.finished_ok && release && !strcmp(release->mime, "image/jpeg"));
    assert(release->length == sizeof(bytes) && !memcmp(release->bytes, bytes, sizeof(bytes)));
}

static void check_invalid_text(struct harness *h, yasl_surface *surface)
{
    /* A text drop must be valid UTF-8. Invalid bytes fail the drop. */
    const char *types[] = {"UTF8_STRING"};
    assert(yasl_surface_set_drop_types(surface, types, 1) == 0);
    const char bad[] = {(char)0xff, 'x'};
    offer(h, "UTF8_STRING", bad, sizeof(bad));
    const char *offered[] = {"UTF8_STRING"};
    begin_drag(h, offered, 1);
    assert(h->src.accepted);
    finish_drag(h);
    assert(h->src.finished_seen && !h->src.finished_ok);
    assert(!find_action(&h->log, YASL_DROP_RELEASE));
}

static void check_large(struct harness *h, yasl_surface *surface)
{
    /* A drop of 20 MiB is under the 64 MiB cap, so it arrives whole. */
    const char *types[] = {"image/*"};
    assert(yasl_surface_set_drop_types(surface, types, 1) == 0);
    size_t length = 20u * 1024u * 1024u;
    unsigned char *payload = malloc(length);
    assert(payload);
    fill(payload, length, 3);
    offer(h, "image/png", payload, length);
    const char *offered[] = {"image/png"};
    begin_drag(h, offered, 1);
    assert(h->src.accepted);
    finish_drag(h);
    const struct drop_record *release = find_action(&h->log, YASL_DROP_RELEASE);
    assert(h->src.finished_ok && release && release->length == length);
    assert(memcmp(release->bytes, payload, length) == 0);
    free(payload);
}

static void check_oversize(struct harness *h, yasl_surface *surface)
{
    /* A drop above the 64 MiB cap fails. */
    const char *types[] = {"image/*"};
    assert(yasl_surface_set_drop_types(surface, types, 1) == 0);
    size_t length = (size_t)OVERSIZE_LIMIT + 1;
    unsigned char *payload = malloc(length);
    assert(payload);
    fill(payload, length, 2);
    offer(h, "image/png", payload, length);
    const char *offered[] = {"image/png"};
    begin_drag(h, offered, 1);
    assert(h->src.accepted);
    finish_drag(h);
    assert(h->src.finished_seen && !h->src.finished_ok);
    assert(!find_action(&h->log, YASL_DROP_RELEASE));
    free(payload);
}

int main(void)
{
    Display *probe = XOpenDisplay(NULL);
    if (!probe) {
        fprintf(stderr, "SKIP: no X server\n");
        return SKIP_STATUS;
    }
    XCloseDisplay(probe);

    struct yasl_options options = {.backend = YASL_BACKEND_X11};
    assert(yasl_init(&options) == 0);
    struct yasl_surface_options window = {
        .title = "Drop target", .width = 200, .height = 150, .type = YASL_SURFACE_CPU,
    };
    yasl_surface *surface = yasl_surface_create(&window);
    assert(surface);
    struct yasl_native native = {0};
    assert(yasl_surface_get_native(surface, &native) == 0 && native.x11_window);

    struct harness h = {0};
    h.src.dpy = XOpenDisplay(NULL);
    assert(h.src.dpy);
    h.src.target = (Window)native.x11_window;
    h.src.window = XCreateSimpleWindow(h.src.dpy, DefaultRootWindow(h.src.dpy), 0, 0, 1, 1, 0, 0, 0);
    h.src.enter = intern(h.src.dpy, "XdndEnter");
    h.src.position = intern(h.src.dpy, "XdndPosition");
    h.src.drop = intern(h.src.dpy, "XdndDrop");
    h.src.status = intern(h.src.dpy, "XdndStatus");
    h.src.finished = intern(h.src.dpy, "XdndFinished");
    h.src.selection = intern(h.src.dpy, "XdndSelection");
    h.src.action_copy = intern(h.src.dpy, "XdndActionCopy");
    h.src.type_list = intern(h.src.dpy, "XdndTypeList");
    h.src.incr = intern(h.src.dpy, "INCR");
    XSetSelectionOwner(h.src.dpy, h.src.selection, h.src.window, CurrentTime);
    XSync(h.src.dpy, False);

    check_refused(&h, surface);
    check_incr_image(&h, surface);
    check_text(&h, surface);
    check_type_list(&h, surface);
    check_invalid_text(&h, surface);
    check_large(&h, surface);
    check_oversize(&h, surface);

    log_clear(&h.log);
    XDestroyWindow(h.src.dpy, h.src.window);
    XCloseDisplay(h.src.dpy);
    assert(yasl_surface_destroy(surface) == 0);
    assert(yasl_shutdown() == 0);
    return 0;
}
