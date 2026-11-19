#ifndef YASL_PRIVATE_EVENT_TEXT_H
#define YASL_PRIVATE_EVENT_TEXT_H

#include "common/types.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Returns true when an event type has a text payload. The payload sits in the
 * .text, .paste, .text_edit, or .composition member. The display FIFO and the
 * input queue both use this list. Add a new text type here. */
static inline bool yasl_private_event_has_text(enum yasl_private_event_type type)
{
    switch (type) {
    case YASL_PRIVATE_EVENT_TEXT:
    case YASL_PRIVATE_EVENT_PASTE:
    case YASL_PRIVATE_EVENT_DROP:
    case YASL_PRIVATE_EVENT_TEXT_EDIT:
    case YASL_PRIVATE_EVENT_COMPOSITION:
        return true;
    default:
        return false;
    }
}

/* Copies the text payload of an event into a new NUL-terminated block. The
 * event then points its text field at the copy. Call it only for a type that
 * yasl_private_event_has_text() accepts. Returns NULL when the payload is
 * invalid or the allocation fails. The caller owns the block and frees it
 * when the event is discarded. */
static inline char *yasl_private_event_copy_text(struct yasl_private_event *event)
{
    size_t len = event->text.len;
    if ((!event->text.data && len) || len == SIZE_MAX) return NULL;
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    if (len) memcpy(copy, event->text.data, len);
    copy[len] = '\0';
    switch (event->type) {
    case YASL_PRIVATE_EVENT_PASTE: event->paste.data = copy; break;
    case YASL_PRIVATE_EVENT_TEXT_EDIT: event->text_edit.data = copy; break;
    case YASL_PRIVATE_EVENT_COMPOSITION: event->composition.data = copy; break;
    default: event->text.data = copy; break; /* TEXT and DROP. */
    }
    return copy;
}

#endif
