#define _GNU_SOURCE
#include "font/backends/freetype/internal.h"

#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <hb-ft.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__thread FT_Library ft_lib = NULL;

__thread struct ft_face_slot faces[MAX_FACES];
__thread int num_faces = 0;
int configured_faces;
__thread uint64_t face_stamp;
__thread int ft_applied_px = FONT_DEFAULT_PIXEL_SIZE;

const char embedded_cards_path[] = "embedded:playing-cards";

/* pick_face_style() tries the variants in this order. */
const enum font_variant font_style_order[3] = {
    FONT_VARIANT_BOLD_ITALIC, FONT_VARIANT_BOLD, FONT_VARIANT_ITALIC
};

static struct cfg_face_spec { char *path; int index; bool is_emoji; bool deferred; }
    cfg_specs[MAX_FACES];
_Atomic unsigned ft_sync_epoch;

const char *face_configured_family(void)
{
    return font_get_family();
}

bool size_face(FT_Face face, int px)
{
    if (FT_Set_Pixel_Sizes(face, 0, px) == 0)
        return true;
    if (face->num_fixed_sizes <= 0)
        return false;
    int best = 0;
    for (int i = 1; i < face->num_fixed_sizes; i++) {
        if (abs(face->available_sizes[i].height - px) <
            abs(face->available_sizes[best].height - px))
            best = i;
    }
    return FT_Select_Size(face, best) == 0;
}

FcPattern *fc_font_match(FcPattern *pat)
{
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    return FcFontMatch(NULL, pat, &result);
}

static FT_Face load_style_face(FT_Face regular, int slant, int weight)
{
    if (!regular || !regular->family_name)
        return NULL;

    FcPattern *pat = FcPatternCreate();
    if (!pat)
        return NULL;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)regular->family_name);
    FcPatternAddInteger(pat, FC_SLANT, slant);
    FcPatternAddInteger(pat, FC_WEIGHT, weight);

    FcPattern *match = fc_font_match(pat);
    FT_Face styled = NULL;
    if (match) {
        FcChar8 *file = NULL;
        int index = 0;
        int mslant = 0, mweight = 0;
        FcPatternGetInteger(match, FC_SLANT, 0, &mslant);
        FcPatternGetInteger(match, FC_WEIGHT, 0, &mweight);
        bool slant_ok = slant == FC_SLANT_ROMAN
            ? mslant == FC_SLANT_ROMAN
            : (mslant == FC_SLANT_ITALIC || mslant == FC_SLANT_OBLIQUE);
        bool weight_ok = weight == FC_WEIGHT_REGULAR ||
                         mweight >= FC_WEIGHT_DEMIBOLD;
        if (slant_ok && weight_ok &&
            FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            FcPatternGetInteger(match, FC_INDEX, 0, &index);
            if (FT_New_Face(ft_lib, (const char *)file, index, &styled) == 0) {
                if ((slant != FC_SLANT_ROMAN && !(styled->style_flags & FT_STYLE_FLAG_ITALIC)) ||
                    (weight != FC_WEIGHT_REGULAR && !(styled->style_flags & FT_STYLE_FLAG_BOLD)) ||
                    !size_face(styled, ft_applied_px)) {
                    FT_Done_Face(styled);
                    styled = NULL;
                }
            }
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return styled;
}

void ensure_primary_face(int i)
{
    if (i < 0 || i >= num_faces || !faces[i].deferred)
        return;
    struct ft_face_slot *f = &faces[i];
    if (!f->path)
        return;
    FT_Face face = NULL;
    if (FT_New_Face(ft_lib, f->path, f->index, &face) != 0)
        return;
    if (!size_face(face, ft_applied_px) || !face->size) {
        FT_Done_Face(face);
        return;
    }
    f->styles[0] = face;
    f->deferred = false;
    f->last_used = ++face_stamp;
}

void ensure_styled_face(int i, enum font_variant variant)
{
    if (i < 0 || i >= num_faces || (unsigned)variant >= FONT_VARIANT_COUNT)
        return;
    ensure_primary_face(i);
    struct ft_face_slot *f = &faces[i];
    uint8_t bit = (uint8_t)(1u << variant);
    if ((f->styles_checked & bit) || f->is_emoji)
        return;
    f->styles_checked |= bit;
    f->styles[variant] = load_style_face(f->styles[FONT_VARIANT_PLAIN],
        variant & FONT_VARIANT_ITALIC ? FC_SLANT_ITALIC : FC_SLANT_ROMAN,
        variant & FONT_VARIANT_BOLD ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
}

static bool style_covers(int i, const uint32_t *cps, int len,
                         enum font_variant variant)
{
    if (i < 0 || i >= num_faces || (unsigned)variant >= FONT_VARIANT_COUNT)
        return false;
    ensure_styled_face(i, variant);
    FT_Face ft = faces[i].styles[variant];
    if (!ft)
        return false;
    for (int k = 0; k < len; k++)
        if (!FT_Get_Char_Index(ft, cps[k]))
            return false;
    return true;
}

enum font_variant pick_face_style(int i, const uint32_t *cps, int len,
                                  enum font_variant want)
{
    for (int a = 0; a < 3; a++)
        if ((font_style_order[a] & want) == font_style_order[a] &&
            style_covers(i, cps, len, font_style_order[a]))
            return font_style_order[a];
    return FONT_VARIANT_PLAIN;
}

hb_font_t *face_hb_font(int i, enum font_variant variant)
{
    if (i < 0 || i >= num_faces || (unsigned)variant >= FONT_VARIANT_COUNT)
        return NULL;
    ensure_primary_face(i);
    if (variant && !faces[i].styles[variant]) {
        ensure_styled_face(i, variant);
        if (!faces[i].styles[variant])
            variant = FONT_VARIANT_PLAIN;
    }
    if (!faces[i].styles[variant])
        return NULL;
    if (!faces[i].hb[variant]) {
        faces[i].hb[variant] =
            hb_ft_font_create_referenced(faces[i].styles[variant]);
        if (faces[i].hb[variant])
            hb_ft_font_set_load_flags(faces[i].hb[variant],
                                      font_ft_load_flags());
    }
    return faces[i].hb[variant];
}

static void pattern_face_attrs(FcPattern *p, int *index, bool *is_emoji)
{
    FcPatternGetInteger(p, FC_INDEX, 0, index);
    if (pattern_is_emoji(p))
        *is_emoji = true;
}

static char *resolve_font_path(const char *name_or_pattern, int *index, bool *is_emoji)
{
    *index = 0;
    *is_emoji = false;
    if (!name_or_pattern || !name_or_pattern[0])
        return NULL;
    if (access(name_or_pattern, R_OK) == 0) {

        int count = 0;
        FcPattern *query = FcFreeTypeQuery((const FcChar8 *)name_or_pattern,
                                           0, NULL, &count);
        if (query) {
            pattern_face_attrs(query, index, is_emoji);
            FcPatternDestroy(query);
        }
        return strdup(name_or_pattern);
    }

    FcPattern *pat = FcNameParse((const FcChar8 *)name_or_pattern);
    if (!pat)
        return NULL;

    FcPattern *font = fc_font_match(pat);
    char *font_path = NULL;
    FcChar8 *file = NULL;

    if (font && FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch && file) {
        pattern_face_attrs(font, index, is_emoji);
        if (access((const char *)file, R_OK) == 0)
            font_path = strdup((const char *)file);
    }
    if (font)
        FcPatternDestroy(font);
    FcPatternDestroy(pat);
    return font_path;
}

static void release_face_hb(int i)
{
    for (int s = 0; s < FONT_VARIANT_COUNT; s++) {
        if (faces[i].hb[s])
            hb_font_destroy(faces[i].hb[s]);
        faces[i].hb[s] = NULL;
    }
}

static void release_face(int i)
{
    unsigned gen = faces[i].slot_reuse + 1;
    release_face_hb(i);
    for (int s = 0; s < FONT_VARIANT_COUNT; s++) {
        if (faces[i].layout_gids[s])
            hb_set_destroy(faces[i].layout_gids[s]);
        if (faces[i].styles[s])
            FT_Done_Face(faces[i].styles[s]);
    }
    free(faces[i].path);
    faces[i] = (struct ft_face_slot){0};
    faces[i].slot_reuse = gen;
}

static int insert_face(FT_Face face, const char *path, int index,
                       bool is_emoji)
{
    if (!size_face(face, ft_applied_px) || !face->size) {
        FT_Done_Face(face);
        return -1;
    }

    int slot = num_faces;
    if (num_faces == MAX_FACES) {
        if (configured_faces >= MAX_FACES) {
            FT_Done_Face(face);
            return -1;
        }
        slot = configured_faces;
        for (int i = configured_faces + 1; i < num_faces; i++)
            if (faces[i].last_used < faces[slot].last_used)
                slot = i;
        release_face(slot);
    } else {
        num_faces++;
    }

    faces[slot] = (struct ft_face_slot){
        .styles = { face },
        .is_emoji = is_emoji,
        .path = strdup(path),
        .index = index,
        .last_used = ++face_stamp,
        .slot_reuse = faces[slot].slot_reuse,
    };
    return slot;
}

int ft_load_face(const char *path, int index, bool is_emoji)
{
    if (!path)
        return -1;

    FT_Face face = NULL;
    if (FT_New_Face(ft_lib, path, index, &face))
        return -1;

    return insert_face(face, path, index, is_emoji);
}

int load_embedded_face(const unsigned char *data, size_t len, bool is_emoji)
{
    FT_Face face = NULL;
    if (FT_New_Memory_Face(ft_lib, data, (FT_Long)len, 0, &face))
        return -1;

    return insert_face(face, embedded_cards_path, 0, is_emoji);
}

static int defer_face(const char *path, int index, bool is_emoji)
{
    if (!path)
        return -1;
    if (num_faces == MAX_FACES)
        return ft_load_face(path, index, is_emoji);
    int slot = num_faces++;
    faces[slot] = (struct ft_face_slot){
        .is_emoji = is_emoji,
        .path = strdup(path),
        .index = index,
        .last_used = ++face_stamp,
        .slot_reuse = faces[slot].slot_reuse,
        .deferred = true,
    };
    return slot;
}

static void load_named_face_ex(const char *name, bool defer)
{
    int index;
    bool is_emoji;
    char *path = resolve_font_path(name, &index, &is_emoji);
    if (!path) return;
    bool duplicate = false;
    for (int i = 0; i < num_faces; i++)
        if (faces[i].path && faces[i].index == index && !strcmp(path, faces[i].path))
            duplicate = true;
    if (!duplicate) {
        if (defer)
            defer_face(path, index, is_emoji);
        else
            ft_load_face(path, index, is_emoji);
    }
    free(path);
}

void load_named_face(const char *name)
{
    load_named_face_ex(name, false);
}

void defer_named_face(const char *name)
{
    load_named_face_ex(name, true);
}

void release_all_faces(void)
{
    for (int i = 0; i < num_faces; i++)
        release_face(i);
    num_faces = 0;
}

void face_cfg_specs_destroy(void)
{
    for (int i = 0; i < MAX_FACES; i++) {
        free(cfg_specs[i].path);
        cfg_specs[i].path = NULL;
    }
}

void face_snapshot_configured(void)
{
    face_cfg_specs_destroy();
    configured_faces = num_faces;
    for (int i = 0; i < configured_faces; i++)
        cfg_specs[i] = (struct cfg_face_spec){ strdup(faces[i].path),
                                              faces[i].index,
                                              faces[i].is_emoji,
                                              faces[i].deferred };
}

void face_reload_configured(void)
{
    for (int i = 0; i < configured_faces; i++) {
        if (cfg_specs[i].deferred)
            defer_face(cfg_specs[i].path, cfg_specs[i].index,
                       cfg_specs[i].is_emoji);
        else
            ft_load_face(cfg_specs[i].path, cfg_specs[i].index,
                         cfg_specs[i].is_emoji);
    }
}

/* The cell of face at the applied size. Without a sized face it gives the
 * bound metrics. */
struct font_metrics ft_measure_cell(FT_Face face)
{
    if (!face || !face->size)
        return font_thread_metrics;
    FT_Size_Metrics m = face->size->metrics;
    int advance = (int)(m.max_advance >> 6);
    int load_flags = font_ft_load_flags();
    if (!FT_Load_Char(face, 'M', load_flags))
        advance = ft_px_round(face->glyph->advance.x);

    int line = ft_px_round(FT_MulFix(face->height, m.y_scale));
    int ascent = ft_px_round(FT_MulFix(face->ascender, m.y_scale));
    int descent = ft_px_round(FT_MulFix(face->descender, m.y_scale));
    if (descent > 0)
        descent = -descent;

    struct font_metrics t = { .pixel_size = ft_applied_px };
    t.cell_w = advance < 1 ? 1 : advance;
    t.cell_h = line < 1 ? 1 : line;

    t.baseline = ascent + (t.cell_h - (ascent - descent)) / 2;
    if (t.baseline < 0) t.baseline = 0;
    if (t.baseline > t.cell_h) t.baseline = t.cell_h;

    if (!FT_Load_Char(face, '(', load_flags | FT_LOAD_RENDER) && face->glyph->bitmap.rows)
        t.ink_rows = (int)face->glyph->bitmap.rows;
    else
        t.ink_rows = t.cell_h;
    return t;
}

/* Raster and vector caches key on the pixel size, so they keep what
 * other sizes drew. Hb fonts cache the scale, so they go. A size that is
 * already applied changes nothing. */
void ft_size_faces(int px)
{
    if (px == ft_applied_px)
        return;
    ft_applied_px = px;
    for (int i = 0; i < num_faces; i++) {
        for (int s = 0; s < FONT_VARIANT_COUNT; s++)
            if (faces[i].styles[s])
                size_face(faces[i].styles[s], px);
        release_face_hb(i);
    }
}

void face_activate(const struct font_metrics *m)
{
    if (m) {
        ft_size_faces(m->pixel_size);
        font_thread_metrics_set(m);
    }
}
