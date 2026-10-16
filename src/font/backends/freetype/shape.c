#define _GNU_SOURCE
#include "font/backends/freetype/internal.h"
#include "font/glyph/layout.h"
#include "font/shaping.h"

#include FT_SYNTHESIS_H
#include <hb-ot.h>
#include <hb-aat.h>

#include <stdlib.h>
#include <string.h>

static __thread hb_buffer_t *shape_buffer;

#define RASTER_CACHE_SIZE 16384

struct raster_entry {
    FT_BitmapGlyph glyph;
    int advance_px;
    unsigned slot_reuse;
    uint32_t gid;
    unsigned fit_pass;
    uint16_t slot;
    uint16_t px;
    uint8_t style;
    uint8_t hinting;
    bool valid;
};

_Static_assert(sizeof(struct raster_entry) == 32, "raster_entry grew");
_Static_assert(MAX_FACES <= UINT16_MAX + 1, "face slots fit in 16 bits");

static __thread struct raster_entry *raster_cache;

static __thread unsigned fit_pass;

static __thread uint8_t *neutral_cp;

void raster_cache_clear(void)
{
    if (raster_cache) {
        for (int i = 0; i < RASTER_CACHE_SIZE; i++) {
            if (raster_cache[i].glyph)
                FT_Done_Glyph((FT_Glyph)raster_cache[i].glyph);
            raster_cache[i].valid = false;
            raster_cache[i].glyph = NULL;
        }
    }
}

bool shape_buffer_live(void)
{
    return shape_buffer != NULL;
}

void shape_thread_reset(void)
{
    if (shape_buffer) {
        hb_buffer_destroy(shape_buffer);
        shape_buffer = NULL;
    }
    raster_cache_clear();
    free(raster_cache);
    raster_cache = NULL;
    free(neutral_cp);
    neutral_cp = NULL;
}

static unsigned raster_hash(int slot, uint32_t gid, unsigned style)
{
    uint64_t h = (uint64_t)gid * UINT64_C(0x9e3779b97f4a7c15);
    h ^= (uint64_t)(slot + 1) * UINT64_C(0xbf58476d1ce4e5b9);
    h ^= (uint64_t)(style + 1) * UINT64_C(0x94d049bb133111eb);
    h ^= h >> 29;
    return (unsigned)h & (RASTER_CACHE_SIZE - 1);
}

enum { RASTER_OBLIQUE = 4, RASTER_EMBOLDEN = 8 };
_Static_assert((FONT_STYLE_VARIANT_MASK & (RASTER_OBLIQUE | RASTER_EMBOLDEN)) == 0,
               "the synthesis bits sit above the variant bits");

static const struct raster_entry *raster_fetch(int slot, enum font_variant variant,
        uint32_t gid, bool oblique, bool embolden)
{
    if (slot < 0 || slot >= num_faces || (unsigned)variant >= FONT_VARIANT_COUNT || !gid)
        return NULL;
    if (!raster_cache) {
        raster_cache = calloc(RASTER_CACHE_SIZE, sizeof(*raster_cache));
        if (!raster_cache)
            return NULL;
    }
    FT_Face face = faces[slot].styles[variant];
    if (!face)
        return NULL;

    unsigned bits = (unsigned)variant | (oblique ? RASTER_OBLIQUE : 0) |
                    (embolden ? RASTER_EMBOLDEN : 0);
    unsigned idx = raster_hash(slot, gid, bits ^ ((unsigned)ft_applied_px << 4));
    struct raster_entry *e = &raster_cache[idx];
    for (unsigned p = 0; p < RASTER_CACHE_SIZE; p++) {
        if (e->valid && e->slot == slot &&
            e->slot_reuse == faces[slot].slot_reuse &&
            e->gid == gid && e->style == bits && e->px == ft_applied_px &&
            e->hinting == raster_hint_key()) {
            e->fit_pass = fit_pass;
            return e;
        }
        if (!e->valid || e->fit_pass != fit_pass)
            break;
        idx = (idx + 1) & (RASTER_CACHE_SIZE - 1);
        e = &raster_cache[idx];
    }
    if (e->valid && e->fit_pass == fit_pass)
        return NULL;

    int load_flags = font_ft_load_flags();
    FT_Render_Mode render_mode = font_ft_render_mode();

    if (oblique || embolden) {
        if (FT_Load_Glyph(face, gid, load_flags | FT_LOAD_NO_BITMAP) != 0)
            return NULL;
        if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            if (oblique) FT_GlyphSlot_Oblique(face->glyph);
            if (embolden) FT_GlyphSlot_Embolden(face->glyph);
        }
        if (FT_Render_Glyph(face->glyph, render_mode) != 0)
            return NULL;
    } else {
        bool match = (FT_LOAD_TARGET_MODE(load_flags) == render_mode);
        if (match) {
            if (FT_Load_Glyph(face, gid, load_flags | FT_LOAD_RENDER | FT_LOAD_COLOR) != 0 &&
                (FT_Load_Glyph(face, gid, load_flags | FT_LOAD_COLOR) != 0 ||
                 FT_Render_Glyph(face->glyph, render_mode) != 0))
                return NULL;
        } else {
            if (FT_Load_Glyph(face, gid, load_flags | FT_LOAD_COLOR) != 0 ||
                FT_Render_Glyph(face->glyph, render_mode) != 0)
                return NULL;
        }
    }

    FT_Glyph owned = NULL;
    if (FT_Get_Glyph(face->glyph, &owned) != 0 || !owned)
        return NULL;
    if (owned->format != FT_GLYPH_FORMAT_BITMAP &&
        FT_Glyph_To_Bitmap(&owned, render_mode, NULL, true) != 0) {
        FT_Done_Glyph(owned);
        return NULL;
    }
    if (!owned || owned->format != FT_GLYPH_FORMAT_BITMAP) {
        if (owned) FT_Done_Glyph(owned);
        return NULL;
    }

    if (e->glyph)
        FT_Done_Glyph((FT_Glyph)e->glyph);
    e->glyph = (FT_BitmapGlyph)owned;
    e->advance_px = ft_px_round(face->glyph->advance.x);
    e->px = (uint16_t)ft_applied_px;
    e->slot = (uint16_t)slot;
    e->slot_reuse = faces[slot].slot_reuse;
    e->gid = gid;
    e->style = (uint8_t)bits;
    e->hinting = raster_hint_key();
    e->fit_pass = fit_pass;
    e->valid = true;
    if (FT_HAS_COLOR(face) && !oblique && !embolden &&
        e->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
        colr_rasterize(face, gid, e->glyph);
    return e;
}

static bool script_is_default_shaper(hb_script_t s)
{
    return s != HB_SCRIPT_UNKNOWN && font_layout_script_simple(s);
}

static bool cp_layout_neutral(uint32_t cp)
{
    if (cp >= UNICODE_LIMIT)
        return false;
    uint8_t v = neutral_cp ? (neutral_cp[cp >> 2] >> ((cp & 3) * 2)) & 3 : 0;
    if (v)
        return v == 1;
    bool ok = true;
    hb_unicode_funcs_t *uf = hb_ufuncs();
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0xA960 && cp <= 0xA97C) ||
        (cp >= 0xD7B0 && cp <= 0xD7FB) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0x3130 && cp <= 0x318F))
        ok = false;
    else if (fallback_coverage_exempt(cp))
        ok = false;
    else {
        hb_codepoint_t a = 0, b = 0;
        if (hb_unicode_decompose(uf, cp, &a, &b) ||
            hb_unicode_combining_class(uf, cp) != 0 ||
            hb_unicode_mirroring(uf, cp) != cp ||
            hb_unicode_general_category(uf, cp) ==
            HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR ||
            !script_is_default_shaper(hb_unicode_script(uf, cp)))
            ok = false;
    }
    if (!neutral_cp)
        neutral_cp = calloc(UNICODE_LIMIT / 4, sizeof(*neutral_cp));
    if (neutral_cp) {
        unsigned shift = (cp & 3) * 2;
        neutral_cp[cp >> 2] = (uint8_t)((neutral_cp[cp >> 2] & ~(3u << shift)) |
                                        ((ok ? 1u : 2u) << shift));
    }
    return ok;
}

static hb_set_t *layout_input_gids(int slot, enum font_variant variant)
{
    uint8_t *st = &faces[slot].layout_gids_state[variant];
    hb_set_t **ps = &faces[slot].layout_gids[variant];
    if (*st)
        return *st == 1 ? *ps : NULL;
    *st = 2;
    hb_set_t *s = NULL;
    hb_font_t *hb = face_hb_font(slot, variant);
    hb_face_t *hf = hb ? hb_font_get_face(hb) : NULL;
    if (hf && !hb_aat_layout_has_substitution(hf) &&
        !hb_aat_layout_has_positioning(hf) &&
        !hb_aat_layout_has_tracking(hf)) {
        hb_blob_t *kern = hb_face_reference_table(hf, HB_TAG('k','e','r','n'));
        size_t kern_len = kern ? hb_blob_get_length(kern) : 0;
        hb_blob_destroy(kern);
        if (!kern_len) {
            s = hb_set_create();
            unsigned n =
                hb_ot_layout_table_get_lookup_count(hf, HB_OT_TAG_GSUB);
            for (unsigned i = 0; i < n; i++)
                hb_ot_layout_lookup_collect_glyphs(hf, HB_OT_TAG_GSUB, i,
                    NULL, s, NULL, NULL);
        }
    }
    if (s) {
        *ps = s;
        *st = 1;
    }
    return s;
}

static void fill_run_meta(struct shaped_run *out, int slot, uint16_t flags,
                          bool oblique, bool embolden, bool emoji,
                          uint8_t variant)
{
    out->slot = slot;
    out->flags = flags;
    out->oblique = oblique;
    out->embolden = embolden;
    out->emoji = emoji;
    out->variant = variant;
}

enum font_shape_result font_shape(int slot, const uint32_t *cps, int len,
                                  uint16_t flags, uint32_t script,
                                  bool require_substitution, struct shaped_run *out)
{
    if (!out || !cps || len < 1 || len > FONT_RUN_MAX)
        return FONT_SHAPE_UNAVAILABLE;
    if (slot < 0 || slot >= num_faces)
        return FONT_SHAPE_UNAVAILABLE;
    ensure_primary_face(slot);
    if (!faces[slot].styles[0])
        return FONT_SHAPE_UNAVAILABLE;
    faces[slot].last_used = ++face_stamp;

    bool italic = (flags & FONT_STYLE_ITALIC) != 0;
    bool emoji = faces[slot].is_emoji;
    bool bold = (flags & FONT_STYLE_BOLD) && !emoji;

    enum font_variant variant = FONT_VARIANT_PLAIN;
    enum font_variant want = emoji ? FONT_VARIANT_PLAIN : font_variant_of(flags);
    if (want)
        variant = pick_face_style(slot, cps, len, want);
    bool oblique = italic && !emoji && !(variant & FONT_VARIANT_ITALIC);
    bool embolden = bold && !(variant & FONT_VARIANT_BOLD);
    FT_Face ft = faces[slot].styles[variant];

    hb_font_t *hb = face_hb_font(slot, variant);
    if (!hb)
        return FONT_SHAPE_UNAVAILABLE;

    hb_set_t *in = layout_input_gids(slot, variant);
    if (in) {
        bool identity = true;
        for (int k = 0; k < len && identity; k++) {
            uint32_t gid = FT_Get_Char_Index(ft, cps[k]);
            out->glyphs[k].gid = gid;
            out->glyphs[k].cluster = (uint16_t)k;
            out->glyphs[k].x_off = 0;
            out->glyphs[k].y_off = 0;
            out->glyphs[k].x_adv = hb_font_get_glyph_h_advance(hb, gid);
            out->glyphs[k].y_adv = 0;
            if (!gid || hb_set_has(in, gid) ||
                !cp_layout_neutral(cps[k]) ||
                hb_ot_layout_get_glyph_class(hb_font_get_face(hb), gid) ==
                HB_OT_LAYOUT_GLYPH_CLASS_MARK)
                identity = false;
        }
        if (identity) {
            fill_run_meta(out, slot, flags, oblique, embolden, emoji,
                          (uint8_t)variant);
            out->count = len;
            return require_substitution ? FONT_SHAPE_UNAVAILABLE
                                        : FONT_SHAPE_NO_SUBST;
        }
    }

    if (!shape_buffer)
        shape_buffer = hb_buffer_create();
    hb_buffer_t *buf = shape_buffer;
    hb_buffer_clear_contents(buf);
    hb_buffer_add_codepoints(buf, cps, len, 0, len);
    if (script) {
        hb_buffer_set_script(buf, (hb_script_t)script);
        hb_buffer_set_direction(buf, hb_script_get_horizontal_direction((hb_script_t)script));
    }
    hb_buffer_guess_segment_properties(buf);
    font_shape_text(hb, buf, cps, len);

    unsigned count = 0;
    const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &count);
    const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &count);

    bool ok = count > 0 && count <= FONT_RUN_MAX;
    for (unsigned k = 0; ok && k < count; k++)
        if (!info[k].codepoint)
            ok = false;

    if (!ok)
        return FONT_SHAPE_UNAVAILABLE;

    bool substituted = count != (unsigned)len;
    for (unsigned k = 0; !substituted && k < count; k++)
        if (info[k].codepoint != FT_Get_Char_Index(ft, cps[k]))
            substituted = true;

    fill_run_meta(out, slot, flags, oblique, embolden, emoji, (uint8_t)variant);
    out->count = (int)count;
    for (unsigned k = 0; k < count; k++) {
        out->glyphs[k].gid = info[k].codepoint;
        out->glyphs[k].cluster = (uint16_t)info[k].cluster;
        out->glyphs[k].x_off = pos[k].x_offset;
        out->glyphs[k].y_off = pos[k].y_offset;
        out->glyphs[k].x_adv = pos[k].x_advance;
        out->glyphs[k].y_adv = pos[k].y_advance;
    }

    if (!substituted)
        return require_substitution ? FONT_SHAPE_UNAVAILABLE : FONT_SHAPE_NO_SUBST;
    return FONT_SHAPE_SHAPED;
}

static bool ensure_color_plane(struct glyph *g)
{
    return g->color || glyph_color_plane(g);
}

static void ink_add_box(struct box *ink, const struct raster_entry **ras,
                       const struct raster_entry *e, struct box b,
                       int *left, int *top, int *right, int *bottom,
                       int *drawn)
{
    if (!*drawn) {
        *left = b.x; *top = b.y;
        *right = b.x + b.w; *bottom = b.y + b.h;
    } else {
        if (b.x < *left) *left = b.x;
        if (b.y < *top) *top = b.y;
        if (b.x + b.w > *right) *right = b.x + b.w;
        if (b.y + b.h > *bottom) *bottom = b.y + b.h;
    }
    ras[*drawn] = e;
    ink[*drawn] = b;
    (*drawn)++;
}

static struct box scale_box(const struct box *b, struct box box,
                             int left, int top, int run_w, int run_h)
{
    struct box dst = {
        box.x + (b->x - left) * box.w / run_w,
        box.y + (b->y - top) * box.h / run_h,
        b->w * box.w / run_w,
        b->h * box.h / run_h,
    };
    if (dst.w < 1) dst.w = 1;
    if (dst.h < 1) dst.h = 1;
    return dst;
}

struct run_ink {
    const struct raster_entry *ras[FONT_RUN_MAX];
    struct box boxes[FONT_RUN_MAX];
    int left, top, right, bottom, drawn;
    int advance;
    bool color;
};

static int clamp_ink_coord(int64_t v)
{
    if (v < -(INT64_C(1) << 24)) return -(1 << 24);
    if (v > (INT64_C(1) << 24)) return 1 << 24;
    return (int)v;
}

static bool collect_run_ink(const struct shaped_run *run, bool by_cluster,
                            struct run_ink *out)
{
    int64_t pen_x = 0, pen_y = 0, advance = 0;
    fit_pass++;
    for (int i = 0; i < run->count; i++) {
        const struct shaped_glyph *sg = &run->glyphs[i];
        const struct raster_entry *e = raster_fetch(run->slot,
            run->variant, sg->gid, run->oblique, run->embolden);
        if (!e || !supported_pixel_mode(&e->glyph->bitmap))
            return false;
        const FT_Bitmap *bm = &e->glyph->bitmap;
        if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
            if (by_cluster)
                return false;
            out->color = true;
        }
        int64_t dx, dy;
        if (by_cluster) {
            dx = (int64_t)sg->cluster * GLYPH_CELL_W + (sg->x_off >> 6);
            dy = -((pen_y + sg->y_off) >> 6);
            pen_y += sg->y_adv;
        } else {
            advance += e->advance_px;
            dx = (pen_x + sg->x_off) >> 6;
            dy = -((pen_y + sg->y_off) >> 6);
            pen_x += sg->x_adv;
            pen_y += sg->y_adv;
        }
        int bw = bitmap_width(bm), bh = bitmap_height(bm);
        out->advance = clamp_ink_coord(advance);
        if (bw <= 0 || bh <= 0)
            continue;
        struct box b = {
            clamp_ink_coord(dx + e->glyph->left),
            clamp_ink_coord(dy - e->glyph->top),
            clamp_ink_coord(bw),
            clamp_ink_coord(bh),
        };
        ink_add_box(out->boxes, out->ras, e, b, &out->left, &out->top,
                   &out->right, &out->bottom, &out->drawn);
    }
    return true;
}

bool font_fit_single(const struct shaped_run *run, struct glyph *g)
{
    if (!run || !g || run->count < 1 || run->count > FONT_RUN_MAX)
        return false;
    if (run->slot < 0 || run->slot >= num_faces)
        return false;

    struct run_ink ri = {0};
    if (!collect_run_ink(run, false, &ri))
        return false;
    if (ri.color && ri.drawn > 1)
        return false;
    if (!ri.drawn)
        return true;

    int run_w = ri.right - ri.left, run_h = ri.bottom - ri.top;
    if (run_w <= 0 || run_h <= 0)
        return false;
    struct box box = ft_place_ink(ri.left, -ri.top, run_w, run_h, ri.advance,
                                  g->width,
                               ri.color ? font_thread_metrics.ink_rows : GLYPH_CELL_H,
                               ri.color);
    if (!ft_prepare_canvas(g, &box))
        return false;
    g->has_color = ri.color;
    if (ri.color && !ensure_color_plane(g))
        return false;

    for (int i = 0; i < ri.drawn; i++) {
        struct box dst = scale_box(&ri.boxes[i], box, ri.left, ri.top,
                                   run_w, run_h);
        ft_blit_bitmap(g, &ri.ras[i]->glyph->bitmap, &dst);
    }
    return true;
}

bool font_fit_cells(const struct shaped_run *run, int n_cells, struct glyph *cells)
{
    if (!run || !cells || n_cells < 1 || n_cells > FONT_RUN_MAX ||
        run->count < 1 || run->count > FONT_RUN_MAX)
        return false;
    if (run->slot < 0 || run->slot >= num_faces)
        return false;

    struct run_ink ri = {0};
    if (!collect_run_ink(run, true, &ri))
        return false;
    if (!ri.drawn)
        return false;

    for (int i = 0; i < n_cells; i++)
        if (!glyph_reset(&cells[i], GLYPH_CELL_W))
            return false;

    int run_w = ri.right - ri.left, run_h = ri.bottom - ri.top;
    if (run_w <= 0 || run_h <= 0)
        return false;
    struct box fitted = ft_place_ink(ri.left, -ri.top, run_w, run_h,
                                     n_cells * GLYPH_CELL_W, n_cells * GLYPH_CELL_W,
                                  GLYPH_CELL_H, false);
    uint8_t covered[FONT_RUN_MAX];
    memset(covered, 0, sizeof(covered[0]) * (size_t)n_cells);
    for (int i = 0; i < ri.drawn; i++) {
        struct box dst = scale_box(&ri.boxes[i], fitted, ri.left, ri.top,
                                   run_w, run_h);
        if (dst.x < 0)
            dst.x = 0;
        int first = dst.x / GLYPH_CELL_W;
        int last = (dst.x + dst.w - 1) / GLYPH_CELL_W;
        if (first < 0) first = 0;
        if (last >= n_cells) last = n_cells - 1;
        for (int cell = first; cell <= last; cell++) {
            struct box slice = dst;
            slice.x -= cell * GLYPH_CELL_W;
            ft_blit_bitmap(&cells[cell], &ri.ras[i]->glyph->bitmap, &slice);
            covered[cell] = 1;
        }
    }
    for (int i = 0; i < n_cells; i++) {
        if (!covered[i])
            continue;
        int rows = glyph_rows(&cells[i]);
        for (int y = 0; y < rows; y++) {
            if (glyph_row_inked(&cells[i], y))
                break;
            if (y == rows - 1)
                return false;
        }
    }
    return true;
}
