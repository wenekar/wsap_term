#define _GNU_SOURCE
#include "internal.h"
#include "clipboard.h"
#include "common/utf8.h"
#include <errno.h>

#define CLIPBOARD_TEXT_MAX (256u * 1024)
#define CLIPBOARD_TRANSFER_TIMEOUT_NS (30ull * 1000000000ull)

struct offer {
    struct offer *next;
    struct wl_data_offer *proxy;
    char *mime;
    bool uri_list;
    bool tab;
};
struct primary_offer {
    struct primary_offer *next;
    struct zwp_primary_selection_offer_v1 *proxy;
    char *mime;
};
struct transfer {
    struct transfer *next;
    struct clipboard *owner;
    struct offer *drop;
    struct wl_surface *drop_surface;
    int fd, id;
    yasl_private_timer_id timeout;
    char *text;
    size_t len, offset, capacity;
    uint64_t request_id;
    bool sending;
    bool uri_list;
    bool paste;
    bool cancelled;
};
struct clipboard {
    struct wl_display *display;
    struct wl_data_device_manager *manager;
    struct wl_data_device *device;
    struct zwp_primary_selection_device_manager_v1 *primary_manager;
    struct zwp_primary_selection_device_v1 *primary_device;
    struct zwp_primary_selection_source_v1 *primary_source;
    struct wl_seat *seat;
    struct wl_data_source *source;
    struct offer *selection;
    struct offer *pending_offers;
    struct offer *drag_offer;
    struct primary_offer *primary_selection;
    struct primary_offer *primary_pending_offers;
    struct yasl_loop *loop;
    struct transfer *transfers;
    char *text;
    char *primary_text;
    clipboard_receive_fn receive;
    void *ud;
    clipboard_drag_fn drag;
    void *drag_ud;
    clipboard_drop_fn drop;
    void *drop_ud;
    wl_fixed_t drag_x, drag_y;
    struct wl_surface *drag_surface;
};

const char *clipboard_drag_mime(void)
{
    static char mime[64];
    if (!mime[0])
        snprintf(mime, sizeof(mime), "application/x-yasl-tab;pid=%ld", (long)getpid());
    return mime;
}

static void drag_event(struct clipboard *c, enum yasl_private_drag_event ev,
                       wl_fixed_t x, wl_fixed_t y)
{
    if (c->drag)
        c->drag(ev, c->drag_surface, wl_fixed_to_double(x), wl_fixed_to_double(y),
                c->drag_ud);
}

static void drop_event(struct clipboard *c, enum yasl_private_drop_event ev)
{
    if (c->drop && c->drag_surface)
        c->drop(ev, c->drag_surface, wl_fixed_to_double(c->drag_x),
                wl_fixed_to_double(c->drag_y), c->drop_ud);
}

static void free_offer(struct offer *o);

static void transfer_end(struct transfer *t, bool complete);

static uint64_t transfer_timeout(void *ud, yasl_private_timer_id id, uint64_t interval)
{
    (void)interval;
    struct transfer *t = ud;
    if (t->timeout != id)
        return 0;
    t->timeout = 0;
    transfer_end(t, false);
    return 0;
}

static void transfer_end(struct transfer *t, bool complete)
{
    struct clipboard *owner = t->owner;
    struct transfer **p = &owner->transfers;
    while (*p && *p != t) p = &(*p)->next;
    if (*p) *p = t->next;
    yasl_loop_del_fd(owner->loop, t->id);
    if (t->timeout)
        yasl_loop_del_timer(owner->loop, t->timeout);
    close(t->fd);
    if (t->drop) {
        if (complete && wl_data_offer_get_version(t->drop->proxy) >=
                WL_DATA_OFFER_FINISH_SINCE_VERSION)
            wl_data_offer_finish(t->drop->proxy);
        free_offer(t->drop);
        wl_display_flush(owner->display);
    }
    bool notify = owner->receive && !t->cancelled &&
            ((!t->sending && complete) || (t->paste && !complete));
    clipboard_receive_fn receive = owner->receive;
    void *receive_ud = owner->ud;
    struct wl_surface *surface = t->drop_surface;
    uint64_t request_id = t->request_id;
    bool uri_list = t->uri_list;
    enum yasl_private_transfer_status status = complete ? YASL_PRIVATE_TRANSFER_OK : YASL_PRIVATE_TRANSFER_FAILED;
    const char *text = complete ? (t->text ? t->text : "") : NULL;
    size_t len = complete ? t->len : 0;
    if (notify)
        receive(text, len, uri_list, surface, request_id, status, receive_ud);
    free(t->text);
    free(t);
}

static void transfer_io(void *ud, int fd, short events)
{
    struct transfer *t = ud;
    if (events & (POLLERR | POLLNVAL)) { transfer_end(t, false); return; }
    if (t->sending) {
        ssize_t n = write(fd, t->text + t->offset, t->len - t->offset);
        if (n > 0) t->offset += (size_t)n;
        if (t->offset == t->len || n == 0 ||
            (n < 0 && errno != EAGAIN && errno != EINTR)) transfer_end(t, false);
    } else {
        if (t->capacity - t->len < 4096) {
            size_t capacity = t->capacity ? t->capacity * 2 : 8192;
            if (capacity > CLIPBOARD_TEXT_MAX) capacity = CLIPBOARD_TEXT_MAX;
            if (capacity <= t->len) { transfer_end(t, false); return; }
            char *text = realloc(t->text, capacity);
            if (!text) { transfer_end(t, false); return; }
            t->text = text;
            t->capacity = capacity;
        }
        ssize_t n = read(fd, t->text + t->len, t->capacity - t->len);
        if (n > 0) {
            t->len += (size_t)n;
        } else if (n == 0) {
            bool valid = yasl_private_utf8_validate(t->text, t->len);
            transfer_end(t, valid);
        } else if (errno != EAGAIN && errno != EINTR) transfer_end(t, false);
    }
}

static struct transfer *start_transfer_text(struct clipboard *c, int fd, bool sending,
                                            bool uri_list, const char *send_text)
{
    struct transfer *t = calloc(1, sizeof(*t));
    if (!t) { close(fd); return NULL; }
    t->owner = c; t->fd = fd; t->sending = sending; t->uri_list = uri_list;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (sending) {
        t->text = strdup(send_text ? send_text : "");
        if (!t->text) { close(fd); free(t); return NULL; }
        t->len = strlen(t->text);
    }
    t->id = yasl_loop_add_fd(c->loop, fd, sending ? POLLOUT : POLLIN, transfer_io, t);
    if (t->id <= 0) { close(fd); free(t->text); free(t); return NULL; }
    t->timeout = yasl_loop_add_timer(c->loop, CLIPBOARD_TRANSFER_TIMEOUT_NS,
                                     transfer_timeout, t);
    if (!t->timeout) {
        yasl_loop_del_fd(c->loop, t->id);
        close(fd);
        free(t->text);
        free(t);
        return NULL;
    }
    t->next = c->transfers; c->transfers = t;
    return t;
}

static struct transfer *start_transfer_paste(struct clipboard *c, int fd,
        struct wl_surface *target, uint64_t request_id)
{
    struct transfer *t = start_transfer_text(c, fd, false, false, NULL);
    if (t) {
        t->drop_surface = target;
        t->request_id = request_id;
        t->paste = true;
    }
    return t;
}

static const char *match_text_mime(const char *mime)
{
    if (unlikely(!mime)) return NULL;
    if (likely(!strncasecmp(mime, "text/plain", 10))) {
        if (!strcasecmp(mime, "text/plain;charset=utf-8")) return "text/plain;charset=utf-8";
        if (!strcasecmp(mime, "text/plain;charset=utf8")) return "text/plain;charset=utf8";
        if (!strcasecmp(mime, "text/plain")) return "text/plain";
    }
    if (!strcasecmp(mime, "UTF8_STRING")) return "UTF8_STRING";
    if (!strcasecmp(mime, "STRING")) return "STRING";
    if (!strcasecmp(mime, "TEXT")) return "TEXT";
    return NULL;
}

static void offer_keep_mime(char **slot, const char *mime)
{
    char *copy = strdup(mime);
    if (!copy) return;
    free(*slot);
    *slot = copy;
}

static void offer_mime(void *ud, struct wl_data_offer *proxy, const char *mime)
{
    (void)proxy;
    struct offer *o = ud;
    if (!strcmp(mime, clipboard_drag_mime())) o->tab = true;
    if (!strcmp(mime, "text/uri-list")) o->uri_list = true;
    const char *match = match_text_mime(mime);
    if (match) {
        if (!o->mime || !strcasecmp(match, "text/plain;charset=utf-8"))
            offer_keep_mime(&o->mime, mime);
    }
}
static void offer_source_actions(void *ud, struct wl_data_offer *proxy, uint32_t actions)
{ (void)ud; (void)proxy; (void)actions; }
static void offer_action(void *ud, struct wl_data_offer *proxy, uint32_t action)
{ (void)ud; (void)proxy; (void)action; }
static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime,
    .source_actions = offer_source_actions,
    .action = offer_action,
};
static void free_offer(struct offer *o)
{
    if (o) { wl_data_offer_destroy(o->proxy); free(o->mime); free(o); }
}
static void take_pending_offer(struct clipboard *c, struct offer *o)
{
    if (!o) return;
    struct offer **p = &c->pending_offers;
    while (*p && *p != o) p = &(*p)->next;
    if (*p) *p = o->next;
    o->next = NULL;
}
static void clear_pending_offers(struct clipboard *c)
{
    while (c->pending_offers) {
        struct offer *o = c->pending_offers;
        c->pending_offers = o->next;
        free_offer(o);
    }
}
static void data_offer(void *ud, struct wl_data_device *device, struct wl_data_offer *proxy)
{
    (void)device;
    struct clipboard *c = ud;
    struct offer *o = calloc(1, sizeof(*o));
    if (!o) { wl_data_offer_destroy(proxy); return; }
    o->proxy = proxy;
    wl_data_offer_add_listener(proxy, &offer_listener, o);
    o->next = c->pending_offers;
    c->pending_offers = o;
}
static void selection(void *ud, struct wl_data_device *device, struct wl_data_offer *proxy)
{
    (void)device;
    struct clipboard *c = ud;
    struct offer *o = proxy ? wl_data_offer_get_user_data(proxy) : NULL;
    take_pending_offer(c, o);
    if (c->drag_offer == o) c->drag_offer = NULL;
    if (c->selection != o) free_offer(c->selection);
    c->selection = o;
}
static void drag_offer_enter(void *ud, struct wl_data_device *device, uint32_t serial,
                       struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *proxy)
{
    (void)device;
    if (proxy) {
        struct clipboard *c = ud;
        struct offer *o = wl_data_offer_get_user_data(proxy);
        take_pending_offer(c, o);
        if (c->selection == o) c->selection = NULL;
        if (c->drag_offer && c->drag_offer != o) free_offer(c->drag_offer);
        c->drag_offer = o;
        const char *mime = o && o->tab ? clipboard_drag_mime()
                         : o && o->uri_list ? "text/uri-list"
                         : o ? o->mime : NULL;
        wl_data_offer_accept(proxy, serial, mime);
        if (mime && wl_data_offer_get_version(proxy) >= 3) {
            uint32_t action = o->tab ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
                                     : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
            wl_data_offer_set_actions(proxy, action, action);
        }
        c->drag_x = x;
        c->drag_y = y;
        c->drag_surface = surface;
        if (o && o->tab)
            drag_event(c, YASL_PRIVATE_DRAG_OVER, x, y);
        else if (o && !o->tab && (o->uri_list || o->mime))
            drop_event(c, YASL_PRIVATE_DROP_OVER);
    }
}
static void drag_offer_clear(void *ud, struct wl_data_device *device)
{
    (void)device;
    struct clipboard *c = ud;
    if (c->drag_offer && c->drag_offer->tab)
        drag_event(c, YASL_PRIVATE_DRAG_LEAVE, c->drag_x, c->drag_y);
    else if (c->drag_offer && !c->drag_offer->tab &&
             (c->drag_offer->uri_list || c->drag_offer->mime))
        drop_event(c, YASL_PRIVATE_DROP_LEAVE);
    free_offer(c->drag_offer);
    c->drag_offer = NULL;
    c->drag_surface = NULL;
}
static void drag_motion(void *ud, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)device; (void)time;
    struct clipboard *c = ud;
    c->drag_x = x;
    c->drag_y = y;
    if (c->drag_offer && c->drag_offer->tab)
        drag_event(c, YASL_PRIVATE_DRAG_OVER, x, y);
    else if (c->drag_offer && !c->drag_offer->tab &&
             (c->drag_offer->uri_list || c->drag_offer->mime))
        drop_event(c, YASL_PRIVATE_DROP_OVER);
}
static void drag_drop(void *ud, struct wl_data_device *device)
{
    (void)device;
    struct clipboard *c = ud;
    struct offer *o = c->drag_offer;
    if (!o) return;
    c->drag_offer = NULL;
    bool v3 = wl_data_offer_get_version(o->proxy) >= 3;
    if (o->tab) {
        drag_event(c, YASL_PRIVATE_DRAG_DROP, c->drag_x, c->drag_y);
        if (v3)
            wl_data_offer_finish(o->proxy);
    } else if (o && !o->tab && (o->uri_list || o->mime)) {
        drop_event(c, YASL_PRIVATE_DROP_LEAVE);
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) == 0) {
            wl_data_offer_receive(o->proxy, o->uri_list ? "text/uri-list" : o->mime, fds[1]);
            close(fds[1]);
            struct transfer *t = start_transfer_text(c, fds[0], false, o->uri_list, c->text);
            if (t) {
                t->drop = o;
                t->drop_surface = c->drag_surface;
                o = NULL;
            }
            wl_display_flush(c->display);
        }
    }
    c->drag_surface = NULL;
    free_offer(o);
}
static const struct wl_data_device_listener device_listener = {
    .data_offer = data_offer, .selection = selection, .enter = drag_offer_enter,
    .leave = drag_offer_clear, .motion = drag_motion, .drop = drag_drop
};
static void source_target(void *ud, struct wl_data_source *s, const char *mime) { (void)ud; (void)s; (void)mime; }
static void source_send(void *ud, struct wl_data_source *s, const char *mime, int32_t fd)
{
    struct clipboard *c = ud;
    (void)s; (void)mime;
    start_transfer_text(c, fd, true, false, c->text);
}
static void source_cancelled(void *ud, struct wl_data_source *s)
{
    struct clipboard *c = ud;
    if (c->source == s) c->source = NULL;
    wl_data_source_destroy(s);
}
static void source_dnd_drop_performed(void *ud, struct wl_data_source *s) { (void)ud; (void)s; }
static void source_dnd_finished(void *ud, struct wl_data_source *s) { (void)ud; (void)s; }
static void source_action(void *ud, struct wl_data_source *s, uint32_t action) { (void)ud; (void)s; (void)action; }
static const struct wl_data_source_listener source_listener = {
    .target = source_target, .send = source_send, .cancelled = source_cancelled,
    .dnd_drop_performed = source_dnd_drop_performed,
    .dnd_finished = source_dnd_finished, .action = source_action,
};

static void free_primary_offer(struct primary_offer *o)
{
    if (o) { zwp_primary_selection_offer_v1_destroy(o->proxy); free(o->mime); free(o); }
}

static void take_pending_primary_offer(struct clipboard *c, struct primary_offer *o)
{
    if (!o) return;
    struct primary_offer **p = &c->primary_pending_offers;
    while (*p && *p != o) p = &(*p)->next;
    if (*p) *p = o->next;
    o->next = NULL;
}

static void clear_pending_primary_offers(struct clipboard *c)
{
    while (c->primary_pending_offers) {
        struct primary_offer *o = c->primary_pending_offers;
        c->primary_pending_offers = o->next;
        free_primary_offer(o);
    }
}

static void primary_offer_mime(void *ud, struct zwp_primary_selection_offer_v1 *proxy, const char *mime)
{
    (void)proxy;
    struct primary_offer *o = ud;
    const char *match = match_text_mime(mime);
    if (match) {
        if (!o->mime || !strcasecmp(match, "text/plain;charset=utf-8"))
            offer_keep_mime(&o->mime, mime);
    }
}

static const struct zwp_primary_selection_offer_v1_listener primary_offer_listener = {
    .offer = primary_offer_mime,
};

static void primary_data_offer(void *ud, struct zwp_primary_selection_device_v1 *device,
                               struct zwp_primary_selection_offer_v1 *proxy)
{
    (void)device;
    struct clipboard *c = ud;
    struct primary_offer *o = calloc(1, sizeof(*o));
    if (!o) { zwp_primary_selection_offer_v1_destroy(proxy); return; }
    o->proxy = proxy;
    zwp_primary_selection_offer_v1_add_listener(proxy, &primary_offer_listener, o);
    o->next = c->primary_pending_offers;
    c->primary_pending_offers = o;
}

static void primary_selection(void *ud, struct zwp_primary_selection_device_v1 *device,
                              struct zwp_primary_selection_offer_v1 *proxy)
{
    (void)device;
    struct clipboard *c = ud;
    struct primary_offer *o = proxy ? zwp_primary_selection_offer_v1_get_user_data(proxy) : NULL;
    take_pending_primary_offer(c, o);
    if (c->primary_selection != o) free_primary_offer(c->primary_selection);
    c->primary_selection = o;
}

static const struct zwp_primary_selection_device_v1_listener primary_device_listener = {
    .data_offer = primary_data_offer,
    .selection = primary_selection,
};

static void primary_source_send(void *ud, struct zwp_primary_selection_source_v1 *s,
                                const char *mime, int32_t fd)
{
    (void)s; (void)mime;
    struct clipboard *c = ud;
    start_transfer_text(c, fd, true, false, c->primary_text);
}

static void primary_source_cancelled(void *ud, struct zwp_primary_selection_source_v1 *s)
{
    struct clipboard *c = ud;
    if (c->primary_source == s) c->primary_source = NULL;
    zwp_primary_selection_source_v1_destroy(s);
}

static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
    .send = primary_source_send,
    .cancelled = primary_source_cancelled,
};

void clipboard_set_primary_manager(struct clipboard *c,
                                   struct zwp_primary_selection_device_manager_v1 *mgr)
{
    if (!c) return;
    c->primary_manager = mgr;
    if (c->primary_device) {
        zwp_primary_selection_device_v1_destroy(c->primary_device);
        c->primary_device = NULL;
    }
    if (c->primary_manager && c->seat) {
        c->primary_device =
            zwp_primary_selection_device_manager_v1_get_device(c->primary_manager, c->seat);
        if (c->primary_device)
            zwp_primary_selection_device_v1_add_listener(c->primary_device,
                                                         &primary_device_listener, c);
    }
}

void clipboard_set_drag_handler(struct clipboard *c, clipboard_drag_fn fn, void *ud)
{
    if (!c) return;
    c->drag = fn;
    c->drag_ud = ud;
}

void clipboard_set_drop_handler(struct clipboard *c, clipboard_drop_fn fn, void *ud)
{
    if (!c) return;
    c->drop = fn;
    c->drop_ud = ud;
}

struct wl_data_device *clipboard_data_device(const struct clipboard *c)
{
    return c ? c->device : NULL;
}

struct clipboard *clipboard_create(struct wl_display *display, struct wl_data_device_manager *manager,
                                  struct wl_seat *seat, struct yasl_loop *loop,
                                  clipboard_receive_fn receive, void *ud)
{
    if (!manager || !seat) return NULL;
    struct clipboard *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->display = display; c->manager = manager; c->loop = loop; c->receive = receive; c->ud = ud;
    c->device = wl_data_device_manager_get_data_device(manager, seat);
    if (!c->device) { free(c); return NULL; }
    c->seat = seat;
    wl_data_device_add_listener(c->device, &device_listener, c);
    return c;
}

bool clipboard_set_seat(struct clipboard *c, struct wl_seat *seat)
{
    if (!c || c->seat == seat) return false;
    c->seat = seat;
    drag_offer_clear(c, c->device);
    if (c->device) {
        wl_data_device_destroy(c->device);
        c->device = NULL;
    }
    free_offer(c->selection);
    c->selection = NULL;
    clear_pending_offers(c);
    if (c->source) {
        wl_data_source_destroy(c->source);
        c->source = NULL;
    }
    if (c->primary_device) {
        zwp_primary_selection_device_v1_destroy(c->primary_device);
        c->primary_device = NULL;
    }
    free_primary_offer(c->primary_selection);
    c->primary_selection = NULL;
    clear_pending_primary_offers(c);
    if (c->primary_source) {
        zwp_primary_selection_source_v1_destroy(c->primary_source);
        c->primary_source = NULL;
    }
    if (!seat) return true;
    if (c->manager) {
        c->device = wl_data_device_manager_get_data_device(c->manager, seat);
        if (c->device)
            wl_data_device_add_listener(c->device, &device_listener, c);
    }
    if (c->primary_manager) {
        c->primary_device =
            zwp_primary_selection_device_manager_v1_get_device(c->primary_manager, seat);
        if (c->primary_device)
            zwp_primary_selection_device_v1_add_listener(c->primary_device,
                                                         &primary_device_listener, c);
    }
    return true;
}

bool clipboard_copy(struct clipboard *c, const char *text, uint32_t serial)
{
    if (!c || !c->device) { errno = ENOTSUP; return false; }
    if (!text) { errno = EINVAL; return false; }
    if (!serial) { errno = EAGAIN; return false; }
    char *copy = strdup(text);
    if (!copy) { errno = ENOMEM; return false; }
    if (c->source) wl_data_source_destroy(c->source);
    free(c->text); c->text = copy;
    c->source = wl_data_device_manager_create_data_source(c->manager);
    if (!c->source) { errno = ENOMEM; return false; }
    wl_data_source_add_listener(c->source, &source_listener, c);
    wl_data_source_offer(c->source, "text/plain;charset=utf-8");
    wl_data_source_offer(c->source, "text/plain");
    wl_data_source_offer(c->source, "UTF8_STRING");
    wl_data_source_offer(c->source, "STRING");
    wl_data_source_offer(c->source, "TEXT");
    wl_data_device_set_selection(c->device, c->source, serial);
    return true;
}

bool clipboard_copy_primary(struct clipboard *c, const char *text, uint32_t serial)
{
    if (!c || !c->primary_device || !c->primary_manager) { errno = ENOTSUP; return false; }
    if (!text) { errno = EINVAL; return false; }
    if (!serial) { errno = EAGAIN; return false; }
    char *copy = strdup(text);
    if (!copy) { errno = ENOMEM; return false; }
    if (c->primary_source) zwp_primary_selection_source_v1_destroy(c->primary_source);
    free(c->primary_text); c->primary_text = copy;
    c->primary_source = zwp_primary_selection_device_manager_v1_create_source(c->primary_manager);
    if (!c->primary_source) { errno = ENOMEM; return false; }
    zwp_primary_selection_source_v1_add_listener(c->primary_source, &primary_source_listener, c);
    zwp_primary_selection_source_v1_offer(c->primary_source, "text/plain;charset=utf-8");
    zwp_primary_selection_source_v1_offer(c->primary_source, "text/plain");
    zwp_primary_selection_source_v1_offer(c->primary_source, "UTF8_STRING");
    zwp_primary_selection_source_v1_offer(c->primary_source, "STRING");
    zwp_primary_selection_source_v1_offer(c->primary_source, "TEXT");
    zwp_primary_selection_device_v1_set_selection(c->primary_device, c->primary_source, serial);
    return true;
}

bool clipboard_request_paste(struct clipboard *c, enum yasl_private_selection selection,
                             struct wl_surface *target, uint64_t request_id)
{
    if (!c || !target || !c->receive)
        return false;
    int fds[2];
    if (selection == YASL_PRIVATE_PRIMARY) {
        if (!c->primary_device || !c->primary_selection || !c->primary_selection->mime) {
            c->receive(NULL, 0, false, target, request_id,
                       YASL_PRIVATE_TRANSFER_UNAVAILABLE, c->ud);
            return true;
        }
        if (pipe2(fds, O_CLOEXEC) < 0) {
            c->receive(NULL, 0, false, target, request_id,
                       YASL_PRIVATE_TRANSFER_FAILED, c->ud);
            return true;
        }
        zwp_primary_selection_offer_v1_receive(c->primary_selection->proxy,
                                               c->primary_selection->mime, fds[1]);
        close(fds[1]);
    } else {
        if (!c->device || !c->selection || !c->selection->mime) {
            c->receive(NULL, 0, false, target, request_id,
                       YASL_PRIVATE_TRANSFER_UNAVAILABLE, c->ud);
            return true;
        }
        if (pipe2(fds, O_CLOEXEC) < 0) {
            c->receive(NULL, 0, false, target, request_id,
                       YASL_PRIVATE_TRANSFER_FAILED, c->ud);
            return true;
        }
        wl_data_offer_receive(c->selection->proxy, c->selection->mime, fds[1]);
        close(fds[1]);
    }
    struct transfer *transfer = start_transfer_paste(c, fds[0], target, request_id);
    if (!transfer) {
        c->receive(NULL, 0, false, target, request_id,
                   YASL_PRIVATE_TRANSFER_FAILED, c->ud);
        return true;
    }
    if (wl_display_flush(c->display) < 0 && errno != EAGAIN)
        transfer_end(transfer, false);
    return true;
}

void clipboard_forget_surface(struct clipboard *c, struct wl_surface *surface)
{
    if (!c || !surface) return;
    for (struct transfer *t = c->transfers, *next; t; t = next) {
        next = t->next;
        if (t->drop_surface == surface) {
            t->cancelled = true;
            transfer_end(t, false);
        }
    }
    if (c->drag_surface == surface) c->drag_surface = NULL;
}

void clipboard_destroy(struct clipboard *c)
{
    if (!c) return;
    while (c->transfers)
        transfer_end(c->transfers, false);
    free_offer(c->selection);
    clear_pending_offers(c);
    free_offer(c->drag_offer);
    free_primary_offer(c->primary_selection);
    clear_pending_primary_offers(c);
    if (c->primary_source) zwp_primary_selection_source_v1_destroy(c->primary_source);
    if (c->primary_device) zwp_primary_selection_device_v1_destroy(c->primary_device);
    free(c->primary_text);
    if (c->source) wl_data_source_destroy(c->source);
    if (c->device) wl_data_device_destroy(c->device);
    free(c->text); free(c);
}
