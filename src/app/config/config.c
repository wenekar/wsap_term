#include "app/config/config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "util/name.h"

static bool hint_in_range(const struct opt *o, int v)
{
    return !v || (v >= o->min && v <= o->max);
}

static bool parse_hint_style_field(const struct opt *o, const char *val, void *field)
{
    enum font_hint_style v;
    if (!font_parse_hint_style(val, &v) || !hint_in_range(o, v))
        return false;
    *(enum font_hint_style *)field = v;
    return true;
}

static void format_hint_style(const struct opt *o, const void *field, char *buf, size_t n)
{
    (void)o;
    snprintf(buf, n, "%s", font_hint_style_name(*(const enum font_hint_style *)field));
}

static bool parse_aa_mode_field(const struct opt *o, const char *val, void *field)
{
    enum font_aa_mode v;
    if (!font_parse_aa_mode(val, &v) || !hint_in_range(o, v))
        return false;
    *(enum font_aa_mode *)field = v;
    return true;
}

static void format_aa_mode(const struct opt *o, const void *field, char *buf, size_t n)
{
    (void)o;
    snprintf(buf, n, "%s", font_aa_mode_name(*(const enum font_aa_mode *)field));
}

static bool parse_color(const struct opt *o, const char *val, void *field)
{
    (void)o;
    return wsap_vtcore_color_parse(val, field);
}

static void format_color(const struct opt *o, const void *field, char *buf, size_t n)
{
    (void)o;
    snprintf(buf, n, "#%06x", *(const uint32_t *)field & 0xFFFFFFu);
}

/* The choices offer "auto" first, then each value from min up to max. */
static const char *hint_style_choice(const struct opt *o, size_t index)
{
    if (!index)
        return "auto";
    if (index > (size_t)(o->max - o->min + 1))
        return NULL;
    int v = o->min + (int)index - 1;
    if (v > o->max)
        return NULL;
    return font_hint_style_name(v);
}

static const char *aa_mode_choice(const struct opt *o, size_t index)
{
    if (!index)
        return "auto";
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    static const char *const choices[] = { "auto", "grayscale", "mono" };
    return index < 3 ? choices[index] : NULL;
#else
    if (index > (size_t)(o->max - o->min + 1))
        return NULL;
    int v = o->min + (int)index - 1;
    if (v > o->max)
        return NULL;
    return font_aa_mode_name(v);
#endif
}

static const struct opt_type OPT_TYPE_HINT_STYLE = {
    .parse = parse_hint_style_field,
    .format = format_hint_style,
    .size = sizeof(enum font_hint_style),
    .kind = OPT_KIND_CHOICE,
    .choice = hint_style_choice,
};

static const struct opt_type OPT_TYPE_AA_MODE = {
    .parse = parse_aa_mode_field,
    .format = format_aa_mode,
    .size = sizeof(enum font_aa_mode),
    .kind = OPT_KIND_CHOICE,
    .choice = aa_mode_choice,
};

static const struct opt_type OPT_TYPE_COLOR = {
    .parse = parse_color,
    .format = format_color,
    .size = sizeof(uint32_t),
    .kind = OPT_KIND_COLOR,
};

static const struct enum_name CURSOR_NAMES[] = {
    { "block", WSAP_VTCORE_CURSOR_BLOCK },
    { "line", WSAP_VTCORE_CURSOR_BAR },
    { "bar", WSAP_VTCORE_CURSOR_BAR },
    { "underline", WSAP_VTCORE_CURSOR_UNDERLINE },
    { 0 },
};

static void set_defaults(void *base)
{
    struct app_config *c = base;
    bool sys_contrast = platform_prefers_high_contrast();
    c->window.font_size = FONT_DEFAULT_SIZE_PT;
    c->window.menu_hover_delay_ms = 250;
    c->terminal.scrollback = WSAP_VTCORE_SCROLLBACK_DEFAULT;
    c->terminal.cursor_blink = true;
    c->high_contrast = sys_contrast;
    if (sys_contrast)
        wsap_vtcore_theme_high_contrast(&c->palette);
    else
        wsap_vtcore_theme_default(&c->palette);
}

static const char *app_config_name(void)
{
    return app_name();
}

#define F(field) offsetof(struct app_config, field)
#define COLOR(i) \
    { .key = "color" #i, .type = &OPT_TYPE_COLOR, .off = F(palette.ansi[i]) }

static const struct opt OPTS[] = {
    { .key = "font", .type = &OPT_TYPE_TEXT, .off = F(window.font_family),
      .doc = "fontconfig pattern for the terminal font", .example = "monospace" },
    { .key = "font-size", .type = &OPT_TYPE_INT, .off = F(window.font_size),
      .min = FONT_MIN_SIZE_PT, .max = FONT_MAX_SIZE_PT,
      .doc = "font size in points" },
    { .key = "hinting", .type = &OPT_TYPE_HINT_STYLE, .off = F(hinting),
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
      .min = FONT_HINT_NONE, .max = FONT_HINT_NONE,
      .doc = "pixel positioning: none (unquantized), auto (native)" },
#else
      .min = FONT_HINT_NONE, .max = FONT_HINT_FULL,
      .doc = "hinting strength: none, slight, medium, full, auto" },
#endif
    { .key = "antialias", .type = &OPT_TYPE_AA_MODE, .off = F(antialias),
      .min = FONT_AA_GRAYSCALE, .max = FONT_AA_MONO,
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
      .doc = "antialiasing: mono, grayscale, auto" },
#else
      .doc = "antialiasing: mono, grayscale, rgb, bgr, vrgb, vbgr, auto" },
#endif
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    { .key = "font-smoothing", .type = &OPT_TYPE_BOOL, .off = F(font_smoothing),
      .doc = "enable native font smoothing" },
#endif
    { .key = "foreground", .type = &OPT_TYPE_COLOR, .off = F(palette.fg),
      .doc = "colors accept #rrggbb, #rgb, or rgb:rr/gg/bb" },
    { .key = "background", .type = &OPT_TYPE_COLOR, .off = F(palette.bg) },
    COLOR(0), COLOR(1), COLOR(2), COLOR(3), COLOR(4), COLOR(5), COLOR(6),
    COLOR(7), COLOR(8), COLOR(9), COLOR(10), COLOR(11), COLOR(12), COLOR(13),
    COLOR(14), COLOR(15),
    { .key = "cursor-style", .type = &OPT_TYPE_ENUM, .off = F(terminal.cursor_style),
      .names = CURSOR_NAMES, .doc = "cursor style: block, line, underline" },
    { .key = "cursor-blink", .type = &OPT_TYPE_BOOL, .off = F(terminal.cursor_blink),
      .doc = "blink the cursor" },
    { .key = "visual-bell", .type = &OPT_TYPE_BOOL, .off = F(terminal.visual_bell),
      .doc = "flash the screen on terminal bell" },
    { .key = "high-contrast", .type = &OPT_TYPE_BOOL, .off = F(high_contrast),
      .doc = "use high-contrast palette and controls" },
    { .key = "scrollback", .type = &OPT_TYPE_INT, .off = F(terminal.scrollback),
      .min = 0, .max = 1000000,
      .doc = "history lines retained; 0 to 1000000 (default 10000)" },
    { .key = "vsync", .type = &OPT_TYPE_NOT_BOOL, .off = F(window.tearing),
      .doc = "false enables tearing (uncapped FPS)" },
    { .key = "fullscreen", .type = &OPT_TYPE_BOOL, .off = F(window.fullscreen),
      .doc = "open windows fullscreen" },
    { .key = "width", .type = &OPT_TYPE_SIZE, .off = F(window.size.x), .min = 64, .max = 1 << 15,
      .doc = "window size in pixels; 0 derives 132x42 cells" },
    { .key = "height", .type = &OPT_TYPE_SIZE, .off = F(window.size.y), .min = 64, .max = 1 << 15 },
    { .key = "command", .type = &OPT_TYPE_TEXT, .off = F(terminal.command),
      .doc = "program to run; default is the login shell ($SHELL)",
      .example = "/bin/sh" },
    { .key = "theme", .type = &OPT_TYPE_TEXT, .off = F(theme),
      .doc = "chrome theme name (see assets/themes) or path to a .css file",
      .example = "breeze-dark" },
    { .key = "css-path", .type = &OPT_TYPE_TEXT, .off = F(theme), .unlisted = true },
    { .key = "menu-hover-delay", .type = &OPT_TYPE_INT, .off = F(window.menu_hover_delay_ms),
      .max = 10000, .doc = "context menu submenu hover delay in milliseconds" },
    { .key = "debug-socket", .type = &OPT_TYPE_TEXT, .off = F(debug_socket),
      .unlisted = true,
      .doc = "serve the UI inspector on a Unix socket; see scripts/wsap-debug" },
    { .key = "stats-ms", .type = &OPT_TYPE_INT, .off = F(stats_ms), .max = 1 << 30,
      .unlisted = true, .doc = "print frame stats every N ms" },
};

#undef COLOR
#undef F

const struct opts_spec app_config_spec = {
    .table = OPTS,
    .count = sizeof(OPTS) / sizeof(OPTS[0]),
    .size = sizeof(struct app_config),
    .defaults = set_defaults,
    .name = app_config_name,
    .env_prefix = "WSAP_",
    .config_env = "WSAP_CONFIG",
    .config_file = "wsap_term/term.conf",
    .file_note =
        "# WSAP_<KEY> environment variables override this file, and --key\n"
        "# arguments override both (font-size: WSAP_FONT_SIZE, --font-size).\n",
    .usage_tail =
        "  --background, --color0 .. --color15 set the other colors.\n"
        "Every option also reads WSAP_<KEY>, e.g. WSAP_FONT_SIZE.\n"
        "Precedence: defaults < term.conf < environment < arguments.\n",
};

void app_config_defaults(struct app_config *c)
{
    set_defaults(c);
}

struct font_settings app_config_font_settings(const struct app_config *c)
{
    return (struct font_settings){
        .family = c->window.font_family,
        .size_pt = c->window.font_size,
        .hinting = c->hinting,
        .antialias = c->antialias,
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
        .smoothing = c->font_smoothing,
#endif
    };
}
