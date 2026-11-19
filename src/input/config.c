#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util/app_identity.h"
#include "input/command.h"
#include "input/keybind.h"
#include "util/config.h"
#include "util/fs.h"
#include "util/path.h"

static bool input_config_default_path(const struct app_identity *identity,
                                      char *out, size_t sz)
{
    return app_identity_config_path(identity, out, sz, "input.conf");
}

static void write_default_input_config(const struct app_identity *identity,
                                       const char *path)
{
    FILE *f = fs_create(identity->id, path);
    if (!f)
        return;
    fprintf(f,
        "# %s input configuration.\n"
        "# Lines are 'chord = action'; everything here is commented out.\n\n"
        "# 'style' picks the built-in copy and paste keys for the whole\n"
        "# file, wherever the line is. Both styles also bind ctrl+shift+c\n"
        "# and ctrl+shift+v.\n"
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
        "#   undo redo cut select_all command_palette settings\n"
        "#   debug_overlay\n"
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
        "#cmd+f6 = focus_tabs (macOS)\n", identity->id);
    fclose(f);
}

/* The log prefix of the file that loads now. Initialization is one thread. */
static const char *config_tag = "";

/* The style and the path of the app configuration. The app sets them once,
 * before it opens a window. The window host then reads the file once for
 * all its windows, before its first window and at each reload. */
static enum input_keybind_style config_style = INPUT_KEYBIND_MODERN;
static char config_path[4096];
static bool config_has_path;

/* One parse of the configuration file. The init pass finds the style. The
 * load pass keeps that style and applies the bindings. */
struct input_config_pass {
    struct input_keymap *km;
    bool find_style;
};

static void input_config_line(void *ctx, const char *path, int line,
                              const char *chord, const char *val)
{
    struct input_config_pass *pass = ctx;
    struct input_keymap *km = pass->km;
    const char *tag = config_tag;
    if (pass->find_style) {
        if (val && !strcasecmp(chord, "style")) {
            enum input_keybind_style style;
            if (input_parse_keybind_style(val, &style))
                config_style = style;
            else
                fprintf(stderr, "[%s] %s:%d: invalid style '%s'\n",
                        tag, path, line, val);
        }
        return;
    }
    if (!val) {
        if (!strcasecmp(chord, "clear")) {
            /* The file holds app bindings, so a clear line keeps the
             * bindings of the widget scopes. */
            input_keymap_clear_scope(km, INPUT_COMMAND_SCOPE_APP);
            return;
        }
        fprintf(stderr, "[%s] %s:%d: expected 'chord = action'\n",
                tag, path, line);
        return;
    }

    if (!strcasecmp(chord, "style")) {
        /* The style applies to the whole file, so the find pass already
         * selected it. A style line here must not restore the defaults after
         * a clear line. */
        return;
    }

    char spec[256];
    if (snprintf(spec, sizeof(spec), "%s=%s", chord, val) >= (int)sizeof(spec) ||
        !input_keymap_apply_spec(km, spec))
        fprintf(stderr, "[%s] %s:%d: cannot bind '%s' to '%s'\n",
                tag, path, line, chord, val);
}

/* Reads the style line of the file. A missing line keeps the current
 * style. */
static void input_config_find_style(void)
{
    if (!config_has_path)
        return;
    struct input_config_pass pass = { .km = NULL, .find_style = true };
    if (!config_parse_file(config_path, input_config_line, &pass) &&
        errno != ENOENT)
        fprintf(stderr, "[%s] cannot read %s: %s\n", config_tag, config_path,
                strerror(errno));
}

void input_config_load(struct input_keymap *km)
{
    if (!km)
        return;
    /* A load reads the style again. The window host loads again on a
     * reload, so a style change and a binding change take effect without a
     * restart. */
    config_style = INPUT_KEYBIND_MODERN;
    input_config_find_style();
    input_keymap_reset(km, config_style);
    if (!config_has_path)
        return;
    struct input_config_pass pass = { .km = km };
    if (!config_parse_file(config_path, input_config_line, &pass) &&
        errno != ENOENT)
        fprintf(stderr, "[%s] cannot read %s: %s\n", config_tag, config_path,
                strerror(errno));
}

void input_config_init(const struct app_identity *identity)
{
    if (identity)
        config_tag = identity->id;
    config_style = INPUT_KEYBIND_MODERN;
    config_has_path = false;

    const char *path = getenv("WSAP_INPUT_CONFIG");
    if (path && !path[0])
        path = NULL;
    if (!path && input_config_default_path(identity, config_path,
                                           sizeof(config_path)))
        path = config_path;
    if (!path || !strcasecmp(path, "NONE"))
        return;
    if (path != config_path && strlen(path) < sizeof(config_path)) {
        memcpy(config_path, path, strlen(path) + 1);
    }
    config_has_path = true;
    if (access(config_path, R_OK) != 0 && errno == ENOENT)
        write_default_input_config(identity, config_path);
    /* The find pass selects the style that each window then loads. The
     * style applies to the whole file. The app keeps no keymap of its own. */
    config_style = INPUT_KEYBIND_MODERN;
    input_config_find_style();
}
