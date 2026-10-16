#ifndef RENDER_TERM_FRAME_H
#define RENDER_TERM_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>

/* What one paint of a terminal needs. It points at a snapshot, never at a
 * live terminal, so paint workers may read it in parallel. */
struct terminal_frame {
    const struct wsap_vtcore_snapshot *source;
    /* Changes when every row must repaint (a palette change). */
    uint64_t full_gen;
    int cols, rows;
    int origin_y;
    int view_offset, scrolled;
    uint32_t default_fg, default_bg;
    uint32_t cursor_color;
    int cursor_x, cursor_y;
    int cursor_style;
    bool reverse_video;
    bool has_selection;
};

_Static_assert(sizeof(struct terminal_frame) == 64,
               "terminal_frame must fit in exactly one 64-byte cache line");

/* cursor_visible is the blink phase. */
void terminal_frame_init(struct terminal_frame *frame, const struct wsap_vtcore_snapshot *snap,
                         bool cursor_visible);
bool terminal_frame_valid(const struct terminal_frame *frame);

const struct wsap_vtcore_cell *terminal_frame_row(const struct terminal_frame *frame, int row);
/* Changes when the cells of the row change. */
uint64_t terminal_frame_row_key(const struct terminal_frame *frame, int row);
/* The inclusive selected columns [c0, c1], or c0 = cols, c1 = -1. */
void terminal_frame_row_selection(const struct terminal_frame *frame, int row,
                                  int cols, int *c0, int *c1);
uint32_t terminal_frame_color(const struct terminal_frame *frame, wsap_vtcore_color c);
/* The code point the font draws for a cell's text: the code point itself,
 * or a font cluster handle for a cluster cell. */
uint32_t terminal_frame_glyph(const struct terminal_frame *frame, uint32_t text);

#endif
