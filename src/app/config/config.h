#ifndef APP_CONFIG_CONFIG_H
#define APP_CONFIG_CONFIG_H

#include <stdbool.h>

#include "font/font.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "terminal/terminal.h"
#include "util/options.h"
#include "ui/yasl/window.h"

/* Every setting the app loads, saves, and applies. The windows and the
 * terminal read the parts they need. The app updates it in place, so
 * their pointers stay valid. */
struct app_config {
    struct window_config window;
    struct terminal_config terminal;
    struct wsap_vtcore_theme palette;
    /* Owned. */
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

/* The option table. Pass it to the util/options functions. */
extern const struct opts_spec app_config_spec;

/* Writes the defaults. c must be zeroed first. */
void app_config_defaults(struct app_config *c);
/* The font settings that c sets. Borrows c's family string. */
struct font_settings app_config_font_settings(const struct app_config *c);

#endif
