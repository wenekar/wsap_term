#ifndef WSAP_UI_ACTION_H
#define WSAP_UI_ACTION_H

#include <stdbool.h>

#include "util/geom.h"

struct ui_widget;

enum ui_action_kind {
    UI_ACTION_NONE = 0,
    UI_ACTION_ACTIVATE,
    UI_ACTION_CHANGE,
    UI_ACTION_HIGHLIGHT,
    UI_ACTION_CLOSE,
    UI_ACTION_MENU,
    /* The first kind that a widget can define for itself. */
    UI_ACTION_USER = 64,
};

struct ui_action {
    enum ui_action_kind kind;
    int id;
    int index;
    int arg;
    struct vector2i pos;
};

typedef bool (*ui_action_fn)(struct ui_widget *source,
                              const struct ui_action *action, void *userdata);

struct ui_action_handler {
    ui_action_fn fn;
    void *userdata;
};

#endif
