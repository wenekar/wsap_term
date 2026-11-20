#ifndef GLYPH_CACHE_H
#define GLYPH_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "font/font.h"

/* Initializes the global font and glyph subsystem. Returns false when the
 * backend or the default font instance cannot be created. */
bool font_system_init(void);
/* Frees every font instance and shuts down the font backend. */
void font_system_shutdown(void);
/* Reopens the face of `fi` when the font generation changed and drops its
 * cached glyphs, runs, and tiles. Requires serial execution, because paint
 * workers read cache memory. */
void glyph_cache_rebuild(struct font_instance *fi);
/* Sets the foreground and background colors of the frame tiles and refreshes
 * `fi`. A color or generation change rebuilds the instance in serial mode. */
void glyph_cache_begin_frame(struct font_instance *fi, uint32_t default_fg,
                             uint32_t default_bg);
/* Rebuilds font instance `fi` when the font generation changed. Does nothing
 * in parallel mode. */
void glyph_cache_refresh(struct font_instance *fi);

/* Selects parallel cache mode. A refill then publishes a new buffer and defers
 * the replaced buffer to its thread, and instances stop rebuilding until
 * parallel mode is off. */
void glyph_cache_set_parallel(bool on);
/* Frees the buffers that the calling thread deferred and returns their blocks
 * to its pool. */
void glyph_cache_flush_deferred(void);

/* Frees the run entries that any thread retired and the deferred buffers of
 * the calling thread. Call it after the worker batch finishes. */
void glyph_cache_flush_frame(void);
/* Frees the pooled blocks and the scratch glyph, segment, and tile storage of
 * the calling thread. */
void glyph_cache_thread_detach(void);

/* Returns the borrowed glyph for code point `cp` with `style`, fetching or
 * rasterizing it on a miss. A rebuild of `fi` or a later fetch on the same
 * thread can invalidate the result. */
const struct glyph *glyph_cache_fetch(struct font_instance *fi, uint32_t cp,
                                      uint16_t style);
/*
 * Fetches the glyph for code point `cp` with `style` at `phase` quarter
 * pixels. The key holds the phase, so a placed glyph and the cell glyph do not
 * share an entry. The result follows the lifetime rule of
 * glyph_cache_fetch().
 */
const struct glyph *glyph_cache_fetch_at(struct font_instance *fi, uint32_t cp,
                                         uint16_t style, uint8_t phase);
/* Returns the borrowed tile of code point `cp` with `style` blended with the
 * current tile colors. Returns NULL when the glyph cannot use a tile, the code
 * point is above the cache key range, or allocation fails. */
const uint32_t *glyph_cache_fetch_tile(struct font_instance *fi,
                                       uint32_t cp, uint16_t style);
struct glyph_span {
    int start, count;
    const struct glyph *cells;
    const uint32_t *tiles;
    uint64_t id;
};

typedef void (*glyph_span_fn)(void *ctx, const struct glyph_span *span);

/*
 * Shapes segment code points and invokes `emit` for each run of shaped
 * columns. A span starts at its index in `cps`, and a segment of fewer than
 * two code points emits nothing.
 *
 * Span cells and tiles stay valid until `emit` returns. `emit` can call
 * glyph_cache_fetch() and glyph_cache_fetch_tile(). It must not call
 * glyph_cache_shape_segment() recursively or rebuild an instance: those reuse
 * the thread scratch that holds the segment.
 */
void glyph_cache_shape_segment(struct font_instance *fi, const uint32_t *cps, int count,
                               uint16_t style, glyph_span_fn emit, void *ctx);

/*
 * Reserves scratch buffer space for `count` code points on the calling thread.
 * Returns true on success, or false when allocation fails.
 */
bool glyph_cache_reserve_segment(int count);

#endif
