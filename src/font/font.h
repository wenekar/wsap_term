#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum font_style {
    FONT_STYLE_BOLD = 1 << 0,
    FONT_STYLE_ITALIC = 1 << 1,
    FONT_STYLE_EMOJI = 1 << 6,
    FONT_STYLE_TEXT = 1 << 7,
    FONT_STYLE_WIDE = 1 << 8,
};

#define FONT_STYLE_VARIANT_MASK (FONT_STYLE_BOLD | FONT_STYLE_ITALIC)

enum font_variant {
    FONT_VARIANT_PLAIN = 0,
    FONT_VARIANT_BOLD = 1,
    FONT_VARIANT_ITALIC = 2,
    FONT_VARIANT_BOLD_ITALIC = 3,
};
#define FONT_VARIANT_COUNT 4

_Static_assert((unsigned)FONT_VARIANT_BOLD == (unsigned)FONT_STYLE_BOLD &&
               (unsigned)FONT_VARIANT_ITALIC == (unsigned)FONT_STYLE_ITALIC &&
               (unsigned)FONT_VARIANT_BOLD_ITALIC == (unsigned)FONT_STYLE_VARIANT_MASK &&
               FONT_VARIANT_COUNT == FONT_STYLE_VARIANT_MASK + 1,
               "a font variant is the variant bits of a style");

static inline enum font_variant font_variant_of(uint16_t style)
{
    return (enum font_variant)(style & FONT_STYLE_VARIANT_MASK);
}

bool font_prefers_emoji(uint32_t cp, uint16_t style);

#define FONT_DEFAULT_SIZE_PT 10
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
#define FONT_DEFAULT_FAMILY "FiraCode Nerd Font Mono,SF Mono,Menlo,Monaco"
#else
#define FONT_DEFAULT_FAMILY "JetBrainsMono Nerd Font,JetBrainsMono,JetBrains Mono,FiraCode Nerd Font Mono"
#endif
#define FONT_MIN_SIZE_PT 6
#define FONT_MAX_SIZE_PT 1000
#define FONT_PT_TO_PX(pt) ((pt) * 96 / 72)
#define FONT_DEFAULT_PIXEL_SIZE FONT_PT_TO_PX(FONT_DEFAULT_SIZE_PT)

struct font_size {
    int pt;
    uint32_t scale_120;
};

int font_size_px(struct font_size size);

struct font_metrics {
    int cell_w, cell_h;
    int baseline;
    int pixel_size;
    int ink_rows;
};

struct font_instance;

struct font_instance *font_instance_acquire(int size_pt, uint32_t scale_120);
struct font_instance *font_instance_ref(struct font_instance *fi);
void font_instance_unref(struct font_instance *fi);
struct font_instance *font_instance_default(void);
static inline const struct font_metrics *font_instance_metrics(
    const struct font_instance *fi)
{
    return (const struct font_metrics *)fi;
}
int font_instance_size_pt(const struct font_instance *fi);
uint32_t font_instance_scale_120(const struct font_instance *fi);
uint64_t font_instance_version(const struct font_instance *fi);
void font_bind_thread(const struct font_instance *fi);

#define FONT_RUN_MAX 32

struct glyph {
    uint64_t *row_masks;
    uint8_t **alpha;
    uint8_t **subpx;
    uint32_t *color;
    void *raster;
    void *subpx_mem;
    size_t raster_capacity;
    size_t color_capacity;
    size_t subpx_capacity;
    uint16_t width;
    uint16_t height;
    uint16_t mask_words;
    int16_t x_offset;
    bool has_color;
    bool has_subpx_ink;
};

static inline uint64_t *glyph_mask(const struct glyph *g, int y)
{
    return g->row_masks + (size_t)y * g->mask_words;
}

static inline void glyph_mark(struct glyph *g, int y, int x)
{
    glyph_mask(g, y)[x >> 6] |= UINT64_C(1) << (x & 63);
}

static inline bool glyph_row_inked(const struct glyph *g, int y)
{
    const uint64_t *m = glyph_mask(g, y);
    for (int i = 0; i < g->mask_words; i++)
        if (m[i])
            return true;
    return false;
}

static inline bool glyph_inked(const struct glyph *g, int y, int x)
{
    return (glyph_mask(g, y)[x >> 6] >> (x & 63)) & 1;
}

static inline uint32_t *glyph_color_at(const struct glyph *g, int y, int x)
{
    return g->color + (size_t)y * g->width + x;
}

static inline const uint8_t *glyph_subpx_at(const struct glyph *g, int y, int x)
{
    return g->subpx[y] + (size_t)x * 3;
}

bool glyph_color_plane(struct glyph *g);

bool glyph_subpx_plane(struct glyph *g);

void glyph_release(struct glyph *g);
bool glyph_prepare(struct glyph *g, unsigned width);

enum font_shape_result {
    FONT_SHAPE_UNAVAILABLE,
    FONT_SHAPE_NO_SUBST,
    FONT_SHAPE_SHAPED,
};

bool font_instance_render_glyph(const struct font_instance *fi, uint32_t cp,
                                uint16_t style, struct glyph *g);
enum font_run_result {
    FONT_RUN_UNAVAILABLE,
    FONT_RUN_UNCHANGED,
    FONT_RUN_SHAPED,
};

enum font_hint_style {
    FONT_HINT_AUTO = 0,
    FONT_HINT_NONE,
    FONT_HINT_SLIGHT,
    FONT_HINT_MEDIUM,
    FONT_HINT_FULL,
};

enum font_aa_mode {
    FONT_AA_AUTO = 0,
    FONT_AA_GRAYSCALE,
    FONT_AA_RGB,
    FONT_AA_BGR,
    FONT_AA_VRGB,
    FONT_AA_VBGR,
    FONT_AA_MONO,
};

void font_set_hint_style(enum font_hint_style style);
void font_set_aa_mode(enum font_aa_mode aa);
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
void font_set_smoothing(bool enabled);
#endif
bool font_parse_hint_style(const char *str, enum font_hint_style *out);
bool font_parse_aa_mode(const char *str, enum font_aa_mode *out);
const char *font_hint_style_name(enum font_hint_style style);
const char *font_aa_mode_name(enum font_aa_mode aa);
void font_set_family(const char *name);
const char *font_get_family(void);
char **font_list_families(size_t *count);
void font_set_configured_size(int size_pt);
int font_configured_size(void);

static inline struct font_size font_size_resolve(struct font_size size)
{
    return (struct font_size){
        .pt = size.pt > 0 ? size.pt : font_configured_size(),
        .scale_120 = size.scale_120 ? size.scale_120 : 120,
    };
}

struct font_settings {
    const char *family;
    int size_pt;
    enum font_hint_style hinting;
    enum font_aa_mode antialias;
    bool smoothing;
};
void font_apply_settings(const struct font_settings *prev,
                         const struct font_settings *next);

bool font_backend_init(void);
void font_backend_shutdown(void);

bool font_thread_attach(void);
void font_thread_detach(void);

uint64_t font_generation(void);
void font_bump_generation(void);

#endif
