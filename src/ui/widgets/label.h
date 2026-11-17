#ifndef WSAP_UI_WIDGETS_LABEL_H
#define WSAP_UI_WIDGETS_LABEL_H

#include <stdint.h>

#include "ui/text_select.h"
#include "ui/widget.h"

#define UI_LABEL_TEXT_CAP 256

enum ui_label_wrap : uint8_t {
    UI_LABEL_WRAP_WORD = 0,
    UI_LABEL_WRAP_CHAR,
    UI_LABEL_CLIP,
    UI_LABEL_ELLIPSIS,
};

enum ui_label_align : uint8_t {
    UI_LABEL_ALIGN_LEFT = 0,
    UI_LABEL_ALIGN_CENTER,
    UI_LABEL_ALIGN_RIGHT,
};

/* One line of label text. */
struct label_line {
    size_t start, len;
    int cells;
    bool ellipsis;
};

/* The lines of one label text for one width, wrap mode, and cell size. */
struct label_lines {
    struct label_line *items;
    size_t count, capacity;
    unsigned text_gen;
    int width, advance, cell_h;
    enum ui_label_wrap wrap;
};

struct ui_label {
    struct ui_widget widget;
    struct {
        char *text, *link;
        size_t len;
        struct ui_text_select select;
        struct label_lines lines;
        int action_id;
        unsigned text_gen;
        enum ui_label_wrap wrap;
        enum ui_label_align align;
        bool selectable, disabled;
    } state;
};

void ui_label_init(struct ui_label *label);
bool ui_label_set_text(struct ui_label *label, const char *text);
const char *ui_label_text(const struct ui_label *label);
void ui_label_set_wrap(struct ui_label *label, enum ui_label_wrap wrap);
void ui_label_set_align(struct ui_label *label, enum ui_label_align align);

void ui_label_set_selectable(struct ui_label *label, bool selectable);
bool ui_label_selection(const struct ui_label *label, size_t *start, size_t *end);
void ui_label_select_all(struct ui_label *label);
void ui_label_clear_selection(struct ui_label *label);
bool ui_label_copy_selection(struct ui_label *label);

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
