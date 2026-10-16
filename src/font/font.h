#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Style bits for a glyph lookup. render_cell_style() converts terminal
 * attributes to these bits. */
enum font_style {
    FONT_STYLE_BOLD = 1 << 0,
    FONT_STYLE_ITALIC = 1 << 1,
    FONT_STYLE_EMOJI = 1 << 6,      /* the text asked for emoji presentation */
    FONT_STYLE_TEXT = 1 << 7,       /* the text asked for text presentation */
    FONT_STYLE_WIDE = 1 << 8,       /* the glyph covers two cells */
};

/* The style bits that pick a face variant. */
#define FONT_STYLE_VARIANT_MASK (FONT_STYLE_BOLD | FONT_STYLE_ITALIC)

/* One of the four faces of a family. The value is the
 * FONT_STYLE_VARIANT_MASK bits of a style. Arrays of faces use it as the
 * index. */
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

/* Whether cp draws as an emoji under style: an explicit presentation
 * request wins over the Unicode default. */
bool font_prefers_emoji(uint32_t cp, uint16_t style);

#define FONT_DEFAULT_SIZE_PT 10
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
#define FONT_DEFAULT_FAMILY "FiraCode Nerd Font Mono,SF Mono,Menlo,Monaco"
#else
#define FONT_DEFAULT_FAMILY "JetBrainsMono Nerd Font,JetBrainsMono,JetBrains Mono,FiraCode Nerd Font Mono"
#endif
#define FONT_MIN_SIZE_PT 6
/* Keeps a glyph row inside the 16-bit width of struct glyph at any
 * display scale; not a rendering limit. */
#define FONT_MAX_SIZE_PT 1000
#define FONT_PT_TO_PX(pt) ((pt) * 96 / 72)
#define FONT_DEFAULT_PIXEL_SIZE FONT_PT_TO_PX(FONT_DEFAULT_SIZE_PT)

/* The size a face opens at. A zero pt is the configured default size,
 * and a zero scale_120 is scale 1. */
struct font_size {
    /* The nominal size in points. */
    int pt;
    /* The display scale in 1/120 units. */
    uint32_t scale_120;
};

/* The em size of size in device pixels: FONT_PT_TO_PX(pt) multiplied by
 * scale_120 / 120 and rounded. The result is clamped to 1..UINT16_MAX / 4.
 * Every backend fills its cells at this size, whatever size it gives to
 * the platform font API. */
int font_size_px(struct font_size size);

/* The cell of one font size at one scale, in device pixels. */
struct font_metrics {
    int cell_w, cell_h;
    int baseline;
    int pixel_size;
    /* The height of '(' in pixels: what text actually covers. */
    int ink_rows;
};

/* The configured family at one size and one scale: its metrics and a
 * glyph cache of its own. Instances are interned, so a size and a scale
 * give the same instance while anyone holds it. */
struct font_instance;

/* Returns one reference. size_pt is clamped to FONT_MIN_SIZE_PT..
 * FONT_MAX_SIZE_PT, and scale_120 0 is scale 1. NULL when the backend
 * cannot open or measure the face, or on allocation failure. The result
 * of a call before font_system_init() depends on the backend.
 * May rebuild a stale instance. Glyph pointers from earlier fetches become
 * invalid. Call between frames. */
struct font_instance *font_instance_acquire(int size_pt, uint32_t scale_120);
struct font_instance *font_instance_ref(struct font_instance *fi);
void font_instance_unref(struct font_instance *fi);
/* The configured size at scale 1, alive from font_system_init() to
 * font_system_shutdown(). Borrowed. NULL before font_system_init(), or
 * when font_system_init() could not make it.
 * May rebuild a stale instance. Glyph pointers from earlier fetches become
 * invalid. Call between frames. */
struct font_instance *font_instance_default(void);
/* The metrics are the instance's first member, so this costs no call. */
static inline const struct font_metrics *font_instance_metrics(
    const struct font_instance *fi)
{
    return (const struct font_metrics *)fi;
}
int font_instance_size_pt(const struct font_instance *fi);
uint32_t font_instance_scale_120(const struct font_instance *fi);
/* Changes whenever glyphs of fi may look different: a new instance, or
 * a font_generation() bump. A hinting change, a family change,
 * font_backend_shutdown(), and a CoreText font registration each bump it. */
uint64_t font_instance_version(const struct font_instance *fi);
/* Makes this thread rasterize at fi's size. font_instance_render_glyph()
 * and the glyph cache call it. */
void font_bind_thread(const struct font_instance *fi);

#define FONT_RUN_MAX 32

/* One cell (or a wide pair) of ink. Row y has mask_words 64-bit words of
 * ink bits; bit x of the row is set where alpha, or color when
 * has_color, is nonzero. color, when allocated, holds cell_h rows of
 * width pixels. */
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
    /* The rows of the planes. 0 means the cell height of the instance
     * bound to this thread. */
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

/* Allocates the color plane for the current width, zeroed. */
bool glyph_color_plane(struct glyph *g);

/* Allocates the subpixel RGB plane for the current width, zeroed. */
bool glyph_subpx_plane(struct glyph *g);

void glyph_release(struct glyph *g);
bool glyph_prepare(struct glyph *g, unsigned width);

enum font_shape_result {
    FONT_SHAPE_UNAVAILABLE,
    /* Shaping made no substitution. out is filled. */
    FONT_SHAPE_NO_SUBST,
    FONT_SHAPE_SHAPED,
};

/* Renders cp at fi's cell size. It binds fi to this thread first.
 * Returns false when fi or g is NULL. */
bool font_instance_render_glyph(const struct font_instance *fi, uint32_t cp,
                                uint16_t style, struct glyph *g);
enum font_run_result {
    FONT_RUN_UNAVAILABLE,
    /* The cells are untouched. Nothing is filled. */
    FONT_RUN_UNCHANGED,
    FONT_RUN_SHAPED,
};

/* Auto takes the value from the system. */
enum font_hint_style {
    FONT_HINT_AUTO = 0,
    FONT_HINT_NONE,
    FONT_HINT_SLIGHT,
    FONT_HINT_MEDIUM,
    FONT_HINT_FULL,
};

/* Auto takes the value from the system. The settings choices follow
 * this order. */
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
/* Case-insensitive. Each backend accepts only the names it supports. */
bool font_parse_hint_style(const char *str, enum font_hint_style *out);
bool font_parse_aa_mode(const char *str, enum font_aa_mode *out);
/* The canonical name. "auto" for a value the backend does not offer. */
const char *font_hint_style_name(enum font_hint_style style);
const char *font_aa_mode_name(enum font_aa_mode aa);
void font_set_family(const char *name);
const char *font_get_family(void);
/* Lists installed monospace families. The caller frees each string and
 * the array. NULL means no families or an allocation failure. */
char **font_list_families(size_t *count);
/* The size that a zero pt resolves to. font_system_init() makes
 * font_instance_default() at this size. A later change affects only the
 * instances created after it, not font_instance_default(). */
void font_set_configured_size(int size_pt);
int font_configured_size(void);

/* Replaces a zero pt with font_configured_size() and a zero scale_120
 * with 120. font_size_px() and every backend use this rule. */
static inline struct font_size font_size_resolve(struct font_size size)
{
    return (struct font_size){
        .pt = size.pt > 0 ? size.pt : font_configured_size(),
        .scale_120 = size.scale_120 ? size.scale_120 : 120,
    };
}

/* The process-wide font settings the app applies. smoothing has an
 * effect only with Core Text. */
struct font_settings {
    const char *family;
    int size_pt;
    enum font_hint_style hinting;
    enum font_aa_mode antialias;
    bool smoothing;
};
/* Applies next. With prev, sets only what differs from prev. */
void font_apply_settings(const struct font_settings *prev,
                         const struct font_settings *next);

/* For tests and the system layer only. Other code calls
 * font_system_init() and font_system_shutdown(). */
bool font_backend_init(void);
void font_backend_shutdown(void);

/* Loads this thread's backend state ahead of use. Every backend call
 * loads it on demand too. */
bool font_thread_attach(void);
/* Frees this thread's backend state. Call it before the thread exits. */
void font_thread_detach(void);

/* Bumped when the faces change: a hinting change, a family change,
 * font_backend_shutdown(), or a CoreText font registration. */
uint64_t font_generation(void);
void font_bump_generation(void);

#endif
