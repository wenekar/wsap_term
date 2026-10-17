#ifndef RENDER_PLAN_H
#define RENDER_PLAN_H

/*
 * The frame planner. It decides, from keys and spans only, what each grid
 * row needs, where the UI repaints, and what the damage is. It touches no
 * pixel, no font, and no global. It allocates nothing: the caller gives
 * every output array. Unit tests drive it through this header alone.
 *
 * Words
 * - canvas row r of the grid: the band of pixels
 *   [grid.y + r * ch, grid.y + (r + 1) * ch).
 * - bottom margin: the band [grid.y + rows * ch, grid.y + grid.height). It
 *   is the pseudo-row with index `rows`. Its content key never changes.
 * - UI ink of a row: the x-span (grid-local) of every UI op that can draw
 *   over the band of that row. One span for each row: the union of the ops.
 *
 * Rules
 * 1. Invalid frame (in->invalid): every row paints, the UI region is the
 *    whole canvas, and the damage is all.
 * 2. Per row, with new key k = new_keys[r]:
 *    - keep, when old_keys[r] == k;
 *    - move, when k sits at r - shift in old_keys, for the dominant shift;
 *    - paint, in every other case.
 *    A key of 0 never matches.
 * 3. Dominant shift: each changed row r whose key sits at some old row r'
 *    votes for r - r'. The shift with the most votes wins. A tie goes to the
 *    smaller |shift|, then to the negative one. A row whose key sits at a
 *    row of another shift paints. Overlaid rows may move: the terminal
 *    scrollbar is an overlay that flashes on every wheel scroll.
 * 4. Moves: consecutive moved rows join into one move. The moves are in an
 *    order where no move reads rows that an earlier move wrote.
 * 5. UI regions: ui_damage (NULL: the whole canvas) plus the band of each
 *    painted and moved row, clipped to the canvas. Each region that crosses
 *    the grid rows grows to whole row bands. Only y grows, also for a region
 *    beside the grid. The bottom margin is one band.
 *    The grid repaints whole bands, so the UI must repaint them too. Then
 *    the regions merge with
 *    ui_damage_add() into at most UI_DAMAGE_MAX_RECTS rects. Merging keeps
 *    the rects row-aligned.
 * 6. Overlay windows: the old UI ink is baked into kept and moved rows. The
 *    UI pass must not blend over it twice. So for each UI region q and each
 *    kept or moved row r inside q, the window is the x-range of q
 *    intersected with the old ink of r. For a moved row, the old ink is the
 *    ink of its source row. The bottom margin counts as a kept row. Each
 *    window repaints the grid before the UI pass.
 *    The new ink needs no window: it lands on clean grid pixels or on
 *    pixels that a window restores.
 * 7. Output damage: the bands of the painted and moved rows, merged into
 *    runs of consecutive rows, plus ui_damage, clipped to the canvas. A
 *    window outside these rects gives the same pixels as before, so it adds
 *    no damage. On overflow the list becomes one rect: its bounds.
 *
 * The caller paints the UI in each region: it fills `base` in the part of
 * the region outside the grid rect, then paints the scene clipped to the
 * region and to the scene bounds.
 */

#include <stdbool.h>
#include <stdint.h>

#include "paint/damage.h"
#include "util/damage.h"
#include "util/geom.h"

struct ui_scene;

/* A half-open x range [x0, x1) in grid-local pixels. Empty when x1 <= x0. */
struct plan_span {
    int x0, x1;
};

static inline bool plan_span_empty(struct plan_span s)
{
    return s.x1 <= s.x0;
}

enum plan_row_op : uint8_t {
    PLAN_KEEP,
    PLAN_MOVE,
    PLAN_PAINT,
};

struct plan_row {
    enum plan_row_op op;
    int src;
};

/* Canvas rows [src, src + count) move to [dst, dst + count). */
struct plan_move {
    int dst, src, count;
};

/* Repaint grid row `row` (rows: the bottom margin) inside `x`. */
struct plan_window {
    int row;
    struct plan_span x;
};

/* The window capacity that plan_frame() can need for `rows` rows. */
#define PLAN_WINDOWS_MAX(rows) (((rows) + 1) * UI_DAMAGE_MAX_RECTS)

/*
 * - `canvas`: the canvas size.
 * - `grid`: the grid rect, already clipped to the canvas. An empty rect
 *   means no grid. rows must then be 0. A grid smaller than one cell has
 *   rows 0: its bottom margin is the whole rect.
 * - `rows`, `ch`: the visible rows and the cell height.
 * - `invalid`: the surface state is unknown. See rule 1.
 * - `old_keys[rows]`: the row keys that the canvas shows now. Ignored when
 *   invalid.
 * - `new_keys[rows]`: the row keys of this frame.
 * - `old_ui[rows + 1]`: the UI ink in the canvas now, one span per row and
 *   one for the bottom margin. Ignored when invalid.
 * - `ui_damage`: NULL means all of the UI changed.
 */
struct plan_input {
    struct vector2i canvas;
    struct rect2i grid;
    int rows;
    int ch;
    bool invalid;
    const uint64_t *old_keys;
    const uint64_t *new_keys;
    const struct plan_span *old_ui;
    const struct ui_damage *ui_damage;
};

/*
 * The caller sets the array pointers. plan_frame() fills the rest.
 * - `rows[in->rows]`: one op for each row. src is set for PLAN_MOVE.
 * - `moves[in->rows]`: n_moves blocks, in execution order.
 * - `windows[PLAN_WINDOWS_MAX(in->rows)]`: n_windows windows, ordered by
 *   row, then by x. The windows of one row do not touch. plan_frame() also
 *   uses this array as scratch space.
 * - `ui`: the UI regions. ui.all is true only when the region is the whole
 *   canvas. Then ui.rects[0] is the canvas rect.
 * - `damage`: the output damage. damage.all is true for an invalid frame.
 *   Then damage.rects[0] is the canvas rect.
 * - `shift`: the dominant shift, 0 when no row moves.
 * - `all`: the frame is invalid. Every row is PLAN_PAINT.
 */
struct plan_output {
    struct plan_row *rows;
    struct plan_move *moves;
    struct plan_window *windows;
    int n_moves;
    int n_windows;
    struct ui_damage ui;
    struct damage_list damage;
    int shift;
    bool all;
};

void plan_frame(const struct plan_input *in, struct plan_output *out);

/*
 * Computes the UI ink of each row of a grid: out[rows + 1]. Each op adds its
 * ink rect, clipped by its clip stack and by the grid rect, to every row
 * band that it touches. An op that draws nothing (alpha 0) adds no ink.
 * Spans are grid-local. A NULL or invalid scene gives no ink. Pure.
 */
void plan_ui_ink(const struct ui_scene *scene, struct rect2i grid, int ch, int rows,
                 struct plan_span *out);

#endif
