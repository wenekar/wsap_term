#ifndef APP_CONFIG_CONFIG_H
#define APP_CONFIG_CONFIG_H

#include <stdbool.h>

#include "font/font.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "terminal/terminal.h"
#include "util/options.h"
#include "ui/yasl/window.h"

struct app_config {
    struct window_config window;
    struct terminal_config terminal;
    struct wsap_vtcore_theme palette;
    char *theme;
    char *debug_socket;
    enum font_hint_style hinting;
    enum font_aa_mode antialias;
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    bool font_smoothing;
#endif
    bool high_contrast;
    int stats_ms;
};

extern const struct opts_spec app_config_spec;

void app_config_defaults(struct app_config *c);
struct font_settings app_config_font_settings(const struct app_config *c);

#endif
