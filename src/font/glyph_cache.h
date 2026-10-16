#ifndef GLYPH_CACHE_H
#define GLYPH_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "font/font.h"

/* Loads the fonts and makes font_instance_default(). Returns false when
 * the backend or the default instance fails. */
bool font_system_init(void);
void font_system_shutdown(void);
/* Rebuilds the cache. Reopens the face first when the font changed.
 * Glyph pointers from earlier fetches become invalid. */
void glyph_cache_rebuild(struct font_instance *fi);
/* Starts a frame of fi. Sets the default colors that tiles blend between.
 * Rebuilds fi when the font changed or when its tiles use other colors.
 * Call it before the frame fetches glyphs, never from a paint worker.
 * The tile colors are process-global. One frame has one color pair. */
void glyph_cache_begin_frame(struct font_instance *fi, uint32_t default_fg,
                             uint32_t default_bg);
/* Rebuilds fi when the font changed. Call once per frame, before the frame
 * fetches glyphs. Never call it from an emit callback. It does nothing in
 * parallel mode. It does not check the tile colors. */
void glyph_cache_refresh(struct font_instance *fi);

void glyph_cache_set_parallel(bool on);
void glyph_cache_flush_deferred(void);

/* Free the run cache entries retired during the frame. Also free this
 * thread's deferred glyph and tile buffers. Call this only where no reader
 * can be running, so after every paint worker has joined. */
void glyph_cache_flush_frame(void);
void glyph_cache_thread_detach(void);

/* Keeps the glyph_style() bits of style: bold, italic, emoji, text, and
 * wide. The presentation bits pick the face of one glyph, and the wide
 * bit sets its width. glyph_cache_shape_segment() keeps fewer bits.
 *
 * The result can be thread scratch. Then it is valid until the next fetch
 * on this thread. Otherwise it is a cache slot. In serial mode a refill
 * rewrites the slot in place. In parallel mode a refill makes a new
 * buffer, and the old one stays valid until glyph_cache_flush_frame().
 *
 * A fetch never rebuilds. After a font change, and before the frame-start
 * rebuild, a fetch can return a glyph of the old face. Nothing is freed,
 * so the memory stays valid. The same applies to glyph_cache_fetch_tile()
 * and glyph_cache_shape_segment(). */
const struct glyph *glyph_cache_fetch(struct font_instance *fi, uint32_t cp,
                                      uint16_t style);
/* The glyph pre-blended over the colors of glyph_cache_begin_frame(), as
 * cell_w * cell_h pixels. Does not check the colors. Returns NULL for a glyph that a
 * tile cannot show (a color glyph, or a glyph that does not fill exactly
 * one cell), for a code point above GLYPH_KEY_CP_MAX (U+10FFFF), and on
 * allocation failure. The same lifetime rules as glyph_cache_fetch() apply. */
const uint32_t *glyph_cache_fetch_tile(struct font_instance *fi,
                                       uint32_t cp, uint16_t style);
/* Cells of a segment that show shaped glyphs. */
struct glyph_span {
    int start, count;
    const struct glyph *cells;
    /* The cells pre-blended over the colors of glyph_cache_begin_frame(),
     * count tiles of cell_w * cell_h pixels, or NULL. Nothing here checks
     * the colors. Only glyph_cache_begin_frame() keeps them current. */
    const uint32_t *tiles;
    /* Equal for spans with equal cells. A GPU atlas can key on it. */
    uint64_t id;
};

typedef void (*glyph_span_fn)(void *ctx, const struct glyph_span *span);

/* Shapes a segment: cells of one style and one pair of colors, each one
 * cell wide, none blank. Calls emit for each span whose cells show shaped
 * glyphs, in order. The span is valid only during the call. Every other
 * cell shows its own glyph from glyph_cache_fetch().
 *
 * Keeps only the FONT_STYLE_VARIANT_MASK bits of style. Its cells are one
 * cell wide, and the run cache keys a shaped run on the face variant only.
 *
 * It keeps scratch in thread-local buffers (seg_canon, seg_roles,
 * priv_run_cells). emit must not call glyph_cache_shape_segment(): a
 * nested call overwrites that scratch, and a longer segment frees it. */
void glyph_cache_shape_segment(struct font_instance *fi, const uint32_t *cps, int count,
                               uint16_t style, glyph_span_fn emit, void *ctx);

#endif
