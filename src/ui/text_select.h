#ifndef WSAP_UI_TEXT_SELECT_H
#define WSAP_UI_TEXT_SELECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/keys.h"
#include "util/geom.h"

/* The pointer moves less than this many pixels for a press to repeat the
 * click before it. */
#define UI_TEXT_SELECT_DRAG_PX 4

/* Caret, anchor, and click gesture state of one text widget.
 * Call the functions here from the pointer and key handlers of the widget.
 * Read the range here from the paint, copy, and accessibility code. */
struct ui_text_select {
    size_t caret, anchor;
    size_t gesture_start, gesture_end;
    struct vector2i press_pos, click_pos;
    uint64_t click_at;
    unsigned clicks;
    bool pressed, dragged;
    /* The press selects text. A link that cannot be selected passes false. */
    bool selects;
};

/* One primary-button press on the text of a widget. */
struct ui_text_select_click {
    struct vector2i pos;
    uint64_t time_ms;
    size_t offset;
    uint32_t interval_ms;
    int threshold_px;
    unsigned mods;
    bool selects;
};

/* Drops the caret, the selection, and the click count. */
void ui_text_select_clear(struct ui_text_select *sel);
/* Drops the press state and the click count. Keeps the caret and the
 * selection. */
void ui_text_select_end(struct ui_text_select *sel);
/* Moves the caret to `offset`. Without `extend` it drops the selection. */
void ui_text_select_to(struct ui_text_select *sel, size_t offset, bool extend);
/* Moves the caret by one `unit`. Without `extend` it first collapses the
 * selection to the edge at the start of the move. */
void ui_text_select_move(struct ui_text_select *sel, const char *text,
                         size_t len, enum ui_edit_unit unit, bool backward,
                         bool extend);
/* Selects the whole text. */
void ui_text_select_all(struct ui_text_select *sel, size_t len);
/* Sets the caret and the selection from an accessibility request.
 * A collapsed request (`start == end`) moves the caret to `caret`. */
void ui_text_select_apply(struct ui_text_select *sel, const char *text,
                          size_t len, size_t start, size_t end, size_t caret);
/* Handles a primary-button press. A press that repeats within the interval
 * selects the word under the pointer. A third press selects the line. */
void ui_text_select_press(struct ui_text_select *sel, const char *text,
                          size_t len, const struct ui_text_select_click *click);
/* Extends the selection to `offset` while the button stays down.
 * Returns true when the drag changed the selection. */
bool ui_text_select_drag(struct ui_text_select *sel, const char *text,
                         size_t len, size_t offset, struct vector2i pos,
                         int threshold_px);
/* Ends the press. Keeps the click count for the next press. */
void ui_text_select_release(struct ui_text_select *sel);
/* Returns the selection. It returns false when the selection is empty. */
bool ui_text_select_range(const struct ui_text_select *sel, size_t *start,
                          size_t *end);

#endif
