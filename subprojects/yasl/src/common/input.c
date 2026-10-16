#include "input.h"
#include "common/private_loop.h"

#include <stdlib.h>
#include <string.h>

static bool has_text(const struct yasl_private_event *ev)
{
    return ev->type == YASL_PRIVATE_EVENT_TEXT || ev->type == YASL_PRIVATE_EVENT_PASTE ||
           ev->type == YASL_PRIVATE_EVENT_DROP || ev->type == YASL_PRIVATE_EVENT_TEXT_EDIT;
}

static void discard_pending(struct input_queue *q)
{
    for (size_t i = 0; i < q->count; i++) {
        struct yasl_private_event *ev = &q->events[(q->head + i) & (q->capacity - 1)];
        if (has_text(ev)) free((void *)ev->text.data);
    }
    q->head = q->count = 0;
}

bool input_queue_push(struct input_queue *q, const struct yasl_private_event *ev)
{
    struct yasl_private_event copy = *ev;
    char *owned = NULL;
    if (!copy.time_ns) copy.time_ns = yasl_private_loop_now_ns();
    if (has_text(ev) && !q->publish) {
        size_t len = ev->text.len;
        if ((!ev->text.data && len) || len == SIZE_MAX) goto failed;
        owned = malloc(len + 1);
        if (!owned) goto failed;
        if (len) memcpy(owned, ev->text.data, len);
        owned[len] = '\0';
        if (ev->type == YASL_PRIVATE_EVENT_PASTE) copy.paste.data = owned;
        else if (ev->type == YASL_PRIVATE_EVENT_TEXT_EDIT) copy.text_edit.data = owned;
        else copy.text.data = owned;
    }
    uint32_t buttons = q->buttons;
    switch (ev->type) {
    case YASL_PRIVATE_EVENT_POINTER_MOTION:
        if (!q->relative) {
            copy.pointer.delta = q->has_last_pos
                ? (struct yasl_private_vector2d){ev->pointer.pos.x - q->last_pos.x,
                                         ev->pointer.pos.y - q->last_pos.y}
                : (struct yasl_private_vector2d){0, 0};
        }
        q->last_pos = ev->pointer.pos;
        q->has_last_pos = true;
        break;
    case YASL_PRIVATE_EVENT_POINTER_DOWN:
    case YASL_PRIVATE_EVENT_POINTER_UP:
        if ((unsigned)ev->pointer.button < 32) {
            uint32_t bit = YASL_PRIVATE_BUTTON_BIT(ev->pointer.button);
            buttons = ev->type == YASL_PRIVATE_EVENT_POINTER_DOWN ? buttons | bit : buttons & ~bit;
        }
        q->last_pos = ev->pointer.pos;
        q->has_last_pos = true;
        break;
    case YASL_PRIVATE_EVENT_POINTER_LEAVE:
        if (q->leave_ends_press) buttons = 0;
        q->has_last_pos = false;
        break;
    case YASL_PRIVATE_EVENT_INPUT_RESET:
        buttons = 0;
        q->has_last_pos = false;
        break;
    default:
        break;
    }
    if (ev->type >= YASL_PRIVATE_EVENT_POINTER_DOWN && ev->type <= YASL_PRIVATE_EVENT_POINTER_LEAVE)
        copy.pointer.buttons = buttons;
    if (q->publish) {
        q->buttons = buttons;
        return q->publish(&copy, q->publish_ud);
    }
    if (q->count == q->capacity) {
        size_t capacity = q->capacity ? q->capacity * 2 : 32;
        if (capacity < q->capacity || capacity > SIZE_MAX / sizeof(*q->events))
            goto failed;
        struct yasl_private_event *events = malloc(capacity * sizeof(*events));
        if (!events) goto failed;
        if (q->events && q->count > 0) {
            size_t first = q->capacity - q->head;
            if (first > q->count) first = q->count;
            memcpy(events, q->events + q->head, first * sizeof(*events));
            if (q->count > first)
                memcpy(events + first, q->events, (q->count - first) * sizeof(*events));
        }
        free(q->events);
        q->events = events;
        q->capacity = capacity;
        q->head = 0;
    }
    q->events[(q->head + q->count) & (q->capacity - 1)] = copy;
    q->count++;
    q->buttons = buttons;
    return true;

failed:
    free(owned);
    /* A missing release must never silently leave app input state stuck.
     * Discard the old stream and deliver an allocation-free reset first. */
    discard_pending(q);
    q->reset_surface = ev->surface;
    q->reset_pending = true;
    q->has_last_pos = false;
    q->buttons = 0;
    return false;
}

bool input_queue_pop(struct input_queue *q, struct yasl_private_event *ev)
{
    if (!ev || (!q->count && !q->reset_pending)) return false;
    if (q->last_text) {
        free(q->last_text);
        q->last_text = NULL;
    }
    if (q->reset_pending) {
        *ev = (struct yasl_private_event){
            .type = YASL_PRIVATE_EVENT_INPUT_RESET, .surface = q->reset_surface,
            .time_ns = yasl_private_loop_now_ns(),
        };
        q->reset_pending = false;
        return true;
    }
    *ev = q->events[q->head];
    if (has_text(ev))
        q->last_text = (char *)ev->text.data;
    q->head = (q->head + 1) & (q->capacity - 1);
    q->count--;
    return true;
}

void input_queue_clear(struct input_queue *q)
{
    bool (*publish)(const struct yasl_private_event *, void *) = q->publish;
    void *publish_ud = q->publish_ud;
    discard_pending(q);
    free(q->last_text);
    free(q->events);
    *q = (struct input_queue){0};
    q->publish = publish;
    q->publish_ud = publish_ud;
}
