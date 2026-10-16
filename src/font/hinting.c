#define _GNU_SOURCE
#include "font/internal.h"
#if !defined(__APPLE__) || defined(WSAP_USE_FREETYPE)
#include "font/backends/freetype/internal.h"
#endif

#include <strings.h>

#if !defined(__APPLE__) || defined(WSAP_USE_FREETYPE)
#include FT_LCD_FILTER_H
#include <fontconfig/fontconfig.h>
#include <hb-ft.h>

static enum font_hint_style current_style;
static enum font_aa_mode current_aa;
static FT_LcdFilter lcd_filter = FT_LCD_FILTER_DEFAULT;

bool fc_ready;

static inline bool is_subpixel_aa(enum font_aa_mode aa)
{
    return aa >= FONT_AA_RGB && aa <= FONT_AA_VBGR;
}

/* Only ft_apply_hinting resolves an auto axis. ft_init calls it when
 * fontconfig becomes ready. Each mode change calls it too. So while
 * fontconfig is ready, no axis is auto here. The getters and
 * font_hinting_init_library only read. */
int font_ft_load_flags(void)
{
    int hint = current_style == FONT_HINT_NONE ? FT_LOAD_NO_HINTING : 0;
    /* Mono has no light hinting: any style but none is full. */
    if (current_aa == FONT_AA_MONO)
        return hint | FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME;
    if (current_style == FONT_HINT_SLIGHT)
        return hint | FT_LOAD_TARGET_LIGHT;
    switch (current_aa) {
    case FONT_AA_RGB:
    case FONT_AA_BGR:
        return hint | FT_LOAD_TARGET_LCD;
    case FONT_AA_VRGB:
    case FONT_AA_VBGR:
        return hint | FT_LOAD_TARGET_LCD_V;
    default:
        break;
    }
    return hint | FT_LOAD_TARGET_NORMAL;
}

FT_Render_Mode font_ft_render_mode(void)
{
    switch (current_aa) {
    case FONT_AA_MONO:
        return FT_RENDER_MODE_MONO;
    case FONT_AA_RGB:
    case FONT_AA_BGR:
        return FT_RENDER_MODE_LCD;
    case FONT_AA_VRGB:
    case FONT_AA_VBGR:
        return FT_RENDER_MODE_LCD_V;
    default:
        return FT_RENDER_MODE_NORMAL;
    }
}

_Static_assert(FONT_HINT_FULL < 16 && FONT_AA_MONO < 16,
               "raster_hint_key packs each axis into 4 bits");

uint8_t raster_hint_key(void)
{
    return (uint8_t)(current_style | (current_aa << 4));
}

static void fontconfig_query_hinting(enum font_hint_style *style, enum font_aa_mode *aa)
{
    *style = FONT_HINT_SLIGHT;
    *aa = FONT_AA_GRAYSCALE;
    FcPattern *pat = FcPatternCreate();
    if (!pat)
        return;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"monospace");
    FcPattern *match = fc_font_match(pat);
    FcPatternDestroy(pat);
    if (!match)
        return;

    FcBool antialias = FcTrue, hinting = FcTrue;
    int rgba = FC_RGBA_UNKNOWN;
    int hintstyle = -1;
    int filter = FC_LCD_DEFAULT;
    FcPatternGetBool(match, FC_ANTIALIAS, 0, &antialias);
    FcPatternGetBool(match, FC_HINTING, 0, &hinting);
    FcPatternGetInteger(match, FC_RGBA, 0, &rgba);
    FcPatternGetInteger(match, FC_HINT_STYLE, 0, &hintstyle);
    FcPatternGetInteger(match, FC_LCD_FILTER, 0, &filter);
    FcPatternDestroy(match);

    switch (filter) {
    case FC_LCD_NONE:
        lcd_filter = FT_LCD_FILTER_NONE;
        break;
    case FC_LCD_LIGHT:
        lcd_filter = FT_LCD_FILTER_LIGHT;
        break;
    case FC_LCD_LEGACY:
        lcd_filter = FT_LCD_FILTER_LEGACY;
        break;
    default:
        lcd_filter = FT_LCD_FILTER_DEFAULT;
        break;
    }

    if (antialias == FcFalse)
        *aa = FONT_AA_MONO;
    else switch (rgba) {
    case FC_RGBA_RGB:  *aa = FONT_AA_RGB;  break;
    case FC_RGBA_BGR:  *aa = FONT_AA_BGR;  break;
    case FC_RGBA_VRGB: *aa = FONT_AA_VRGB; break;
    case FC_RGBA_VBGR: *aa = FONT_AA_VBGR; break;
    }

    if (hinting == FcFalse)
        *style = FONT_HINT_NONE;
    else switch (hintstyle) {
    case FC_HINT_NONE:   *style = FONT_HINT_NONE;   break;
    case FC_HINT_MEDIUM: *style = FONT_HINT_MEDIUM; break;
    case FC_HINT_FULL:   *style = FONT_HINT_FULL;   break;
    default:             break;
    }
}

static void resolve_hinting(void)
{
    if (!fc_ready)
        return;
    if (current_style != FONT_HINT_AUTO &&
        current_aa != FONT_AA_AUTO)
        return;
    enum font_hint_style style;
    enum font_aa_mode aa;
    fontconfig_query_hinting(&style, &aa);
    if (current_style == FONT_HINT_AUTO)
        current_style = style;
    if (current_aa == FONT_AA_AUTO)
        current_aa = aa;
}

void font_hinting_init_library(FT_Library lib)
{
    if (lib && is_subpixel_aa(current_aa))
        FT_Library_SetLcdFilter(lib, lcd_filter);
}

void ft_apply_hinting(void)
{
    resolve_hinting();
    raster_cache_clear();
    int flags = font_ft_load_flags();
    if (ft_lib)
        font_hinting_init_library(ft_lib);
    for (int i = 0; i < num_faces; i++)
        for (int s = 0; s < FONT_VARIANT_COUNT; s++)
            if (faces[i].hb[s])
                hb_ft_font_set_load_flags(faces[i].hb[s], flags);
    atomic_fetch_add_explicit(&ft_sync_epoch, 1, memory_order_relaxed);
    font_bump_generation();
}

void font_set_hint_style(enum font_hint_style style)
{
    current_style = style;
    ft_apply_hinting();
}

void font_set_aa_mode(enum font_aa_mode aa)
{
    current_aa = aa;
    ft_apply_hinting();
}

bool font_hinting_has_auto_axis(void)
{
    return current_style == FONT_HINT_AUTO || current_aa == FONT_AA_AUTO;
}
#endif

enum { NAME_STYLE = 1, NAME_AA = 2, NAME_CT = 4 };

#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
#define NAME_BACKEND NAME_CT
#else
#define NAME_BACKEND 0
#endif

_Static_assert(FONT_HINT_AUTO == 0 && FONT_AA_AUTO == 0,
               "the auto names serve both axes");

/* The first name of each value on an axis is its canonical one.
 * CoreText accepts only the names marked NAME_CT. */
static const struct hint_name {
    const char *name;
    uint8_t value;
    uint8_t flags;
} hint_names[] = {
    { "auto", 0, NAME_STYLE | NAME_AA | NAME_CT },
    { "system", 0, NAME_STYLE | NAME_AA | NAME_CT },
    { "default", 0, NAME_STYLE | NAME_AA | NAME_CT },
    { "none", FONT_HINT_NONE, NAME_STYLE | NAME_CT },
    { "off", FONT_HINT_NONE, NAME_STYLE | NAME_CT },
    { "false", FONT_HINT_NONE, NAME_STYLE }, { "0", FONT_HINT_NONE, NAME_STYLE },
    { "no", FONT_HINT_NONE, NAME_STYLE },
    { "none hinting", FONT_HINT_NONE, NAME_STYLE },
    { "slight", FONT_HINT_SLIGHT, NAME_STYLE },
    { "light", FONT_HINT_SLIGHT, NAME_STYLE },
    { "slight hinting", FONT_HINT_SLIGHT, NAME_STYLE },
    { "medium", FONT_HINT_MEDIUM, NAME_STYLE },
    { "medium hinting", FONT_HINT_MEDIUM, NAME_STYLE },
    { "full", FONT_HINT_FULL, NAME_STYLE }, { "normal", FONT_HINT_FULL, NAME_STYLE },
    { "full hinting", FONT_HINT_FULL, NAME_STYLE },
    { "grayscale", FONT_AA_GRAYSCALE, NAME_AA | NAME_CT },
    { "gray", FONT_AA_GRAYSCALE, NAME_AA },
    { "greyscale", FONT_AA_GRAYSCALE, NAME_AA },
    { "grey", FONT_AA_GRAYSCALE, NAME_AA },
    { "rgb", FONT_AA_RGB, NAME_AA }, { "subpixel", FONT_AA_RGB, NAME_AA },
    { "lcd", FONT_AA_RGB, NAME_AA }, { "lcd-rgb", FONT_AA_RGB, NAME_AA },
    { "bgr", FONT_AA_BGR, NAME_AA }, { "lcd-bgr", FONT_AA_BGR, NAME_AA },
    { "vrgb", FONT_AA_VRGB, NAME_AA }, { "vertical-rgb", FONT_AA_VRGB, NAME_AA },
    { "lcd-vrgb", FONT_AA_VRGB, NAME_AA },
    { "vbgr", FONT_AA_VBGR, NAME_AA }, { "vertical-bgr", FONT_AA_VBGR, NAME_AA },
    { "lcd-vbgr", FONT_AA_VBGR, NAME_AA },
    { "mono", FONT_AA_MONO, NAME_AA | NAME_CT },
    { "monochrome", FONT_AA_MONO, NAME_AA },
    { "aliased", FONT_AA_MONO, NAME_AA }, { "1bit", FONT_AA_MONO, NAME_AA },
};

#define HINT_NAMES (sizeof(hint_names) / sizeof(hint_names[0]))

static bool name_offered(const struct hint_name *n, unsigned axis)
{
    unsigned need = axis | NAME_BACKEND;
    return (n->flags & need) == need;
}

static const struct hint_name *name_parse(const char *str, unsigned axis)
{
    if (!str)
        return NULL;
    for (size_t i = 0; i < HINT_NAMES; i++)
        if (name_offered(&hint_names[i], axis) &&
            strcasecmp(str, hint_names[i].name) == 0)
            return &hint_names[i];
    return NULL;
}

static const char *name_of(unsigned value, unsigned axis)
{
    for (size_t i = 0; i < HINT_NAMES; i++)
        if (name_offered(&hint_names[i], axis) && hint_names[i].value == value)
            return hint_names[i].name;
    return "auto";
}

bool font_parse_hint_style(const char *str, enum font_hint_style *out)
{
    const struct hint_name *n = out ? name_parse(str, NAME_STYLE) : NULL;
    if (n)
        *out = n->value;
    return n;
}

bool font_parse_aa_mode(const char *str, enum font_aa_mode *out)
{
    const struct hint_name *n = out ? name_parse(str, NAME_AA) : NULL;
    if (n)
        *out = n->value;
    return n;
}

const char *font_hint_style_name(enum font_hint_style style)
{
    return name_of(style, NAME_STYLE);
}

const char *font_aa_mode_name(enum font_aa_mode aa)
{
    return name_of(aa, NAME_AA);
}
