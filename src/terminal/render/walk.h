#ifndef RENDER_FRAME_WALK_H
#define RENDER_FRAME_WALK_H

/*
 * The terminal row walk. It resolves each cell of a row to the final values
 * that a rasterizer draws: colors, glyph, decorations, and cursor. All
 * terminal cell policy is here. The CPU raster and the GPU grid builder
 * read only the output (render/cell.h).
 *
 * The walk streams. It calls a function for each column. A glyph pointer is
 * valid only inside that call. No consumer keeps one.
 */

#include <stdbool.h>
#include <stdint.h>

#include "font/font.h"
#include "render/cell.h"
#include "terminal/render/term_frame.h"

/*
 * Walks row `y` of `f`, columns [c0, c1), and calls the sink for each
 * column: `cell` for one column, or `run`, `text`, and `span` for runs
 * (see render_run_fn, render_text_fn, and render_span_fn). The calls
 * together cover each column once, in increasing column order.
 *
 * - The walk clamps [c0, c1) to [0, f->cols). A row outside the snapshot
 *   gives blank cells in the default colors, after reverse video.
 * - The output for a column does not depend on c0 and c1. The walk shapes
 *   each whole segment that touches [c0, c1), also the part outside the
 *   range. It calls the sink only for the columns inside the range.
 * - A segment is a run of shapeable cells (text above ' ', no cluster,
 *   width 1, no cell-aligned symbol) with the same raw fg, raw bg, and
 *   font variant. This is the rule of the old renderer.
 * - Inside a shaped span, `cell` runs inside the glyph_span emit callback.
 *   For other cells, `cell` runs right after the glyph fetch, or with no
 *   fetch in a peek walk.
 * - A WIDE_TAIL cell takes text, style, and decoration bits from the cell
 *   to its left. Its colors resolve at its own column. A width-0 cell with
 *   no wide cell to its left is a BLANK cell.
 * - A tail at c0 still gets its glyph: the walk reads column c0 - 1.
 * - LINK with no underline kind sets the SINGLE kind.
 * - A cell with the conceal attribute, or with the blink attribute and a
 *   false f->blink_on, is BLANK. It draws only its bg and the row policy
 *   marks (selection and cursor).
 * - An underline uses the SGR 58 color of its column when the snapshot has
 *   one. Then UL_COLOR is set and `ul` holds the resolved color.
 * - FAINT is set only on a cell that has a glyph or a decoration.
 * - The cursor exists when f->cursor_y == y and 0 <= f->cursor_x < cols.
 *   The cells that it covers are the cell at cursor_x, plus the other half
 *   when that cell is half of a two-column glyph. A block cursor changes
 *   the colors of each covered cell. CURSOR_UNDERLINE goes on each covered
 *   cell. CURSOR_BAR goes only on the left covered cell.
 * - Rows without selection, cursor, or reverse video take a fast path.
 *
 * Threads: any thread, with glyph_cache_set_parallel(true) on. Each thread
 * keeps its own scratch for the segment code points. The renderer calls the
 * walk in parallel mode also with 0 workers, so 0 and N workers take the
 * same font path.
 *
 * A segment has at most f->cols cells. After render_walk_thread_reserve()
 * with f->cols or more, a walk of `f` allocates no scratch on that thread.
 */
void render_row_walk(struct font_instance *fi, const struct terminal_frame *f, int y,
                     int c0, int c1, unsigned flags, const struct render_walk_sink *sink);

/*
 * Reserves the thread-local scratch storage of a segment of `cols` cells.
 * Returns true on success, or false on allocation failure.
 */
bool render_walk_thread_reserve(int cols);

#endif
