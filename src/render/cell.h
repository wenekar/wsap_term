#ifndef RENDER_CELL_H
#define RENDER_CELL_H

#include <stdbool.h>
#include <stdint.h>

#include "font/font.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "render/term_frame.h"

#define TERM_SELECTION_FG 0xFFFFFFFFu
#define TERM_SELECTION_BG 0xFF585B70u

static inline bool render_cell_selected(int c, int sel_c0, int sel_c1)
{
    return c >= sel_c0 && c <= sel_c1;
}

static inline uint16_t render_cell_style(const struct wsap_vtcore_cell *cell)
{
    return (uint16_t)((cell->attrs & WSAP_VTCORE_ATTR_BOLD ? FONT_STYLE_BOLD : 0) |
                      (cell->attrs & WSAP_VTCORE_ATTR_ITALIC ? FONT_STYLE_ITALIC : 0) |
                      (cell->attrs & WSAP_VTCORE_ATTR_EMOJI ? FONT_STYLE_EMOJI : 0) |
                      (cell->attrs & WSAP_VTCORE_ATTR_TEXT ? FONT_STYLE_TEXT : 0) |
                      (cell->width == 2 ? FONT_STYLE_WIDE : 0));
}

static inline bool render_cell_decorated(uint16_t attrs)
{
    return attrs & (WSAP_VTCORE_ATTR_UNDERLINE_MASK | WSAP_VTCORE_ATTR_STRIKE |
                    WSAP_VTCORE_ATTR_OVERLINE | WSAP_VTCORE_ATTR_LINK);
}

static inline bool render_cell_shapeable(const struct wsap_vtcore_cell *cell,
                                         bool (*aligned_symbol)(uint32_t cp))
{
    return cell->text > ' ' && !(cell->text & WSAP_VTCORE_TEXT_CLUSTER) && cell->width == 1 &&
           (cell->text < 0x2500 || !aligned_symbol(cell->text));
}

static inline int render_segment_end(const struct wsap_vtcore_cell *cells, int c, int cols,
                                     bool (*aligned_symbol)(uint32_t cp))
{
    const struct wsap_vtcore_cell *first = &cells[c];
    if (!render_cell_shapeable(first, aligned_symbol))
        return c + 1;
    const uint16_t mask = FONT_STYLE_VARIANT_MASK;
    uint16_t style = render_cell_style(first) & mask;
    int end = c + 1;
    while (end < cols && render_cell_shapeable(&cells[end], aligned_symbol) &&
           (render_cell_style(&cells[end]) & mask) == style &&
           cells[end].fg == first->fg && cells[end].bg == first->bg)
        end++;
    return end;
}

static inline bool render_cell_is_cursor(const struct wsap_vtcore_cell *cell, int c,
                                         int cursor)
{
    return c == cursor ||
        (cell->width == 2 && c + 1 == cursor) ||
        (cell->width == 0 && c - 1 == cursor);
}

static inline struct wsap_vtcore_cell render_blank_cell(const struct terminal_frame *frame)
{
    return (struct wsap_vtcore_cell){
        .text = ' ', .fg = frame->default_fg, .bg = frame->default_bg, .width = 1,
    };
}

static inline struct wsap_vtcore_cell render_cell_colors(const struct terminal_frame *frame,
                                                  struct wsap_vtcore_cell cell)
{
    cell.fg = terminal_frame_color(frame, cell.fg);
    cell.bg = terminal_frame_color(frame, cell.bg);
    return cell;
}

static inline struct wsap_vtcore_cell render_resolve_cell(const struct terminal_frame *frame,
                                                   const struct wsap_vtcore_cell *cells, int c,
                                                   int sel_c0, int sel_c1, int cursor)
{
    const struct wsap_vtcore_cell blank = render_blank_cell(frame);
    struct wsap_vtcore_cell cell = cells ? render_cell_colors(frame, cells[c]) : blank;

    if (cell.text == 0 && cell.width != 0)
        cell = blank;
    if (frame->reverse_video) {
        uint32_t swap = cell.fg; cell.fg = cell.bg; cell.bg = swap;
    }
    if (render_cell_selected(c, sel_c0, sel_c1)) {
        cell.fg = TERM_SELECTION_FG;
        cell.bg = TERM_SELECTION_BG;
    }
    if (render_cell_is_cursor(&cell, c, cursor) &&
        frame->cursor_style == WSAP_VTCORE_CURSOR_BLOCK) {
        cell.fg = (cell.bg == frame->cursor_color) ? frame->default_bg : cell.bg;
        cell.bg = frame->cursor_color;
    }
    return cell;
}
#endif
