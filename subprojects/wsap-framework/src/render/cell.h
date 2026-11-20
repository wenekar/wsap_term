#ifndef RENDER_CELL_H
#define RENDER_CELL_H

/*
 * One resolved cell for the raster and the GPU grid builder, and the sink
 * that a row walk fills. This header is generic: it names no terminal type.
 * The terminal layer turns its cells into these values.
 *
 * The walk streams. It calls a function for each column. A glyph pointer is
 * valid only inside that call. No consumer keeps one.
 */

#include <stdbool.h>
#include <stdint.h>

#include "font/font.h"
#include "font/glyph_cache.h"
#include "util/hash.h"

/*
 * render_cell.flags, bit by bit:
 *
 *   bit  0      BLANK: no glyph. The cell draws its bg and its marks only.
 *   bit  1      FAINT: halve the coverage of the glyph and the decorations.
 *   bits 2..4   UNDERLINE: an enum render_underline value, 0..5.
 *   bit  5      STRIKE
 *   bit  6      OVERLINE
 *   bit  7      LINK: the cell is a hyperlink. It adds no pixels itself.
 *   bit  8      CURSOR_BAR: a bar cursor at the left edge, in cursor_color.
 *   bit  9      CURSOR_UNDERLINE: an underline cursor, in cursor_color.
 *   bit 10      WIDE: the left half of a two-column glyph.
 *   bit 11      WIDE_TAIL: the right half of a two-column glyph.
 *   bit 12      UL_COLOR: draw the underline in `ul`, not in `fg`.
 *   bits 13..15 zero.
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
#define RENDER_CELL_UL_COLOR         (1u << 12)

/* Decoration bits: a cell with one of them is not a plain fill. */
#define RENDER_CELL_DECOR_MASK \
    (RENDER_CELL_UNDERLINE_MASK | RENDER_CELL_STRIKE | RENDER_CELL_OVERLINE)
#define RENDER_CELL_CURSOR_MASK \
    (RENDER_CELL_CURSOR_BAR | RENDER_CELL_CURSOR_UNDERLINE)
/*
 * The bits that change the pixels of a cell for a given glyph id, half, and
 * color pair. The cell cache key keeps only these bits. BLANK is implied by
 * the glyph id. WIDE and WIDE_TAIL are implied by the style and the half.
 * LINK adds no pixels. UL_COLOR adds a color that the key does not hold, so
 * the raster does not cache such a cell.
 */
#define RENDER_CELL_PIXEL_MASK \
    (RENDER_CELL_FAINT | RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)

/*
 * The decoration kinds of a cell. The values fit in the three underline bits
 * of render_cell.flags. The terminal layer maps its own kinds to these.
 */
enum render_underline {
    RENDER_UNDERLINE_NONE,
    RENDER_UNDERLINE_SINGLE,
    RENDER_UNDERLINE_DOUBLE,
    RENDER_UNDERLINE_CURLY,
    RENDER_UNDERLINE_DOTTED,
    RENDER_UNDERLINE_DASHED,
};

_Static_assert(RENDER_UNDERLINE_DASHED < 8, "the underline kind fits in three bits");

static inline enum render_underline render_cell_underline(uint16_t flags)
{
    return (enum render_underline)((flags & RENDER_CELL_UNDERLINE_MASK) >>
                                   RENDER_CELL_UNDERLINE_SHIFT);
}

/*
 * The shape of one row. A row that a terminal shows at double size is one of
 * the last three values. The raster and the GPU builder draw it the same way.
 */
enum render_line_size {
    RENDER_LINE_SINGLE,
    RENDER_LINE_DOUBLE_WIDTH,
    RENDER_LINE_DOUBLE_TOP,
    RENDER_LINE_DOUBLE_BOTTOM,
};

/*
 * One resolved cell. 20 bytes.
 *
 * - `fg`, `bg`: the final colors, as the cell draws them. The walk applies,
 *   in this order:
 *   1. the palette;
 *   2. reverse video (DECSCNM): swap fg and bg;
 *   3. the selection;
 *   4. a block cursor: fg = (bg == cursor_color) ? default_bg : bg, then
 *      bg = cursor_color.
 *   A never-written cell starts from the default colors and has no
 *   attributes. Steps 2 to 4 still apply to it.
 * - `text`: the glyph text. It is a code point, or a glyph_intern() handle
 *   (above U+10FFFF) for a cluster. A BLANK cell has ' '.
 * - `ul`: the underline color when flags has UL_COLOR. Zero otherwise.
 * - `style`: FONT_STYLE_* bits for glyph_cache_fetch(). A BLANK cell has 0.
 *   A two-column glyph has FONT_STYLE_WIDE on both halves.
 * - `flags`: RENDER_CELL_* bits.
 */
struct render_cell {
    uint32_t fg, bg;
    uint32_t text;
    uint32_t ul;
    uint16_t style;
    uint16_t flags;
};

_Static_assert(sizeof(struct render_cell) == 20, "a render_cell is 20 bytes");

/*
 * Glyph ids. A glyph id names the pixels of a glyph for one font instance
 * version. Two cells with the same id, font version, half, colors, and pixel
 * flags have the same pixels. The cell cache and the GPU atlas use it.
 *
 * - 0 means: do not cache this cell. For a cluster, the walk reads the
 *   epoch before and after the glyph intern. When the two differ, handles
 *   were reused during the intern, and the id is 0. The id uses the epoch
 *   read after the intern. So a later reuse makes the old key stale instead
 *   of wrong.
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
 * Retrieves the rasterized glyph for `ref`.
 * Fetches from the font glyph cache when `ref` was deferred during a peek walk.
 */
static inline const struct glyph *render_glyph_get(const struct render_glyph_ref *ref)
{
    if (ref->g || ref->blank_)
        return ref->g;
    return glyph_cache_fetch(ref->font_, ref->text_, ref->style_);
}

/*
 * Called for each column of the walk that no run call covers, in
 * increasing column order. `col` is the grid column. The callback must not
 * call glyph_cache_shape_segment() or any rebuilding font function (see
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

/*
 * Called for a run of plain ASCII cells in place of one render_cell_fn call
 * for each of its columns. Column c of [c0, c1) has a render_cell with fg
 * `fg`, bg `bg`, text text[(c - c0) * stride], style `style`, and flags 0.
 * Its text is in ('!', '~'), its style has only FONT_STYLE_VARIANT_MASK
 * bits, and no shaped span covers it. So its glyph id is the unshaped id of
 * its text and style. The text points into the terminal snapshot. It is
 * valid only inside the call.
 *
 * Returns the first column that the call did not draw, in [c0, c1]. The
 * walk sends that column to render_cell_fn, and it can call again for the
 * columns after it.
 */
typedef int (*render_text_fn)(void *ctx, int c0, int c1, uint32_t fg, uint32_t bg,
                              uint16_t style, const uint32_t *text, int stride);

/*
 * Called for a run of cells of one shaped span in place of one
 * render_cell_fn call for each of its columns. Column c of [c0, c1) has a
 * render_cell with fg `fg`, bg `bg`, and flags 0, and the glyph id
 * ids[c - c0]. Its text and style are the ones that the per-cell call
 * would give. The ids are valid only inside the call. The glyphs come
 * from the span: a consumer that needs one returns at its column.
 *
 * Returns the first column that the call did not draw, in [c0, c1]. The
 * walk sends that column to render_cell_fn with its glyph, and it can call
 * again for the columns after it.
 */
typedef int (*render_span_fn)(void *ctx, int c0, int c1, uint32_t fg, uint32_t bg,
                              const uint64_t *ids);

/*
 * The consumer of a walk. `cell` is required. `run`, `text`, and `span`
 * are optional. Without them the walk sends each column to `cell`.
 */
struct render_walk_sink {
    render_cell_fn cell;
    render_run_fn run;
    render_text_fn text;
    render_span_fn span;
    void *ctx;
};

enum render_walk_flags {
    /* Do not fetch unshaped glyphs before the callback. The callback asks
     * with render_glyph_get() only when it needs the glyph, such as on a
     * cache miss. */
    RENDER_WALK_PEEK = 1u << 0,
};

#endif
