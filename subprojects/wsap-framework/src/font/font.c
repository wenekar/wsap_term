#define _GNU_SOURCE
#include "font/backend.h"
#include "font/glyph/internal.h"
#include "font/internal.h"
#if !defined(__APPLE__) || defined(WSAP_USE_FREETYPE)
#include "font/backends/freetype/internal.h"
#endif
#include "font/cell_draw.h"
#include "font/glyph/intern.h"
#include "util/unicode/grapheme.h"
#include "util/unicode/width.h"

#include <stdlib.h>
#include <string.h>

__thread struct font_metrics font_thread_metrics = {
    .cell_w = (FONT_DEFAULT_PIXEL_SIZE * 3 + 4) / 5,
    .cell_h = FONT_DEFAULT_PIXEL_SIZE * 5 / 4,
    .baseline = FONT_DEFAULT_PIXEL_SIZE,
    .pixel_size = FONT_DEFAULT_PIXEL_SIZE,
};
/* Owned family list of the configured default. Replaced, never edited. */
static char *configured_family;
static int configured_size_pt = FONT_DEFAULT_SIZE_PT;
static _Atomic uint64_t font_generation_counter = 1;

const char *font_get_family(void)
{
    return configured_family ? configured_family : FONT_DEFAULT_FAMILY;
}

/* Prepares `g` for one glyph of a font with metrics `m`. */
static bool reset_glyph(const struct font_metrics *m, struct glyph *g, uint16_t style)
{
    if (!g)
        return false;
    int w = m->cell_w * ((style & FONT_STYLE_WIDE) ? 2 : 1);
    if (!glyph_reset(g, (unsigned)w, (unsigned)m->cell_h))
        return false;
    /* A glyph that does not report a pen advance fills its columns. */
    g->advance = (int16_t)w;
    return true;
}

uint16_t font_instance_weight(const struct font_instance *fi)
{
    return fi ? fi->weight : FONT_WEIGHT_REGULAR;
}

bool font_instance_monospace(const struct font_instance *fi)
{
    return fi ? !fi->proportional : true;
}

bool font_prefers_emoji(uint32_t cp, uint16_t style)
{
    if (style & FONT_STYLE_EMOJI)
        return true;
    if (style & FONT_STYLE_TEXT)
        return false;
    return unicode_emoji_presentation(cp);
}

bool font_instance_render_glyph(const struct font_instance *fi, uint32_t cp, uint16_t style, struct glyph *g)
{
    return font_instance_render_glyph_at(fi, cp, style, 0, g);
}

bool font_instance_render_glyph_at(const struct font_instance *fi, uint32_t cp,
                                   uint16_t style, uint8_t phase, struct glyph *g)
{
    if (!fi || !g)
        return false;
    font_bind_thread(fi);
    style |= instance_base_style(fi);
    if (!reset_glyph(&fi->m, g, style))
        return false;
    if (cp <= ' ')
        return true;

    uint32_t cps[UNICODE_GRAPHEME_MAX_LEN];
    int len = glyph_intern_expand(cp, cps, UNICODE_GRAPHEME_MAX_LEN);

    uint32_t base = cps[0];
    if (unicode_grapheme_regional_indicator(base) && !(style & FONT_STYLE_EMOJI))
        base = 'A' + (base - 0x1F1E6);
    /* A cell symbol and the 5x7 fallback belong to the cell path. A
     * proportional font draws every glyph from its own faces. */
    bool cell_aligned = !fi->proportional;
    if (cell_aligned && draw_cell_aligned_symbol(base, g))
        return true;

    uint32_t target = len > 1 ? cp : base;

    const struct font_backend *be = FONT_BACKEND;
    if (fi->face && be->rasterize_glyph &&
        be->rasterize_glyph(fi->face, target, style, phase, &fi->m, g))
        return true;

    if (!reset_glyph(&fi->m, g, style))
        return false;
    if (!cell_aligned) {
        /* Leave the glyph empty. It keeps its advance. */
        return true;
    }
    if (base < 128) {
        glyph_render_5x7(base, g);
        return true;
    }
    glyph_render_unknown_box(g);
    return true;
}

bool font_backend_init(void)
{
    const struct font_backend *be = FONT_BACKEND;
    return be->init ? be->init() : true;
}

void font_backend_shutdown(void)
{
    const struct font_backend *be = FONT_BACKEND;
    if (be->destroy)
        be->destroy();
    atomic_fetch_add_explicit(&font_generation_counter, 1, memory_order_relaxed);
}

bool font_thread_attach(void)
{
    const struct font_backend *be = FONT_BACKEND;
    return be->thread_attach ? be->thread_attach() : true;
}

void font_thread_detach(void)
{
    const struct font_backend *be = FONT_BACKEND;
    if (be->thread_detach)
        be->thread_detach();
}

void font_set_family(const char *name)
{
    char *next = strdup(name && name[0] ? name : FONT_DEFAULT_FAMILY);
    if (!next)
        return;
    char *prev = configured_family;
    configured_family = next;
    free(prev);
#if !defined(__APPLE__) || defined(WSAP_USE_FREETYPE)
    ft_reload_family(font_get_family());
#endif
    atomic_fetch_add_explicit(&font_generation_counter, 1, memory_order_relaxed);
}

void font_set_configured_size(int size_pt)
{
    configured_size_pt = size_pt;
}

int font_configured_size(void)
{
    return configured_size_pt;
}

uint64_t font_generation(void)
{
    return atomic_load_explicit(&font_generation_counter, memory_order_acquire);
}

void font_bump_generation(void)
{
    atomic_fetch_add_explicit(&font_generation_counter, 1, memory_order_relaxed);
}

int font_size_px(struct font_size size)
{
    struct font_size r = font_size_resolve(size);
    int64_t px = ((int64_t)FONT_PT_TO_PX(r.pt) * r.scale_120 + 60) / 120;
    if (px < 1) px = 1;
    if (px > UINT16_MAX / 4) px = UINT16_MAX / 4;
    return (int)px;
}

/*
 * The FreeType backend holds one family set for the process. A descriptor
 * that names another list therefore sets the configured family list, as
 * font_set_family() does.
 */
void font_apply_desc_family(const struct font_desc *desc)
{
    if (desc && desc->family_list && desc->family_list[0] &&
        strcmp(desc->family_list, font_get_family()))
        font_set_family(desc->family_list);
}

bool font_measure_desc(const struct font_desc *desc, struct font_metrics *out)
{
    if (!desc)
        return false;
    font_apply_desc_family(desc);
    const struct font_backend *be = FONT_BACKEND;
    font_handle_t *face = be->open ? be->open(desc) : NULL;
    if (!face)
        return false;
    bool ok = be->measure(face, out);
    be->close(face);
    return ok;
}

void font_thread_metrics_set(const struct font_metrics *m)
{
    font_thread_metrics = *m;
}

#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
void face_activate(const struct font_metrics *m)
{
    if (m)
        font_thread_metrics_set(m);
}
#endif

