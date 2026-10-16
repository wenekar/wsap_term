#ifndef WSAP_UI_WIDGETS_LABEL_H
#define WSAP_UI_WIDGETS_LABEL_H

#include "ui/widget.h"

/* Legacy scratch-buffer size. Label text has no fixed buffer limit. */
#define UI_LABEL_TEXT_CAP 256

/* Zero selects word wrapping and left alignment. */
enum ui_label_wrap {
    UI_LABEL_WRAP_WORD = 0,
    UI_LABEL_WRAP_CHAR,
    UI_LABEL_CLIP,
    UI_LABEL_ELLIPSIS,
};

enum ui_label_align {
    UI_LABEL_ALIGN_LEFT = 0,
    UI_LABEL_ALIGN_CENTER,
    UI_LABEL_ALIGN_RIGHT,
};

/* Read-only UTF-8 text. Storage grows and shrinks with the text.
 * Destroy the widget to release its text and link.
 * Selection and links are opt-in. CSS styles the widget under "label".
 * Text uses the UI's cell metrics, not proportional shaping. */
struct ui_label {
    struct ui_widget widget;
    struct {
        char *text, *link;
        size_t len, anchor, caret;
        size_t gesture_start, gesture_end;
        uint64_t click_time;
        struct vector2i click_pos, press_pos;
        enum ui_label_wrap wrap;
        enum ui_label_align align;
        int action_id;
        unsigned clicks;
        bool selectable, disabled, pressed, dragged;
    } impl;
};

void ui_label_init(struct ui_label *label);
/* Copies text without truncation. NULL clears it.
 * False means allocation failed or the text exceeds INT_MAX bytes.
 * On failure, the previous text and selection stay unchanged.
 * The source can be the label's own text or a suffix of it. */
bool ui_label_set_text(struct ui_label *label, const char *text);
const char *ui_label_text(const struct ui_label *label);
void ui_label_set_wrap(struct ui_label *label, enum ui_label_wrap wrap);
void ui_label_set_align(struct ui_label *label, enum ui_label_align align);

/* Enables drag selection, double-click words, and triple-click lines.
 * Shift extends selection. Ctrl/Command+A selects all.
 * Ctrl/Command+C copies through the surface host. */
void ui_label_set_selectable(struct ui_label *label, bool selectable);
/* Returns UTF-8 byte offsets. False means the selection is empty. */
bool ui_label_selection(const struct ui_label *label, size_t *start, size_t *end);
void ui_label_select_all(struct ui_label *label);
void ui_label_clear_selection(struct ui_label *label);
bool ui_label_copy_selection(struct ui_label *label);

/* Copies a link target. NULL or an empty string removes the link.
 * Activation emits UI_ACTION_ACTIVATE with the label's action ID.
 * If no handler takes it, the surface host opens the target.
 * The UI library does not interpret the target or open it by itself. */
bool ui_label_set_link(struct ui_label *label, const char *target);
const char *ui_label_link(const struct ui_label *label);
void ui_label_set_action_id(struct ui_label *label, int id);
void ui_label_set_disabled(struct ui_label *label, bool disabled);

static inline struct ui_widget *ui_label_widget(struct ui_label *label)
{
    if (label && !label->widget.ops)
        ui_label_init(label);
    return label ? &label->widget : NULL;
}

#endif
