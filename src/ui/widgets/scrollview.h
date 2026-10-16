#ifndef WSAP_UI_WIDGETS_SCROLLVIEW_H
#define WSAP_UI_WIDGETS_SCROLLVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/widget.h"

struct ui_scroll_metrics {
    int history_lines;
    int visible_lines;
    int view_offset;
};

struct ui_scrollview {
    struct ui_widget widget;
    struct {
        struct ui_scroll_metrics metrics;
        uint64_t hide_at;
        float scroll_remainder;
        int scroll_y;
        int scroll_step;
        int grab_y;
        int bar_state;
        bool has_metrics;
        bool bar_off;
        bool fixed_bar;
        bool shown;
    } impl;
};

void ui_scrollview_init(struct ui_scrollview *sv);
struct ui_widget *ui_scrollview_child(const struct ui_scrollview *sv);

int ui_scrollview_offset(const struct ui_scrollview *sv);
int ui_scrollview_content_height(const struct ui_scrollview *sv);
int ui_scrollview_viewport_height(const struct ui_scrollview *sv);

void ui_scrollview_scroll_to(struct ui_scrollview *sv, int y);
void ui_scrollview_scroll_by(struct ui_scrollview *sv, int dy);
void ui_scrollview_set_scroll_step(struct ui_scrollview *sv, int px);
void ui_scrollview_set_scrollbar(struct ui_scrollview *sv, bool enabled);

void ui_scrollview_set_metrics(struct ui_scrollview *sv,
                               struct ui_scroll_metrics metrics);
void ui_scrollview_clear_metrics(struct ui_scrollview *sv);

void ui_scrollview_set_overlay(struct ui_scrollview *sv, bool overlay);
void ui_scrollview_flash(struct ui_scrollview *sv);
void ui_scrollview_hide(struct ui_scrollview *sv);
bool ui_scrollview_is_shown(const struct ui_scrollview *sv);

static inline struct ui_widget *ui_scrollview_widget(struct ui_scrollview *sv)
{
    if (sv && !sv->widget.ops)
        ui_scrollview_init(sv);
    return sv ? &sv->widget : NULL;
}

#endif
