#ifndef WINDOW_UI_INPUT_H
#define WINDOW_UI_INPUT_H

/* Host input events as UI events. */

#include "input/input.h"
#include "ui/event.h"

/* The result points at ev through native; keep ev alive for dispatch. */
struct ui_key_event ui_key_from_input(const struct input_key_event *ev);
struct ui_pointer_event ui_pointer_from_input(const struct input_mouse_event *ev);

#endif
