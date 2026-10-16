#ifndef WSAP_UI_WIDGETS_SCROLLVIEW_H
#define WSAP_UI_WIDGETS_SCROLLVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/widget.h"

/* The model for a line-based view: lines above the view, lines in the view,
 * and how far the view is scrolled back. */
struct ui_scroll_metrics {
    int history_lines;
    int visible_lines;
    int view_offset;
};

/*
 * A vertical scroller with one child, attached with ui_widget_add_child().
 * The child keeps its measured height; the scrollview clips it. Wheel
 * input that the child does not take scrolls the view.
 *
 * The scroller has a bar along its right edge. The bar starts on. In
 * overlay mode, the default, it starts hidden. It takes a primary press
 * and drags on its track, when the track is shown. A press with Shift, or
 * with another button, goes to the child under the track. A hidden bar
 * takes no press, and the child gets it. A press on the track keeps focus
 * where it was. Other presses on the scrollview clear focus.
 *
 * The track is the pointer target over the bar in every state, shown or
 * hidden. The bar passes on what it does not take: motion, presses, the
 * release of a press that began elsewhere, and wheel input the view does
 * not use. So the child sees every event that it would see without the
 * bar. A child that consumes motion does not stop the bar from showing.
 *
 * Two modes:
 * - Pixel mode (the default). The bar follows the child's height and the
 *   scrollview moves the child.
 * - Line mode, after ui_scrollview_set_metrics(). The bar follows the
 *   metrics. A press or a drag on the track sends UI_ACTION_CHANGE with the
 *   new view offset as index. The host applies the offset. The child fills
 *   the viewport height and does not move. Metrics with zero
 *   history_lines draw no bar and take no press. The view still does not
 *   scroll, and wheel input is still not consumed.
 *   ui_scrollview_clear_metrics() returns to pixel mode.
 *
 * Overlay mode, on by default, draws the bar over the child and shows it
 * only when flashed or hovered. It hides a moment after the pointer leaves.
 * Overlay takes no room from the child. The child keeps its full width,
 * and hover widens the bar over the child. A pixel scroll by a wheel step
 * or a key flashes the bar when the offset changes.
 * Without overlay, the bar takes room from the child. It keeps its idle
 * width in every state. Hover and press change only its colors.
 *
 * CSS: ".scrollbar" for the track and ".scrollbar-thumb" for the thumb,
 * with :hover and :active.
 */
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

/* Starts with the bar on, overlay on, and pixel mode. */
void ui_scrollview_init(struct ui_scrollview *sv);
struct ui_widget *ui_scrollview_child(const struct ui_scrollview *sv);

/* In line mode these report the child's pixel offset, which stays 0. */
int ui_scrollview_offset(const struct ui_scrollview *sv);
int ui_scrollview_content_height(const struct ui_scrollview *sv);
int ui_scrollview_viewport_height(const struct ui_scrollview *sv);

/* The offset stays in [0, content height - viewport height]. In line mode
 * the offset stays 0, so these do nothing. Scroll_by flashes the bar in
 * overlay mode when the offset changes. Scroll_to does not. */
void ui_scrollview_scroll_to(struct ui_scrollview *sv, int y);
void ui_scrollview_scroll_by(struct ui_scrollview *sv, int dy);
/* Pixels per wheel line. Zero means three cell heights. */
void ui_scrollview_set_scroll_step(struct ui_scrollview *sv, int px);
/* Turns the bar on or off. Off, it takes no room and no presses. */
void ui_scrollview_set_scrollbar(struct ui_scrollview *sv, bool enabled);

/* Line mode. The bar's range and thumb come from the metrics. The host
 * sets the metrics and applies the offset that the bar reports. */
void ui_scrollview_set_metrics(struct ui_scrollview *sv,
                               struct ui_scroll_metrics metrics);
/* Ends line mode and ends a drag on the bar. The view then scrolls in
 * pixels from the top. Does nothing outside line mode. */
void ui_scrollview_clear_metrics(struct ui_scrollview *sv);

/* Overlay mode, on by default. On, the bar starts hidden and shows when
 * flashed or hovered. Off, the bar takes room from the child and shows at
 * all times. */
void ui_scrollview_set_overlay(struct ui_scrollview *sv, bool overlay);
/* Shows the bar in overlay mode. It hides a moment later without hover. */
void ui_scrollview_flash(struct ui_scrollview *sv);
/* Hides the bar in overlay mode and ends a drag on it. */
void ui_scrollview_hide(struct ui_scrollview *sv);
/* True when the bar is on and, in overlay mode, not hidden. The bar draws
 * only when the view has something to scroll. */
bool ui_scrollview_is_shown(const struct ui_scrollview *sv);

static inline struct ui_widget *ui_scrollview_widget(struct ui_scrollview *sv)
{
    if (sv && !sv->widget.ops)
        ui_scrollview_init(sv);
    return sv ? &sv->widget : NULL;
}

#endif
