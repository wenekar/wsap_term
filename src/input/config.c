#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/name.h"
#include "input/keybind.h"
#include "util/config.h"
#include "util/fs.h"
#include "util/path.h"

static bool input_config_default_path(char *out, size_t sz)
{
    return xdg_config_path(out, sz, "wsap_term/input.conf");
}

static void write_default_input_config(const char *path)
{
    FILE *f = fs_create(path);
    if (!f)
        return;
    fprintf(f,
        "# wsap_term input configuration.\n"
        "# Lines are 'chord = action'; everything here is commented out.\n\n"
        "# 'style' picks the built-in copy and paste keys before any\n"
        "# binding below. Both styles also bind ctrl+shift+c and ctrl+shift+v.\n"
        "#   modern  (default) ctrl+c copies when text is selected, otherwise\n"
        "#           it reaches the shell (interrupt); ctrl+v pastes\n"
        "#   legacy  plain ctrl+c and ctrl+v always reach the shell\n"
        "# The tab shortcuts use ctrl+shift in both styles.\n"
        "# Ignored on macOS, where copy and paste use cmd.\n"
        "#style = modern\n\n"
        "# 'clear' on a line by itself drops every default binding.\n"
        "#clear\n\n"
        "# modifiers: ctrl, shift, alt, super (cmd), hyper, meta\n"
        "# keys: a single character, f1-f35, or one of\n"
        "#   enter backspace tab escape space insert delete\n"
        "#   up down left right home end pageup pagedown\n"
        "#   kp_add kp_subtract kp_enter\n"
        "# actions:\n"
        "#   copy paste font_larger font_smaller scroll_up scroll_down\n"
        "#   new_tab close_tab next_tab prev_tab last_tab tab_1 .. tab_8\n"
        "#   focus_tabs toggles focus for keyboard tab navigation\n"
        "#   kill_line (sends ^U)\n"
        "#   none (unbinds the chord)\n\n"
        "#ctrl+shift+t = new_tab\n"
        "#ctrl+shift+w = close_tab\n"
        "#ctrl+tab = next_tab\n"
        "#ctrl+shift+tab = prev_tab\n"
        "#ctrl+shift+1 = tab_1\n"
        "#ctrl+shift+9 = last_tab\n"
        "#ctrl+shift+f6 = focus_tabs (Linux)\n"
        "#cmd+f6 = focus_tabs (macOS)\n");
    fclose(f);
}

static void input_config_line(void *ctx, const char *path, int line,
                              const char *chord, const char *val)
{
    struct input_keymap *km = ctx;
    if (!val) {
        if (!strcasecmp(chord, "clear")) {
            input_keymap_clear(km);
            return;
        }
        fprintf(stderr, "[%s] %s:%d: expected 'chord = action'\n",
                app_name(), path, line);
        return;
    }

    if (!strcasecmp(chord, "style")) {
        enum input_keybind_style style;
        if (input_parse_keybind_style(val, &style))
            input_keymap_reset_active(style);
        else
            fprintf(stderr, "[%s] %s:%d: invalid style '%s'\n",
                    app_name(), path, line, val);
        return;
    }

    char spec[256];
    if (snprintf(spec, sizeof(spec), "%s=%s", chord, val) >= (int)sizeof(spec) ||
        !input_keymap_apply_spec(km, spec))
        fprintf(stderr, "[%s] %s:%d: cannot bind '%s' to '%s'\n",
                app_name(), path, line, chord, val);
}

static void input_config_load(const char *path)
{
    if (!config_parse_file(path, input_config_line, input_keymap_active())) {
        if (errno == ENOENT)
            write_default_input_config(path);
        else
            fprintf(stderr, "[%s] cannot read %s: %s\n",
                    app_name(), path, strerror(errno));
    }
}

void input_config_init(void)
{
    input_keymap_reset_active(INPUT_KEYBIND_MODERN);

    const char *path = getenv("WSAP_INPUT_CONFIG");
    if (path && !path[0])
        path = NULL;
    char defbuf[4096];
    if (!path && input_config_default_path(defbuf, sizeof(defbuf)))
        path = defbuf;
    if (path && strcasecmp(path, "NONE"))
        input_config_load(path);
}
