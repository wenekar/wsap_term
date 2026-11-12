#ifndef GLYPH_CACHE_H
#define GLYPH_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "font/font.h"

/* Initializes the global font and glyph subsystem. */
bool font_system_init(void);
/* Shuts down the global font and glyph subsystem. */
void font_system_shutdown(void);
/* Rebuilds cached glyph entries for font instance `fi`. */
void glyph_cache_rebuild(struct font_instance *fi);
/* Prepares glyph cache for a new frame with default foreground and background colors. */
void glyph_cache_begin_frame(struct font_instance *fi, uint32_t default_fg,
                             uint32_t default_bg);
/* Updates active glyph cache tables for font instance `fi`. */
void glyph_cache_refresh(struct font_instance *fi);

/* Enables or disables parallel glyph rasterization workers. */
void glyph_cache_set_parallel(bool on);
/* Flushes deferred glyph worker tasks. */
void glyph_cache_flush_deferred(void);

/* Flushes completed frame cache entries. */
void glyph_cache_flush_frame(void);
/* Detaches the calling thread from glyph cache worker pools. */
void glyph_cache_thread_detach(void);

/* Looks up or rasterizes the glyph for code point `cp` with `style`. */
const struct glyph *glyph_cache_fetch(struct font_instance *fi, uint32_t cp,
                                      uint16_t style);
/* Looks up or creates the color tile for code point `cp` with `style`. */
const uint32_t *glyph_cache_fetch_tile(struct font_instance *fi,
                                       uint32_t cp, uint16_t style);
struct glyph_span {
    int start, count;
    const struct glyph *cells;
    const uint32_t *tiles;
    uint64_t id;
};

typedef void (*glyph_span_fn)(void *ctx, const struct glyph_span *span);

/* Shapes segment code points and invokes `emit` for each shaped span. */
void glyph_cache_shape_segment(struct font_instance *fi, const uint32_t *cps, int count,
                               uint16_t style, glyph_span_fn emit, void *ctx);

/*
 * Reserves scratch buffer space for `count` code points on the calling thread.
 * Returns true on success, or false when allocation fails.
 */
bool glyph_cache_reserve_segment(int count);

#endif
