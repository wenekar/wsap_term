#ifndef RENDER_FRAME_WALK_H
#define RENDER_FRAME_WALK_H

/*
 * The row walk. It resolves each cell of a row to the final values that a
 * rasterizer draws: colors, glyph, decorations, and cursor. All cell policy
 * is here. The CPU raster and the GPU grid builder read only this output.
 *
 * The walk streams. It calls a function for each column. A glyph pointer is
 * valid only inside that call. No consumer keeps one.
 */

#include <stdbool.h>
#include <stdint.h>

#include "font/font.h"
#include "font/glyph_cache.h"
#include "render/term_frame_next.h"
#include "util/hash.h"

/*
 * render_cell.flags, bit by bit:
 *
 *   bit  0      BLANK: no glyph. The cell draws its bg and its marks only.
 *   bit  1      FAINT: halve the coverage of the glyph and the decorations.
 *   bits 2..4   UNDERLINE: an enum wsap_vtcore_underline value, 0..5.
 *   bit  5      STRIKE
 *   bit  6      OVERLINE
 *   bit  7      LINK: the cell is a hyperlink. It adds no pixels itself.
 *   bit  8      CURSOR_BAR: a bar cursor at the left edge, in cursor_color.
 *   bit  9      CURSOR_UNDERLINE: an underline cursor, in cursor_color.
 *   bit 10      WIDE: the left half of a two-column glyph.
 *   bit 11      WIDE_TAIL: the right half of a two-column glyph.
 *   bits 12..15 zero.
 */
#define RENDER_CELL_BLANK            (1u << 0)
#define RENDER_CELL_FAINT            (1u << 1)
#define RENDER_CELL_UNDERLINE_SHIFT  2
#define RENDER_CELL_UNDERLINE_MASK   (7u << RENDER_CELL_UNDERLINE_SHIFT)
#define RENDER_CELL_STRIKE           (1u << 5)
#define RENDER_CELL_OVERLINE         (1u << 6)
#define RENDER_CELL_LINK             (1u << 7)
#define RENDER_CELL_CURSOR_BAR       (1u << 8)
#define RENDER_CELL_CURSOR_UNDERLINE (1u << 9)
#define RENDER_CELL_WIDE             (1u << 10)
#define RENDER_CELL_WIDE_TAIL        (1u << 11)

/* Decoration bits: a cell with one of them is not a plain fill. */
#define RENDER_CELL_DECOR_MASK \
    (RENDER_CELL_UNDERLINE_MASK | RENDER_CELL_STRIKE | RENDER_CELL_OVERLINE)
#define RENDER_CELL_CURSOR_MASK \
    (RENDER_CELL_CURSOR_BAR | RENDER_CELL_CURSOR_UNDERLINE)
/*
 * The bits that change the pixels of a cell for a given glyph id, half, and
 * color pair. The cell cache key keeps only these bits. BLANK is implied by
 * the glyph id. WIDE and WIDE_TAIL are implied by the style and the half.
 * LINK adds no pixels.
 */
#define RENDER_CELL_PIXEL_MASK \
    (RENDER_CELL_FAINT | RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)

_Static_assert(WSAP_VTCORE_UNDERLINE_DASHED < 8, "the underline kind fits in three bits");

static inline enum wsap_vtcore_underline render_cell_underline(uint16_t flags)
{
    return (enum wsap_vtcore_underline)((flags & RENDER_CELL_UNDERLINE_MASK) >>
                                        RENDER_CELL_UNDERLINE_SHIFT);
}

/*
 * One resolved cell. 16 bytes.
 *
 * - `fg`, `bg`: the final colors, as the cell draws them. The walk
 *   applies, in this order:
 *   1. the palette (terminal_frame_color());
 *   2. reverse video (DECSCNM): swap fg and bg;
 *   3. the selection: fg = terminal_frame_selection_fg(),
 *      bg = terminal_frame_selection_bg();
 *   4. a block cursor: fg = (bg == cursor_color) ? default_bg : bg, then
 *      bg = cursor_color.
 *   A never-written cell (text 0, width 1) starts from the default colors
 *   and has no attributes. Steps 2 to 4 still apply to it.
 *   This is the policy of the old cell.h, resolve_cell_bg(), and
 *   paint_cell(), in one place.
 * - `text`: the glyph text. It is a code point, or a glyph_intern() handle
 *   (above U+10FFFF) for a cluster. A BLANK cell has ' '.
 * - `style`: FONT_STYLE_* bits for glyph_cache_fetch(). A BLANK cell has 0.
 *   A two-column glyph has FONT_STYLE_WIDE on both halves.
 * - `flags`: RENDER_CELL_* bits.
 */
struct render_cell {
    uint32_t fg, bg;
    uint32_t text;
    uint16_t style;
    uint16_t flags;
};

_Static_assert(sizeof(struct render_cell) == 16, "a render_cell is 16 bytes");

/*
 * Glyph ids. A glyph id names the pixels of a glyph for one font instance
 * version. Two cells with the same id, font version, half, colors, and pixel
 * flags have the same pixels. The cell cache and the GPU atlas use it.
 *
 * - 0 means: do not cache this cell. For a cluster, the walk reads the
 *   epoch before and after terminal_frame_glyph(). When the two differ,
 *   handles were reused during the intern, and the id is 0. The id uses the
 *   epoch read after the intern. So a later reuse makes the old key stale
 *   instead of wrong.
 * - Unshaped cell: text | style << 32 | epoch << 48. The epoch is
 *   glyph_intern_epoch() & 0x7FFF for a cluster handle, and 0 for a code
 *   point. Bit 63 is 0. A code point keeps its id when handles are reused.
 * - Shaped cell: hash64_finish(span.id ^ index) | 1 << 63, where index is
 *   the cell position inside the glyph_span. span.id names the shaped run
 *   and the offset of the span in it.
 * - BLANK cell: RENDER_GLYPH_ID_BLANK. That is the unshaped id of ' '.
 *
 * An epoch reuse after 32768 resets could repeat an id. Each reset needs
 * 65536 new clusters, so a tile of that age is long evicted.
 */
#define RENDER_GLYPH_ID_SHAPED (UINT64_C(1) << 63)
#define RENDER_GLYPH_ID_BLANK  ((uint64_t)' ')
#define RENDER_GLYPH_EPOCH_MASK 0x7FFFu

static inline uint64_t render_glyph_id_unshaped(uint32_t text, uint16_t style, uint32_t epoch)
{
    uint64_t e = text > 0x10FFFFu ? (uint64_t)(epoch & RENDER_GLYPH_EPOCH_MASK) : 0;
    return (uint64_t)text | (uint64_t)style << 32 | e << 48;
}

static inline uint64_t render_glyph_id_shaped(uint64_t span_id, int index)
{
    return hash64_finish(span_id ^ (uint64_t)(uint32_t)index) | RENDER_GLYPH_ID_SHAPED;
}

/*
 * The glyph of one cell. Valid only inside the render_cell_fn call that
 * receives it.
 *
 * - `g`: the glyph, or NULL. It is NULL for a BLANK cell. It is also NULL
 *   in a RENDER_WALK_PEEK walk for an unshaped cell: call
 *   render_glyph_get() to fetch it.
 * - `id`: the glyph id (see above).
 * - A shaped cell always has `g` set. Its glyph comes from the span.
 * - Both halves of a two-column glyph get the same glyph and the same id.
 *   The consumer draws the half that its column shows.
 * - The glyph can have no ink (width 0 or no row masks). Draw it as no
 *   glyph then.
 * - The other fields are private to the walk.
 */
struct render_glyph_ref {
    const struct glyph *g;
    uint64_t id;
    struct font_instance *font_;
    uint32_t text_;
    uint16_t style_;
    bool blank_;
};

/*
 * Returns the glyph of the cell. In a peek walk it fetches the glyph from
 * the font. Each call fetches again, so call it once per column. The result
 * is valid until the callback returns or until the next glyph fetch on this
 * thread, whichever comes first.
 */
static inline const struct glyph *render_glyph_get(const struct render_glyph_ref *ref)
{
    if (ref->g || ref->blank_)
        return ref->g;
    return glyph_cache_fetch(ref->font_, ref->text_, ref->style_);
}

/*
 * Called for each column of the walk that no render_run_fn call covers,
 * in increasing column order. `col` is the grid column. The callback must not call
 * glyph_cache_shape_segment() or any rebuilding font function (see
 * docs/font-api.md, "Re-entrancy"). It can call render_glyph_get() and
 * glyph_cache_fetch().
 */
typedef void (*render_cell_fn)(void *ctx, int col, const struct render_cell *cell,
                               const struct render_glyph_ref *glyph);

/*
 * Called for a run of blank cells in place of one render_cell_fn call for
 * each of its columns. Each column of [c0, c1) has a render_cell with flags
 * RENDER_CELL_BLANK and no other bit, text ' ', style 0, and this `bg`.
 * Such a cell draws only its bg. Its fg has no effect on pixels, so the run
 * does not give it. The walk can also send such a cell to render_cell_fn.
 */
typedef void (*render_run_fn)(void *ctx, int c0, int c1, uint32_t bg);

/* Walk flags. */
enum render_walk_flags {
    /* Do not fetch unshaped glyphs before the callback. The callback asks
     * with render_glyph_get() only when it needs the glyph, such as on a
     * cache miss. */
    RENDER_WALK_PEEK = 1u << 0,
};

/*
 * Walks row `y` of `f`, columns [c0, c1), and calls `fn` for each column.
 * With `run` non-NULL, it calls `run` for runs of blank cells instead (see
 * render_run_fn). The calls together cover each column once, in increasing
 * column order.
 *
 * - The walk clamps [c0, c1) to [0, f->cols). A row outside the snapshot
 *   gives blank cells in the default colors, after reverse video.
 * - The output for a column does not depend on c0 and c1. The walk shapes
 *   each whole segment that touches [c0, c1), also the part outside the
 *   range. It calls `fn` only for the columns inside the range.
 * - A segment is a run of shapeable cells (text above ' ', no cluster,
 *   width 1, no cell-aligned symbol) with the same raw fg, raw bg, and
 *   font variant. This is the rule of the old renderer.
 * - Inside a shaped span, `fn` runs inside the glyph_span emit callback.
 *   For other cells, `fn` runs right after the glyph fetch, or with no fetch
 *   in a peek walk.
 * - A WIDE_TAIL cell takes text, style, and decoration bits from the cell
 *   to its left. Its colors resolve at its own column. A width-0 cell with
 *   no wide cell to its left is a BLANK cell.
 * - A tail at c0 still gets its glyph: the walk reads column c0 - 1.
 * - LINK with no underline kind sets the SINGLE kind.
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
 */
void render_row_walk(struct font_instance *fi, const struct terminal_frame *f, int y,
                     int c0, int c1, unsigned flags, render_cell_fn fn, render_run_fn run,
                     void *ctx);

/*
 * Frees the scratch of the calling thread. Pool workers call it before they
 * exit. A later walk on the same thread allocates it again.
 */
void render_walk_thread_release(void);

#endif
