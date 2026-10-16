#ifndef YASL_PRIVATE_INPUT_H
#define YASL_PRIVATE_INPUT_H

#include "common/types.h"

/* Each queued string and the last successfully polled string are owned here. */
struct input_queue {
    struct yasl_private_event *events;
    size_t head, count, capacity;
    char *last_text;
    /* Last pointer position, for computing absolute-mode motion deltas. */
    struct yasl_private_vector2i last_pos;
    bool has_last_pos;
    /* Set by backends while YASL_PRIVATE_POINTER_RELATIVE is active: keep pushed deltas. */
    bool relative;
    struct yasl_surface *reset_surface;
    bool reset_pending;
    /* YASL_PRIVATE_BUTTON_BIT() mask from pushed DOWN/UP events. */
    uint32_t buttons;
    /* Set by backends whose pointer leave ends any press: no release follows. */
    bool leave_ends_press;
    /* Installed by common code for display-wide publication. Standalone queues
     * retain their normal storage/pop semantics when publish is NULL. */
    bool (*publish)(const struct yasl_private_event *ev, void *ud);
    void *publish_ud;
};

/* Copies text payloads and stamps time_ns when zero. Unless q->relative is set,
 * POINTER_MOTION delta is computed from the previous position; otherwise the
 * backend-supplied delta is kept. Leave and reset clear the position history.
 * Pointer events get the held-button mask; reset clears it. */
bool input_queue_push(struct input_queue *q, const struct yasl_private_event *ev);
bool input_queue_pop(struct input_queue *q, struct yasl_private_event *ev);
void input_queue_clear(struct input_queue *q);

#endif
