#ifndef WSAP_UI_WIDGETS_PALETTE_H
#define WSAP_UI_WIDGETS_PALETTE_H

#include <stdbool.h>

#include "ui/widget.h"
#include "ui/widgets/input.h"
#include "ui/widgets/label.h"
#include "ui/widgets/list.h"

#define UI_PALETTE_MAX_ROWS 12

struct ui_palette_cmd {
    const char *label;
    const char *shortcut;
    int action;
};

struct ui_palette {
    struct ui_widget widget;
    struct ui_input input;
    struct ui_list list;
    struct ui_label empty;
    struct {
        struct ui_palette_cmd *cmds;
        int n_cmds;
        int max_rows;
        bool stale;
    } impl;
};

void ui_palette_init(struct ui_palette *p);
bool ui_palette_open(struct ui_palette *p, const struct ui_palette_cmd *cmds,
                     int n);
void ui_palette_close(struct ui_palette *p);
bool ui_palette_is_open(const struct ui_palette *p);
const char *ui_palette_query(const struct ui_palette *p);

static inline struct ui_widget *ui_palette_widget(struct ui_palette *p)
{
    return p ? &p->widget : NULL;
}

#endif
