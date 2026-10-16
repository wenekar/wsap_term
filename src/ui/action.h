#ifndef WSAP_UI_ACTION_H
#define WSAP_UI_ACTION_H

#include <stdbool.h>

#include "util/geom.h"

struct ui_widget;

/* Zero is "no action". */
enum ui_action_kind {
    UI_ACTION_NONE = 0,
    /* A button, item, or tab was chosen. */
    UI_ACTION_ACTIVATE,
    /* A value changed: input text, list selection, scroll offset. */
    UI_ACTION_CHANGE,
    /* An item is under the pointer or the keyboard selection. */
    UI_ACTION_HIGHLIGHT,
    /* A popup, menu, or palette closed. Sent once, after any ACTIVATE. */
    UI_ACTION_CLOSE,
    /* An item moved from index to `to`. */
    UI_ACTION_MOVE,
    /* A drag of item index is in progress. id is the control's drag state. */
    UI_ACTION_DRAG,
    /* Item index left its control. pos is the grab offset in the item.
     * Return true to take the drag: the control then ends its gesture. */
    UI_ACTION_TEAR_OFF,
    /* A context menu was asked for at pos, in surface space. */
    UI_ACTION_MENU,
};

/* id is the model's action ID, or a control-specific code. index is the
 * model's item index, or -1. */
struct ui_action {
    enum ui_action_kind kind;
    int id;
    int index;
    int to;
    struct vector2i pos;
};

/* Return true to stop the action at this handler. source is the control
 * that raised it. The handler may change or destroy widgets. Keep their
 * storage alive until dispatch returns. */
typedef bool (*ui_action_fn)(struct ui_widget *source,
                              const struct ui_action *action, void *userdata);

struct ui_action_handler {
    ui_action_fn fn;
    void *userdata;
};

#endif
