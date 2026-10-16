#ifndef FONT_FREETYPE_INTERNAL_H
#define FONT_FREETYPE_INTERNAL_H

#include "font/internal.h"

extern __thread int ft_applied_px;

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <hb.h>
#include <fontconfig/fontconfig.h>

#ifndef MAX_FACES
#define MAX_FACES 256
#endif
#define UNICODE_LIMIT 0x110000u

/* enum font_variant indexes each FONT_VARIANT_COUNT array. */
struct ft_face_slot {
    FT_Face styles[FONT_VARIANT_COUNT];
    hb_font_t *hb[FONT_VARIANT_COUNT];
    char *path;
    uint64_t last_used;
    hb_set_t *layout_gids[FONT_VARIANT_COUNT];
    int index;
    unsigned slot_reuse;
    uint8_t layout_gids_state[FONT_VARIANT_COUNT];
    uint8_t styles_checked;
    bool is_emoji;
    bool deferred;
};

_Static_assert(sizeof(struct ft_face_slot) == 128,
               "ft_face_slot should occupy exactly two 64-byte cache lines");

extern __thread FT_Library ft_lib;

extern __thread struct ft_face_slot faces[MAX_FACES];
extern __thread int num_faces;
extern int configured_faces;
extern __thread uint64_t face_stamp;
extern _Atomic unsigned ft_sync_epoch;

extern const char embedded_cards_path[];

void ensure_primary_face(int i);
void ensure_styled_face(int i, enum font_variant variant);
hb_font_t *face_hb_font(int i, enum font_variant variant);
int ft_load_face(const char *path, int index, bool is_emoji);
int load_embedded_face(const unsigned char *data, size_t len, bool is_emoji);
void load_named_face(const char *name);
void defer_named_face(const char *name);
bool size_face(FT_Face face, int px);
void ft_size_faces(int px);
struct font_metrics ft_measure_cell(FT_Face face);
const char *face_configured_family(void);
void face_snapshot_configured(void);
void face_reload_configured(void);
void ft_reload_family(void);

extern bool fc_ready;

int font_ft_load_flags(void);
FT_Render_Mode font_ft_render_mode(void);
uint8_t raster_hint_key(void);
void ft_apply_hinting(void);
void font_hinting_init_library(FT_Library lib);
bool font_hinting_has_auto_axis(void);

bool supported_pixel_mode(const FT_Bitmap *bitmap);
struct box ft_place_ink(int left, int above_baseline, int w, int h,
                        int advance, int cols, int max_rows, bool color);
bool ft_prepare_canvas(struct glyph *g, struct box *ink);
void ft_blit_bitmap(struct glyph *g, const FT_Bitmap *bm, const struct box *dst);

static inline int bitmap_width(const FT_Bitmap *bm)
{
    return bm->pixel_mode == FT_PIXEL_MODE_LCD ? (int)bm->width / 3 : (int)bm->width;
}

static inline int bitmap_height(const FT_Bitmap *bm)
{
    return bm->pixel_mode == FT_PIXEL_MODE_LCD_V ? (int)bm->rows / 3 : (int)bm->rows;
}

extern const enum font_variant font_style_order[3];
enum font_variant pick_face_style(int i, const uint32_t *cps, int len,
                                  enum font_variant want);
static inline bool pattern_is_emoji(FcPattern *p)
{
    FcBool color = FcFalse;
    FcChar8 *family = NULL;
    FcPatternGetBool(p, FC_COLOR, 0, &color);
    if (color != FcFalse)
        return true;
    return FcPatternGetString(p, FC_FAMILY, 0, &family) == FcResultMatch &&
           family && strstr((const char *)family, "Emoji");
}
static inline int ft_px_round(FT_Pos v)
{
    return (int)((v + 32) >> 6);
}
FcPattern *fc_font_match(FcPattern *pat);
void release_all_faces(void);
void face_cfg_specs_destroy(void);

struct font_face_query {
    uint64_t cfg_mask;
    uint32_t cp;
    int phase;
    int idx;
    unsigned cover_seen;
    bool prefer_emoji : 1;
    bool vfail : 1;
};
#define FONT_FACE_QUERY(codepoint, emoji) \
    ((struct font_face_query){ .cfg_mask = UINT64_MAX, .cp = (codepoint), \
                               .prefer_emoji = (emoji) })

hb_unicode_funcs_t *hb_ufuncs(void);
bool fallback_coverage_exempt(uint32_t cp);
bool fallback_ready(void);
bool fallback_init(void);
void fallback_destroy(void);
void fallback_thread_reset(void);
void fallback_coverage_reset(void);
void build_cfg_usable(void);
bool fallback_run_covered(const uint32_t *cps, int len, unsigned *gen);
bool run_coverable(const uint32_t *cps, int len);
uint64_t ft_scan_run(const uint32_t *cps, int len, uint16_t style,
                     uint64_t *hash, int *dead_at);
int font_next_face_covering(struct font_face_query *q, const uint32_t *cps,
                            int len);
int font_next_face_for_run(struct font_face_query *q, const uint32_t *cps,
                           int len, uint16_t flags);

static inline bool needs_coverage(int k, uint32_t cp)
{
    return k == 0 || !fallback_coverage_exempt(cp);
}

struct shaped_glyph {
    uint32_t gid;
    int32_t x_off, y_off;
    int32_t x_adv, y_adv;
    uint16_t cluster;
};
_Static_assert(FONT_RUN_MAX <= UINT16_MAX, "cluster index truncation");

struct shaped_run {
    int slot;
    int count;
    uint16_t flags;
    uint8_t variant;
    bool oblique : 1;
    bool embolden : 1;
    bool emoji : 1;
    uint32_t _pad;
    alignas(16) struct shaped_glyph glyphs[FONT_RUN_MAX];
};

/* script is an hb_script_t, or 0 to guess it from the text. */
enum font_shape_result font_shape(int slot, const uint32_t *cps, int len,
                                  uint16_t flags, uint32_t script,
                                  bool require_substitution, struct shaped_run *out);

bool font_fit_single(const struct shaped_run *run, struct glyph *g);
bool font_fit_cells(const struct shaped_run *run, int n_cells, struct glyph *cells);

void raster_cache_clear(void);
bool shape_buffer_live(void);
void shape_thread_reset(void);

bool colr_rasterize(FT_Face face, uint32_t gid, FT_BitmapGlyph bg);

#endif
