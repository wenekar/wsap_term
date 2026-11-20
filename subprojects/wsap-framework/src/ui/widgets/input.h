#ifndef WSAP_UI_WIDGETS_INPUT_H
#define WSAP_UI_WIDGETS_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/css.h"
#include "ui/text_select.h"
#include "ui/widget.h"

#define UI_INPUT_BLINK_MS        500

struct ui_input_history;

struct ui_input {
    struct ui_widget widget;
    struct {
        char *buf;
        char *placeholder;
        char *kill;
        size_t capacity;
        struct ui_input_history *undo, *redo;
        struct ui_text_select select;
        uint64_t blink_at;
        size_t len;
        int scroll_cells;
        int undo_count;
        int edit_group;
        size_t edit_caret;
        int action_id;
        bool blink_on;
    } state;
};

void ui_input_init(struct ui_input *in);

const char *ui_input_text(const struct ui_input *in);
/* Returns the caret byte offset. */
size_t ui_input_caret(const struct ui_input *in);
/* Returns the selected byte range. The result is false when it is empty. */
bool ui_input_selection(const struct ui_input *in, size_t *start, size_t *end);

/* Replace the text without sending an action.
 * Clear undo history and selection. Move the caret to the end.
 * Remove invalid UTF-8 bytes, ASCII control characters, and the first NUL
 * byte and what follows it.
 * Pass NULL to set an empty string.
 * Return false on allocation failure or if text exceeds INT_MAX bytes.
 * A failed call keeps the previous text and edit state. */
bool ui_input_set_text(struct ui_input *in, const char *utf8);
/* Copy the placeholder. A failed call keeps the previous placeholder. */
bool ui_input_set_placeholder(struct ui_input *in, const char *utf8);
void ui_input_set_action_id(struct ui_input *in, int id);
void ui_input_select_all(struct ui_input *in);
void ui_input_clear(struct ui_input *in);
/* Insert at the caret. Remove the selection first.
 * Authored text ends at the first NUL byte. Pasted text may hold invalid
 * bytes and ASCII control characters. They are removed. */
bool ui_input_insert_text(struct ui_input *in, const char *utf8);

bool ui_input_command(struct ui_input *in, enum ui_edit_command command);
bool ui_input_can_command(const struct ui_input *in, enum ui_edit_command command);

int ui_input_text_inset(const struct ui_input *in);

struct ui_input *ui_input_of(struct ui_widget *widget);

static inline struct ui_widget *ui_input_widget(struct ui_input *in)
{
    if (in && !in->widget.ops)
        ui_input_init(in);
    return in ? &in->widget : NULL;
}

#endif
