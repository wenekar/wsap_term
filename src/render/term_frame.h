#ifndef RENDER_TERM_FRAME_H
#define RENDER_TERM_FRAME_H

/*
 * The terminal grid of one frame: a snapshot and the values that render
 * reads from it. The source is render/frame/term_frame.c.
 *
 * - The frame has no position. The grid position is render_grid.rect.
 *   The renderer finds scrolled content through the row keys.
 * - `palette_gen` is the palette version of the view.
 * - The app sets selection_fg and selection_bg. Zero means the built-in
 *   colors below.
 */

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>

/* The selection colors that a zero selection_fg or selection_bg means. */
#define TERMINAL_FRAME_SELECTION_FG 0xFFFFFFFFu
#define TERMINAL_FRAME_SELECTION_BG 0xFF585B70u

/*
 * - `source` NULL means: no grid. terminal_frame_valid() is then false.
 * - `cols` and `rows` are the snapshot size.
 * - `cursor_y` < 0 means: draw no cursor. terminal_frame_init() sets -1
 *   when the cursor is hidden or the caller asks for no cursor.
 * - `cursor_style` is an enum wsap_vtcore_cursor_style value.
 * - `has_selection` is true when some row has a selection span. It lets
 *   rows skip the selection lookup.
 * - The struct fills exactly one 64-byte cache line.
 */
struct terminal_frame {
    const struct wsap_vtcore_snapshot *source;
    uint64_t palette_gen;
    int cols, rows;
    uint32_t default_fg, default_bg;
    uint32_t cursor_color;
    int cursor_x, cursor_y;
    int cursor_style;
    uint32_t selection_fg, selection_bg;
    bool reverse_video;
    bool has_selection;
};

_Static_assert(sizeof(struct terminal_frame) == 64,
               "terminal_frame must fit in exactly one 64-byte cache line");

/*
 * Fills `frame` from `snap`. It keeps no reference to the caller.
 * A NULL snapshot or an empty view gives an invalid frame. The selection
 * colors stay zero. Set them after this call.
 */
void terminal_frame_init(struct terminal_frame *frame, const struct wsap_vtcore_snapshot *snap,
                         bool cursor_visible);
bool terminal_frame_valid(const struct terminal_frame *frame);

/* The cells of one row, or NULL when the row is outside the snapshot. */
const struct wsap_vtcore_cell *terminal_frame_row(const struct terminal_frame *frame, int row);
/* The content key of one row. It is 0 when the row is outside the snapshot. */
uint64_t terminal_frame_row_key(const struct terminal_frame *frame, int row);
/*
 * The selected columns of one row as a half-open span [begin, end),
 * clipped to [0, cols). An empty span means no selection on this row.
 */
struct wsap_vtcore_span terminal_frame_row_selection(const struct terminal_frame *frame, int row);
/* Resolves a cell color through the palette of the frame. */
uint32_t terminal_frame_color(const struct terminal_frame *frame, wsap_vtcore_color c);
/*
 * Converts cell text to glyph text: a code point, or a glyph_intern()
 * handle for a cluster. A cluster that the snapshot cannot expand gives ' '.
 * Thread safe. glyph_intern() takes a lock.
 */
uint32_t terminal_frame_glyph(const struct terminal_frame *frame, uint32_t text);

static inline uint32_t terminal_frame_selection_fg(const struct terminal_frame *frame)
{
    return frame->selection_fg ? frame->selection_fg : TERMINAL_FRAME_SELECTION_FG;
}

static inline uint32_t terminal_frame_selection_bg(const struct terminal_frame *frame)
{
    return frame->selection_bg ? frame->selection_bg : TERMINAL_FRAME_SELECTION_BG;
}

#endif
