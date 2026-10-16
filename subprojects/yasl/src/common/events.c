#include "backend.h"
#include "platform/sync.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct event_node {
    struct yasl_private_event event;
    uint64_t queue_id;
    char *text;
    void (*release)(void *);
    struct event_node *next;
};

struct yasl_private_event_state {
    yasl_private_mutex mutex;
    struct event_node *head, *tail, *last;
    uint64_t sequence, identity, request;
    int error;
};

static void dispose(struct event_node *node)
{
    if (!node) return;
    if (node->release) node->release(node->event.user.payload);
    free(node->text);
    free(node);
}

static void dispose_list(struct event_node *nodes)
{
    while (nodes) {
        struct event_node *next = nodes->next;
        dispose(nodes);
        nodes = next;
    }
}

bool yasl_private_events_open(struct yasl_private_display *d)
{
    struct yasl_private_event_state *state = calloc(1, sizeof(*state));
    if (!state) { errno = ENOMEM; return false; }
    int error = yasl_private_mutex_init(&state->mutex);
    if (error) { free(state); errno = error; return false; }
    d->events = state;
    return true;
}

void yasl_private_events_close(struct yasl_private_display *d)
{
    struct yasl_private_event_state *state = d->events;
    if (!state) return;
    /* Producers must already have stopped. Detach before callbacks so cleanup
     * can make harmless failed posts without touching a dying queue. */
    d->events = NULL;
    dispose_list(state->head);
    dispose(state->last);
    yasl_private_mutex_destroy(&state->mutex);
    free(state);
}

/* Reports a lost event once, so one failed allocation does not fail every later dispatch. */
int yasl_private_events_error(struct yasl_private_display *d)
{
    if (!d->events) return 0;
    yasl_private_mutex_lock(&d->events->mutex);
    int error = d->events->error;
    d->events->error = 0;
    yasl_private_mutex_unlock(&d->events->mutex);
    return error;
}

bool yasl_private_events_failed(struct yasl_private_display *d)
{
    if (!d->events) return false;
    yasl_private_mutex_lock(&d->events->mutex);
    bool failed = d->events->error != 0;
    yasl_private_mutex_unlock(&d->events->mutex);
    return failed;
}

bool yasl_private_events_pending(struct yasl_private_display *d)
{
    if (!d->events) return false;
    yasl_private_mutex_lock(&d->events->mutex);
    bool pending = d->events->head != NULL;
    yasl_private_mutex_unlock(&d->events->mutex);
    return pending;
}

struct yasl_surface *yasl_private_events_find(struct yasl_private_display *d, uint64_t id)
{
    for (struct yasl_surface *s = d->surfaces; s; s = s->event_next)
        if (s->id == id) return s;
    return NULL;
}

static bool publish_input(const struct yasl_private_event *event, void *ud)
{
    struct yasl_surface *owner = ud;
    return yasl_private_events_push(owner->display, owner, event);
}

bool yasl_private_events_register(struct yasl_surface *s)
{
    struct yasl_private_display *d = s->display;
    if (!d->events) return true; /* Internal test doubles need no queue. */
    yasl_private_mutex_lock(&d->events->mutex);
    if (d->events->identity == UINT64_MAX) {
        yasl_private_mutex_unlock(&d->events->mutex);
        errno = EOVERFLOW;
        return false;
    }
    s->reported_scale = s->scale_120;
    s->id = ++d->events->identity;
    s->event_next = d->surfaces;
    d->surfaces = s;
    yasl_private_mutex_unlock(&d->events->mutex);
    struct input_queue *q = d->ops->surface_input_queue
        ? d->ops->surface_input_queue(s) : &s->input;
    if (q) {
        /* Preserve input generated during native creation. */
        struct yasl_private_event event;
        while (input_queue_pop(q, &event))
            yasl_private_events_push(d, s, &event);
        q->publish = publish_input;
        q->publish_ud = s;
    }
    return true;
}

void yasl_private_events_unregister(struct yasl_surface *s)
{
    struct yasl_private_display *d = s->display;
    if (!d->events) return;
    struct event_node *discarded = NULL;
    yasl_private_mutex_lock(&d->events->mutex);
    struct yasl_surface **surface = &d->surfaces;
    while (*surface && *surface != s) surface = &(*surface)->event_next;
    if (*surface) *surface = s->event_next;
    struct event_node **entry = &d->events->head;
    d->events->tail = NULL;
    while (*entry) {
        struct event_node *node = *entry;
        if (node->event.surface_id == s->id || node->queue_id == s->id) {
            *entry = node->next;
            node->next = discarded;
            discarded = node;
        } else {
            d->events->tail = node;
            entry = &node->next;
        }
    }
    s->event_next = NULL;
    yasl_private_mutex_unlock(&d->events->mutex);
    dispose_list(discarded);
}

static bool append(struct yasl_private_display *d, struct event_node *node)
{
    struct yasl_private_event_state *state = d->events;
    if (state->sequence == UINT64_MAX) { errno = EOVERFLOW; return false; }
    node->event.sequence = ++state->sequence;
    if (!node->event.time_ns) node->event.time_ns = yasl_private_loop_now_ns();
    if (state->tail) state->tail->next = node;
    else state->head = node;
    state->tail = node;
    return true;
}

static bool string_event(enum yasl_private_event_type type)
{
    return type == YASL_PRIVATE_EVENT_TEXT || type == YASL_PRIVATE_EVENT_PASTE ||
        type == YASL_PRIVATE_EVENT_DROP || type == YASL_PRIVATE_EVENT_COMPOSITION || type == YASL_PRIVATE_EVENT_TEXT_EDIT;
}

bool yasl_private_events_push(struct yasl_private_display *d, struct yasl_surface *queue_owner,
                      const struct yasl_private_event *event)
{
    if (!d || !d->events || (event->surface && !event->surface->id)) return true;
    struct event_node *node = calloc(1, sizeof(*node));
    if (!node) goto failed;
    node->event = *event;
    node->event.surface_id = event->surface ? event->surface->id : 0;
    node->queue_id = queue_owner ? queue_owner->id : node->event.surface_id;
    if (string_event(event->type)) {
        if (event->text.len == SIZE_MAX || (!event->text.data && event->text.len)) {
            free(node);
            goto failed;
        }
        node->text = malloc(event->text.len + 1);
        if (!node->text) { free(node); goto failed; }
        if (event->text.len) memcpy(node->text, event->text.data, event->text.len);
        node->text[event->text.len] = '\0';
        if (event->type == YASL_PRIVATE_EVENT_PASTE) node->event.paste.data = node->text;
        else if (event->type == YASL_PRIVATE_EVENT_TEXT_EDIT) node->event.text_edit.data = node->text;
        else if (event->type == YASL_PRIVATE_EVENT_COMPOSITION) node->event.composition.data = node->text;
        else node->event.text.data = node->text;
    }
    yasl_private_mutex_lock(&d->events->mutex);
    if ((node->event.surface_id && !yasl_private_events_find(d, node->event.surface_id)) ||
        (node->queue_id && !yasl_private_events_find(d, node->queue_id))) {
        yasl_private_mutex_unlock(&d->events->mutex);
        dispose(node);
        return true; /* A native teardown notification for a canceled recipient. */
    }
    bool ok = append(d, node);
    if (!ok) d->events->error = errno;
    yasl_private_mutex_unlock(&d->events->mutex);
    if (!ok) dispose(node);
    /* The owner's next step sees the queue; another thread must wake it. */
    yasl_private_loop_wake(d->loop);
    return ok;
failed:
    yasl_private_mutex_lock(&d->events->mutex);
    d->events->error = ENOMEM;
    yasl_private_mutex_unlock(&d->events->mutex);
    /* The loss is an event too; the owner reports it in its next step. */
    yasl_private_loop_wake(d->loop);
    errno = ENOMEM;
    return false;
}

bool yasl_private_display_post(struct yasl_private_display *d, const struct yasl_private_message *message)
{
    if (!d || !d->events || !message) { errno = EINVAL; return false; }
    struct event_node *node = calloc(1, sizeof(*node));
    if (!node) { errno = ENOMEM; return false; }
    node->event = (struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_USER, .surface_id = message->target_id,
        .user = {.code = message->code, .payload = message->payload, .size = message->size},
    };
    node->queue_id = message->target_id;
    node->release = message->release;
    yasl_private_mutex_lock(&d->events->mutex);
    bool ok = !message->target_id || yasl_private_events_find(d, message->target_id);
    if (!ok) errno = ENOENT;
    else ok = append(d, node);
    yasl_private_mutex_unlock(&d->events->mutex);
    if (!ok) { free(node); return false; }
    if (d->loop) yasl_private_loop_kick(d->loop);
    return true;
}

bool yasl_private_display_poll_event(struct yasl_private_display *d, struct yasl_private_event *event)
{
    if (!d || !d->events || !event) return false;
    struct yasl_private_event_state *state = d->events;
    yasl_private_mutex_lock(&state->mutex);
    struct event_node *node = state->head;
    if (!node) {
        yasl_private_mutex_unlock(&state->mutex);
        return false;
    }
    state->head = node->next;
    if (state->tail == node) state->tail = NULL;
    node->next = NULL;
    if (node->event.surface_id)
        node->event.surface = yasl_private_events_find(d, node->event.surface_id);
    struct event_node *last = state->last;
    state->last = node;
    *event = node->event;
    yasl_private_mutex_unlock(&state->mutex);
    dispose(last);
    return true;
}

uint64_t yasl_private_events_request_id(struct yasl_private_display *d)
{
    if (!d->events || d->events->request == UINT64_MAX) { errno = EOVERFLOW; return 0; }
    return ++d->events->request;
}

uint64_t yasl_private_surface_id(const struct yasl_surface *s) { return s ? s->id : 0; }
void yasl_private_surface_set_userdata(struct yasl_surface *s, void *ud) { if (s) s->userdata = ud; }
void *yasl_private_surface_userdata(const struct yasl_surface *s) { return s ? s->userdata : NULL; }

void yasl_private_surface_notify_configure(struct yasl_surface *s, struct yasl_private_geometry geometry)
{
    uint32_t previous_scale = s->reported_scale;
    s->reported_scale = geometry.scale_120;
    s->size = geometry.size;
    s->scale_120 = geometry.scale_120;
    struct yasl_private_event event = {.type = YASL_PRIVATE_EVENT_WINDOW_RESIZE, .surface = s,
        .resize = {.geometry = geometry, .scale_changed = previous_scale != geometry.scale_120}};
    yasl_private_events_push(s->display, s, &event);
    if (s->display) yasl_private_surface_request_frame(s);
}

void yasl_private_surface_notify_close(struct yasl_surface *s)
{
    struct yasl_private_event event = {.type = YASL_PRIVATE_EVENT_WINDOW_CLOSE, .surface = s};
    yasl_private_events_push(s->display, s, &event);
}

void yasl_private_surface_notify_state(struct yasl_surface *s, uint32_t flags)
{
    s->state = flags;
    struct yasl_private_event event = {.type = YASL_PRIVATE_EVENT_WINDOW_STATE, .surface = s, .state.flags = flags};
    yasl_private_events_push(s->display, s, &event);
}

void yasl_private_surface_notify_drag(struct yasl_surface *s, enum yasl_private_drag_event action, struct yasl_private_vector2i pos)
{
    struct yasl_private_event event = {.type = YASL_PRIVATE_EVENT_DRAG, .surface = s, .drag = {action, pos}};
    if (action == YASL_PRIVATE_DRAG_DROP)
        s->display->drag_dropped = true;
    yasl_private_events_push(s->display, s, &event);
}

void yasl_private_surface_notify_drop(struct yasl_surface *s, enum yasl_private_drop_event action, struct yasl_private_vector2i pos)
{
    struct yasl_private_event event = {.type = YASL_PRIVATE_EVENT_DROP_HOVER, .surface = s, .drop = {action, pos}};
    yasl_private_events_push(s->display, s, &event);
}

void yasl_private_surface_notify_composition(struct yasl_surface *s, const char *text, int begin, int end)
{
    struct yasl_private_event event = {.type = YASL_PRIVATE_EVENT_COMPOSITION, .surface = s,
        .composition = {.data = text ? text : "", .len = text ? strlen(text) : 0,
            .begin = begin, .end = end, .active = text && text[0],
            .revision = s->text_revision, .generation = s->text_generation}};
    yasl_private_events_push(s->display, s, &event);
}

void yasl_private_surface_complete_paste(struct yasl_surface *s, uint64_t id,
        enum yasl_private_transfer_status status, const char *text, size_t len)
{
    struct yasl_private_event event = {.type = YASL_PRIVATE_EVENT_PASTE, .surface = s,
        .paste = {.data = text ? text : "", .len = text ? len : 0, .request_id = id, .status = status}};
    if (!s->display->events) {
        struct input_queue *q = s->display->ops->surface_input_queue
            ? s->display->ops->surface_input_queue(s) : &s->input;
        input_queue_push(q, &event);
    } else yasl_private_events_push(s->display, s, &event);
}

void yasl_private_display_notify_event(struct yasl_private_display *d, enum yasl_private_event_type type)
{
    struct yasl_private_event event = {.type = type};
    yasl_private_events_push(d, NULL, &event);
}
