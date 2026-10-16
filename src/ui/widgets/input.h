#ifndef WSAP_UI_WIDGETS_INPUT_H
#define WSAP_UI_WIDGETS_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/css.h"
#include "ui/widget.h"

#define UI_INPUT_CAP             1024
#define UI_INPUT_PLACEHOLDER_CAP 128
#define UI_INPUT_BLINK_MS        500

struct ui_input_history;

struct ui_input {
    struct ui_widget widget;
    struct {
        char buf[UI_INPUT_CAP];
        char placeholder[UI_INPUT_PLACEHOLDER_CAP];
        int len;
        int caret;
        int anchor;
        int scroll_cells;
        uint64_t blink_at;
        bool blink_on;
        bool dragging;
        struct ui_input_history *undo, *redo;
        int undo_count, edit_group, edit_caret;
        bool editing;
        char kill[UI_INPUT_CAP];
        uint64_t click_at;
        struct vector2i click_pos;
        struct vector2i press_pos;
        int clicks;
        int action_id;
    } impl;
};

void ui_input_init(struct ui_input *in);

const char *ui_input_text(const struct ui_input *in);
int ui_input_caret(const struct ui_input *in);
bool ui_input_selection(const struct ui_input *in, int *start, int *end);

void ui_input_set_text(struct ui_input *in, const char *utf8);
void ui_input_set_placeholder(struct ui_input *in, const char *utf8);
void ui_input_set_action_id(struct ui_input *in, int id);
void ui_input_select_all(struct ui_input *in);
void ui_input_clear(struct ui_input *in);
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
