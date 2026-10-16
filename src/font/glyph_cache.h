#ifndef GLYPH_CACHE_H
#define GLYPH_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "font/font.h"

bool font_system_init(void);
void font_system_shutdown(void);
void glyph_cache_rebuild(struct font_instance *fi);
void glyph_cache_begin_frame(struct font_instance *fi, uint32_t default_fg,
                             uint32_t default_bg);
void glyph_cache_refresh(struct font_instance *fi);

void glyph_cache_set_parallel(bool on);
void glyph_cache_flush_deferred(void);

void glyph_cache_flush_frame(void);
void glyph_cache_thread_detach(void);

const struct glyph *glyph_cache_fetch(struct font_instance *fi, uint32_t cp,
                                      uint16_t style);
const uint32_t *glyph_cache_fetch_tile(struct font_instance *fi,
                                       uint32_t cp, uint16_t style);
struct glyph_span {
    int start, count;
    const struct glyph *cells;
    const uint32_t *tiles;
    uint64_t id;
};

typedef void (*glyph_span_fn)(void *ctx, const struct glyph_span *span);

void glyph_cache_shape_segment(struct font_instance *fi, const uint32_t *cps, int count,
                               uint16_t style, glyph_span_fn emit, void *ctx);

#endif
