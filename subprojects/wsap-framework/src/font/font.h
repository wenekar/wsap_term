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

/* Returns true when code point `cp` with `style` prefers emoji presentation. */
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

/* Converts font size `size` to pixels. */
int font_size_px(struct font_size size);

enum font_slant {
    FONT_SLANT_NORMAL = 0,
    FONT_SLANT_ITALIC = 1,
    FONT_SLANT_OBLIQUE = 2,
};

#define FONT_WEIGHT_MIN 100
#define FONT_WEIGHT_REGULAR 400
#define FONT_WEIGHT_MAX 900
/* A weight at or above this value selects a bold face. */
#define FONT_WEIGHT_BOLD_MIN 600

/*
 * The request for a font instance. A zero field takes its default: the
 * configured family list and point size, scale 120, FONT_WEIGHT_REGULAR,
 * FONT_SLANT_NORMAL, or a monospace face. The backend can synthesize a face
 * that the family does not have.
 */
struct font_desc {
    /* Comma-separated family names. NULL uses the configured family list. */
    const char *family_list;
    /* Point size and display scale. */
    struct font_size size;
    /* CSS weight from FONT_WEIGHT_MIN to FONT_WEIGHT_MAX. */
    uint16_t weight;
    /* Base slant of the face. One value of enum font_slant. */
    uint8_t slant;
    /* The request wants the natural advance of each glyph, not one advance
     * for every glyph. A zero value asks for a monospace face. */
    bool proportional;
};

/* Returns the weight of `desc`, which is FONT_WEIGHT_REGULAR for zero and
 * clamped to the FONT_WEIGHT_MIN..FONT_WEIGHT_MAX range. */
static inline uint16_t font_desc_weight(const struct font_desc *desc)
{
    if (!desc || !desc->weight)
        return FONT_WEIGHT_REGULAR;
    return desc->weight < FONT_WEIGHT_MIN ? FONT_WEIGHT_MIN
         : desc->weight > FONT_WEIGHT_MAX ? FONT_WEIGHT_MAX : desc->weight;
}

/* Returns the style bits that the base weight and slant of `desc` select. */
static inline uint16_t font_desc_base_style(const struct font_desc *desc)
{
    uint16_t style = 0;
    if (!desc)
        return 0;
    if (font_desc_weight(desc) >= FONT_WEIGHT_BOLD_MIN)
        style |= FONT_STYLE_BOLD;
    if (desc->slant != FONT_SLANT_NORMAL)
        style |= FONT_STYLE_ITALIC;
    return style;
}

struct font_metrics {
    int cell_w, cell_h;
    int baseline;
    int pixel_size;
    int ink_rows;
};

struct font_instance;

/* Acquires a reference to the font instance that matches font descriptor
 * `desc`. Returns NULL for a NULL `desc` or when no face can be created. The
 * caller releases the reference with font_instance_unref(). */
struct font_instance *font_instance_acquire(const struct font_desc *desc);
/* Increments the reference count of `fi` and returns `fi`. */
struct font_instance *font_instance_ref(struct font_instance *fi);
/* Decrements the reference count of font instance `fi` and frees it on zero. */
void font_instance_unref(struct font_instance *fi);
/* Returns a borrowed pointer to the default font instance. The pointer is NULL
 * before font_system_init() succeeds and after font_system_shutdown(). */
struct font_instance *font_instance_default(void);
/* Returns the metrics of font instance `fi`. The pointer is valid for the
 * lifetime of the instance, and a face reopen updates the values in place. */
static inline const struct font_metrics *font_instance_metrics(
    const struct font_instance *fi)
{
    return (const struct font_metrics *)fi;
}
int font_instance_size_pt(const struct font_instance *fi);
/* Returns the display scale of `fi` in 1/120 units, where 120 is scale 1. */
uint32_t font_instance_scale_120(const struct font_instance *fi);
/* Returns a version value that changes when the instance identity or the font
 * generation changes. */
uint64_t font_instance_version(const struct font_instance *fi);
/* Sets the rasterization size and cell metrics of `fi` for the calling
 * thread. */
void font_bind_thread(const struct font_instance *fi);
uint16_t font_instance_weight(const struct font_instance *fi);
/* Returns true when font instance `fi` uses one advance for every glyph. */
bool font_instance_monospace(const struct font_instance *fi);

#define FONT_RUN_MAX 32

/*
 * One rasterized glyph. The raster holds `width` by `height` pixels, `advance`
 * is the pen step after the glyph, and `x_offset` is the cell column of raster
 * column 0.
 *
 * A cell glyph covers its cell: row 0 of the raster is the top of the cell, so
 * the raster already holds each ink row at its own row of the cell. A painter
 * places the raster at the cell top and at the cell left plus `x_offset`. It
 * must not add `y_offset`, which would move the ink down twice.
 */
struct glyph {
    uint64_t *row_masks;
    uint8_t **alpha;
    uint8_t **subpx;
    uint32_t *color;
    void *raster;
    uint32_t raster_capacity;
    uint32_t color_capacity;
    uint32_t subpx_capacity;
    uint16_t width;
    uint16_t height;
    /* One mask word holds 64 columns, so 11 bits cover the largest width.
     * The two flags share the rest of the unit, to keep the struct at 64
     * bytes. */
    uint16_t mask_words : 11;
    uint16_t has_color : 1;
    uint16_t has_subpx_ink : 1;
    int16_t x_offset;
    int16_t y_offset;
    int16_t advance;
};

_Static_assert(sizeof(struct glyph) == 64, "struct glyph fits in one cacheline");

/* Returns the ink mask words of row `y` of glyph `g`. */
static inline uint64_t *glyph_mask(const struct glyph *g, int y)
{
    return g->row_masks + (size_t)y * g->mask_words;
}

/* Sets the ink mask bit of column `x` in row `y` of glyph `g`. */
static inline void glyph_mark(struct glyph *g, int y, int x)
{
    glyph_mask(g, y)[x >> 6] |= UINT64_C(1) << (x & 63);
}

/* Returns true when row `y` of glyph `g` has any inked pixels. */
static inline bool glyph_row_inked(const struct glyph *g, int y)
{
    const uint64_t *m = glyph_mask(g, y);
    for (int i = 0; i < g->mask_words; i++)
        if (m[i])
            return true;
    return false;
}

/* Returns true when the pixel at column `x` of row `y` in glyph `g` is inked. */
static inline bool glyph_inked(const struct glyph *g, int y, int x)
{
    return (glyph_mask(g, y)[x >> 6] >> (x & 63)) & 1;
}

/* Returns a pointer to the ARGB pixel at column `x` of row `y` in glyph `g`. */
static inline uint32_t *glyph_color_at(const struct glyph *g, int y, int x)
{
    return g->color + (size_t)y * g->width + x;
}

/* Returns a pointer to the three subpixel alpha bytes at column `x` of row `y`
 * in glyph `g`. */
static inline const uint8_t *glyph_subpx_at(const struct glyph *g, int y, int x)
{
    return g->subpx[y] + (size_t)x * 3;
}

/* Allocates and clears the ARGB color plane of glyph `g` at its current size.
 * Returns false when the allocation fails. */
bool glyph_color_plane(struct glyph *g);

/* Allocates and clears the subpixel alpha plane of glyph `g` and marks the
 * glyph as having subpixel ink. Returns false when `g` has zero width or the
 * allocation fails. */
bool glyph_subpx_plane(struct glyph *g);

/* Frees the raster, color, and subpixel buffers of glyph `g` and zeroes it. */
void glyph_release(struct glyph *g);
/* Prepares the raster of glyph `g` for `width` columns and `rows` rows and
 * clears it. Returns false when `g` is NULL, an extent is zero or too large, or
 * allocation fails. */
bool glyph_prepare(struct glyph *g, unsigned width, unsigned rows);

enum font_shape_result {
    FONT_SHAPE_UNAVAILABLE,
    FONT_SHAPE_NO_SUBST,
    FONT_SHAPE_SHAPED,
};

/* Renders glyph `g` for code point `cp` with `style` from font instance `fi`.
 * The caller owns `g` and releases it with glyph_release(); a later render into
 * the same glyph reuses its buffers. Returns false when `fi` or `g` is NULL or
 * the buffers cannot be allocated. */
bool font_instance_render_glyph(const struct font_instance *fi, uint32_t cp,
                                uint16_t style, struct glyph *g);
/*
 * Renders glyph `g` for code point `cp` with `style` at `phase` quarter pixels
 * to the right of the pen origin. `phase` is a value from 0 to 3. A nonzero
 * phase places the outline on a subpixel grid. The glyph cache keys the phase
 * with the code point and the style, so a placed glyph does not share the
 * entry of the phase-zero glyph.
 */
bool font_instance_render_glyph_at(const struct font_instance *fi, uint32_t cp,
                                   uint16_t style, uint8_t phase, struct glyph *g);
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

/* Sets the outline hinting style. FONT_HINT_AUTO takes the system value, and a
 * change in rendering increments the font generation. */
void font_set_hint_style(enum font_hint_style style);
/* Sets the antialiasing mode. FONT_AA_AUTO takes the system value, and a change
 * in rendering increments the font generation. */
void font_set_aa_mode(enum font_aa_mode aa);
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
/* Enables or disables subpixel font smoothing. A change increments the font
 * generation. */
void font_set_smoothing(bool enabled);
#endif
/* Parses hinting style name `str` into `out`. Returns false for an unknown name
 * and leaves `out` unchanged. */
bool font_parse_hint_style(const char *str, enum font_hint_style *out);
/* Parses antialiasing mode name `str` into `out`. Returns false for an unknown
 * name and leaves `out` unchanged. */
bool font_parse_aa_mode(const char *str, enum font_aa_mode *out);
/* Returns the name of hinting style `style` for the current backend, or "auto"
 * when the backend offers no name for the value. */
const char *font_hint_style_name(enum font_hint_style style);
/* Returns the name of antialiasing mode `aa` for the current backend, or
 * "auto" when the backend offers no name for the value. */
const char *font_aa_mode_name(enum font_aa_mode aa);
/* Sets the configured family name list. A NULL or empty `name` restores
 * FONT_DEFAULT_FAMILY. The font generation changes, so instances reopen their
 * faces at the next rebuild. */
void font_set_family(const char *name);
/* Returns the configured family name list, which is never NULL. */
const char *font_get_family(void);
/* Returns the installed font family names and writes their count to `count`.
 * Each family appears once. The caller frees every name and the array. Returns
 * NULL and writes 0 to `count` when the query fails. */
char **font_list_families(size_t *count);
/* Sets the default point size for instances that are acquired later. The clamp
 * to FONT_MIN_SIZE_PT..FONT_MAX_SIZE_PT happens at acquisition. */
void font_set_configured_size(int size_pt);
int font_configured_size(void);

/* Resolves unset dimensions in `size` to configured defaults. */
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
/* Applies the differences between `prev` and `next` font settings. A NULL
 * `prev` applies every setting. */
void font_apply_settings(const struct font_settings *prev,
                         const struct font_settings *next);

/* Initializes the platform font rendering backend. Returns false when it cannot
 * initialize. */
bool font_backend_init(void);
void font_backend_shutdown(void);

/* Attaches the calling thread to the font backend. Returns false when the
 * backend cannot prepare the thread. */
bool font_thread_attach(void);
void font_thread_detach(void);

/* Returns the font configuration change counter. */
uint64_t font_generation(void);
void font_bump_generation(void);

#endif
