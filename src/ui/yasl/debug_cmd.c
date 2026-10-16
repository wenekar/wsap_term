#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wsap_vtcore/unicode/utf8.h"
#include "ui/yasl/debug_internal.h"
#include "font/font.h"

enum dbg_status {
    DBG_OK,
    DBG_FAILED,
    DBG_PENDING,
};

struct dbg_cmd {
    struct dbg_client *client;
    int argc;
    char **argv;
    struct window *window;
    int surface;
    unsigned mods;
    char err[512];
};

static enum dbg_status cmd_fail(struct dbg_cmd *cmd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static enum dbg_status cmd_fail(struct dbg_cmd *cmd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd->err, sizeof(cmd->err), fmt, ap);
    va_end(ap);
    return DBG_FAILED;
}

/* ---- windows ------------------------------------------------------ */

static struct window *window_at(int index)
{
    struct window_host *host = window_ui_host_last;
    if (!host || index < 0 || index >= host->count)
        return NULL;
    return host->windows[index];
}

static int window_index_of(const struct window *w)
{
    struct window_host *host = window_ui_host_last;
    for (int i = 0; host && i < host->count; i++)
        if (host->windows[i] == w)
            return i;
    return -1;
}

static struct window *default_window(void)
{
    struct window_host *host = window_ui_host_last;
    return host ? host->primary : NULL;
}

static void write_window_summary(struct json_writer *j, struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    json_obj_begin(j);
    json_key(j, "index"); json_int(j, window_index_of(w));
    json_key(j, "type"); json_str(j, ui && ui->spec.name ? ui->spec.name : "window");
    json_key(j, "primary"); json_bool(j, default_window() == w);
    json_key(j, "size");
    json_vec(j, window_size(w));
    json_key(j, "frame"); json_uint(j, window_frame_count(w));
    json_key(j, "menu_open"); json_bool(j, ui && ui_menu_is_open(&ui->menu));
    json_key(j, "menu_levels"); json_int(j, ui ? ui_menu_level_count(&ui->menu) : 0);
    json_key(j, "menu_popups"); json_int(j, ui ? ui->menu_popups : 0);
    if (ui && ui->spec.debug_state)
        ui->spec.debug_state(w, j);
    json_obj_end(j);
}

static void write_widget_ref(struct json_writer *j, const struct ui_debug_view *view,
                             const struct ui_widget *w)
{
    char path[256];
    if (!w || !ui_debug_path(view->root, w, path, sizeof(path))) {
        json_null(j);
        return;
    }
    json_obj_begin(j);
    json_key(j, "type"); json_str(j, ui_debug_type_name(w));
    json_key(j, "path"); json_str(j, path);
    json_obj_end(j);
}

/* ---- argument helpers --------------------------------------------- */

static bool parse_int(const char *s, int *out)
{
    char *end;
    long v = s ? strtol(s, &end, 10) : 0;
    if (!s || !*s || *end || v < -1000000 || v > 1000000)
        return false;
    *out = (int)v;
    return true;
}

static enum dbg_status int_arg(struct dbg_cmd *cmd, const char *s, int *out)
{
    return parse_int(s, out) ? DBG_OK
        : cmd_fail(cmd, "expected an integer, got '%s'", s ? s : "");
}

static enum dbg_status view_of(struct dbg_cmd *cmd, struct ui_debug_view *view)
{
    struct window *w = cmd->window;
    struct window_ui *ui = window_ui_of(w);
    if (ui && cmd->surface == WINDOW_DEBUG_MAIN) {
        ui_surface_prepare(&ui->screen);
    } else if (ui && dbg_surface_exists(w, cmd->surface)) {
        ui_surface_prepare(&ui->menu_popup[cmd->surface - 1].ui);
    }
    *view = dbg_view(cmd->window, cmd->surface);
    return view->root ? DBG_OK
        : cmd_fail(cmd, "surface %d has no widget tree", cmd->surface);
}

static enum dbg_status scene_of(struct dbg_cmd *cmd, struct ui_debug_view *view)
{
    enum dbg_status st = view_of(cmd, view);
    if (st == DBG_OK && !view->scene)
        return cmd_fail(cmd, "no scene captured yet; run 'frame' first");
    return st;
}

static enum dbg_status find(struct dbg_cmd *cmd, const struct ui_debug_view *view,
                            const char *sel, const struct ui_widget **out)
{
    *out = ui_debug_find(view->root, sel);
    return *out ? DBG_OK : cmd_fail(cmd, "no widget matches '%s'", sel);
}

/* ---- inspection --------------------------------------------------- */

static enum dbg_status cmd_windows(struct dbg_cmd *cmd, struct json_writer *j)
{
    (void)cmd;
    json_arr_begin(j);
    struct window_host *host = window_ui_host_last;
    for (int i = 0; host && i < host->count; i++)
        write_window_summary(j, host->windows[i]);
    json_arr_end(j);
    return DBG_OK;
}

static enum dbg_status cmd_state(struct dbg_cmd *cmd, struct json_writer *j)
{
    struct window *w = cmd->window;
    struct ui_debug_view view = dbg_view(w, WINDOW_DEBUG_MAIN);
    struct window_pointer pointer = window_pointer(w);
    json_obj_begin(j);
    json_key(j, "window"); write_window_summary(j, w);
    json_key(j, "pointer");
    json_obj_begin(j);
    json_key(j, "pos"); json_vec(j, pointer.pos);
    json_key(j, "inside"); json_bool(j, pointer.inside);
    json_obj_end(j);
    json_key(j, "focused");
    write_widget_ref(j, &view, ui_surface_focused(view.surface));
    json_key(j, "captured");
    write_widget_ref(j, &view, ui_surface_captured(view.surface));
    json_key(j, "overlay");
    const struct window_ui *ui = window_ui_of(w);
    json_uint(j, ui && ui->debug ? ui->debug->overlay.flags : 0);
    json_obj_end(j);
    return DBG_OK;
}

static enum dbg_status cmd_tree(struct dbg_cmd *cmd, struct json_writer *j)
{
    struct ui_debug_view view;
    const struct ui_widget *start = NULL;
    int depth = -1;
    enum dbg_status st = view_of(cmd, &view);
    for (int i = 1; st == DBG_OK && i < cmd->argc; i++) {
        if (!strcmp(cmd->argv[i], "--depth") && i + 1 < cmd->argc)
            st = int_arg(cmd, cmd->argv[++i], &depth);
        else
            st = find(cmd, &view, cmd->argv[i], &start);
    }
    if (st == DBG_OK)
        ui_debug_write_tree(j, &view, start, depth);
    return st;
}

static enum dbg_status cmd_widget(struct dbg_cmd *cmd, struct json_writer *j)
{
    struct ui_debug_view view;
    const struct ui_widget *w;
    enum dbg_status st = view_of(cmd, &view);
    if (st == DBG_OK)
        st = find(cmd, &view, cmd->argc > 1 ? cmd->argv[1] : "/", &w);
    if (st == DBG_OK)
        ui_debug_write_widget(j, &view, w);
    return st;
}

static enum dbg_status cmd_hit(struct dbg_cmd *cmd, struct json_writer *j)
{
    if (cmd->argc < 3)
        return cmd_fail(cmd, "usage: hit X Y");
    struct ui_debug_view view;
    struct vector2i p;
    enum dbg_status st = view_of(cmd, &view);
    if (st == DBG_OK)
        st = int_arg(cmd, cmd->argv[1], &p.x);
    if (st == DBG_OK)
        st = int_arg(cmd, cmd->argv[2], &p.y);
    if (st == DBG_OK)
        ui_debug_write_hit(j, &view, p);
    return st;
}

static enum dbg_status cmd_scene(struct dbg_cmd *cmd, struct json_writer *j)
{
    struct ui_debug_view view;
    const struct ui_widget *filter = NULL;
    enum dbg_status st = scene_of(cmd, &view);
    if (st == DBG_OK && cmd->argc > 1)
        st = find(cmd, &view, cmd->argv[1], &filter);
    if (st == DBG_OK)
        ui_debug_write_scene(j, &view, filter);
    return st;
}

static enum dbg_status cmd_lint(struct dbg_cmd *cmd, struct json_writer *j)
{
    struct ui_debug_view view;
    enum dbg_status st = scene_of(cmd, &view);
    if (st == DBG_OK)
        ui_debug_write_lint(j, &view);
    return st;
}

/* ---- overlay ------------------------------------------------------ */

static const struct {
    const char *name;
    unsigned flag;
} OVERLAY_FLAGS[] = {
    { "bounds", UI_DEBUG_OVERLAY_BOUNDS },
    { "box", UI_DEBUG_OVERLAY_BOX },
    { "text", UI_DEBUG_OVERLAY_TEXT },
    { "parts", UI_DEBUG_OVERLAY_PARTS },
    { "label", UI_DEBUG_OVERLAY_LABEL },
    { "lint", UI_DEBUG_OVERLAY_LINT },
};

#define N_OVERLAY_FLAGS (sizeof(OVERLAY_FLAGS) / sizeof(OVERLAY_FLAGS[0]))

static bool overlay_flag(const char *name, unsigned *out)
{
    if (!strcmp(name, "all")) {
        *out = ~0u;
        return true;
    }
    if (!strcmp(name, "default") || !strcmp(name, "on")) {
        *out = UI_DEBUG_OVERLAY_DEFAULT;
        return true;
    }
    for (size_t i = 0; i < N_OVERLAY_FLAGS; i++) {
        if (!strcmp(name, OVERLAY_FLAGS[i].name)) {
            *out = OVERLAY_FLAGS[i].flag;
            return true;
        }
    }
    return false;
}

/* overlay [on|off|toggle|all|default|FLAG|+FLAG|-FLAG ...]
 *         [select SELECTOR|none] */
static enum dbg_status cmd_overlay(struct dbg_cmd *cmd, struct json_writer *j)
{
    struct window *w = cmd->window;
    struct window_debug *dbg = dbg_state(w);
    if (!dbg)
        return cmd_fail(cmd, "out of memory");
    struct ui_debug_view view = dbg_view(w, cmd->surface);
    unsigned flags = dbg->overlay.flags;
    const struct ui_widget *selected = dbg->overlay.selected;
    bool replaced = false;
    for (int i = 1; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        unsigned f;
        if (!strcmp(a, "off")) {
            flags = 0;
        } else if (!strcmp(a, "toggle")) {
            flags = flags ? 0 : UI_DEBUG_OVERLAY_DEFAULT;
        } else if (!strcmp(a, "select") && i + 1 < cmd->argc) {
            const char *sel = cmd->argv[++i];
            if (!strcmp(sel, "none"))
                selected = NULL;
            else if (!view.root || find(cmd, &view, sel, &selected) != DBG_OK)
                return view.root ? DBG_FAILED : cmd_fail(cmd, "no widget tree");
        } else if ((a[0] == '+' || a[0] == '-') && overlay_flag(a + 1, &f)) {
            flags = a[0] == '+' ? flags | f : flags & ~f;
        } else if (overlay_flag(a, &f)) {
            flags = replaced ? flags | f : f;
            replaced = true;
        } else {
            return cmd_fail(cmd, "unknown overlay argument '%s'", a);
        }
    }
    dbg->overlay.flags = flags;
    dbg->overlay.selected = selected;
    for (int s = 0; s < DBG_SURFACES; s++)
        if (dbg_surface_exists(w, s))
            dbg_redraw(w, s);

    json_obj_begin(j);
    json_key(j, "flags");
    json_arr_begin(j);
    for (size_t i = 0; i < N_OVERLAY_FLAGS; i++)
        if (flags & OVERLAY_FLAGS[i].flag)
            json_str(j, OVERLAY_FLAGS[i].name);
    json_arr_end(j);
    json_key(j, "selected");
    write_widget_ref(j, &view, selected);
    json_obj_end(j);
    return DBG_OK;
}

/* ---- frames ------------------------------------------------------- */

static enum dbg_status wait_frame(struct dbg_cmd *cmd, enum dbg_pending_kind kind,
                                  const char *path)
{
    return dbg_pending_add(cmd->client, cmd->window, cmd->surface, kind, path,
                           cmd->err, sizeof(cmd->err))
        ? DBG_PENDING : DBG_FAILED;
}

static enum dbg_status cmd_frame(struct dbg_cmd *cmd, struct json_writer *j)
{
    (void)j;
    return wait_frame(cmd, DBG_PENDING_FRAME, NULL);
}

static enum dbg_status cmd_screenshot(struct dbg_cmd *cmd, struct json_writer *j)
{
    (void)j;
    if (cmd->argc < 2)
        return cmd_fail(cmd, "usage: screenshot PATH.png");
    return wait_frame(cmd, DBG_PENDING_SHOT, cmd->argv[1]);
}

/* ---- input injection ---------------------------------------------- */

static enum dbg_status injected(struct dbg_cmd *cmd, bool ok)
{
    return ok ? DBG_OK : cmd_fail(cmd, "out of memory");
}

static uint32_t ascii_upper(uint32_t cp)
{
    return cp >= 'a' && cp <= 'z' ? cp - 'a' + 'A' : cp;
}

/* What the Wayland keyboard path produces for a chord, routed through
 * the keymap, so shortcuts fire as they do for real keys. */
static bool inject_key(struct window *w, unsigned mods, uint32_t cp, uint16_t key)
{
    struct window_debug *dbg = dbg_state(w);
    if (!dbg)
        return false;
    bool shift = mods & INPUT_MOD_SHIFT;
    uint32_t text = (mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT | INPUT_MOD_SUPER))
        ? 0 : (shift ? ascii_upper(cp) : cp);
    if (key == INPUT_KEY_RETURN) text = '\r';
    else if (key == INPUT_KEY_TAB) text = '\t';
    else if (key == INPUT_KEY_BACKSPACE) text = 0x7f;
    else if (key == INPUT_KEY_ESCAPE) text = 0x1b;
    else if (key) text = 0;
    unsigned key_id = key ? (256u + key) % INPUT_SHORTCUT_KEY_COUNT : cp % 256u;
    bool ok = true;
    for (int phase = 0; phase < 2; phase++) {
        struct input_key_event k = {
            .key = (enum input_key)key,
            .action = phase ? INPUT_RELEASE : INPUT_PRESS,
            .mods = mods,
            .codepoint = cp,
            .shifted = ascii_upper(cp),
            .base = cp,
            .text = text,
        };
        enum input_shortcut sc;
        enum input_key_route route =
            input_shortcut_route_key(input_keymap_active(), &dbg->scratch, &dbg->keys, &k, key_id, &sc);
        if (route == INPUT_KEY_PASSTHROUGH)
            ok &= input_queue_add(&dbg->scratch, &(struct input_event){
                .kind = INPUT_KEY, .key = k,
            }) != INPUT_PUSH_DROPPED;
        else if (route == INPUT_KEY_DROPPED)
            ok = false;
        ok &= dbg_inject_scratch(w);
    }
    return ok;
}

static enum dbg_status cmd_key(struct dbg_cmd *cmd, struct json_writer *j)
{
    if (cmd->argc < 2)
        return cmd_fail(cmd, "usage: key CHORD... (e.g. ctrl+shift+p, escape, down)");
    bool ok = true;
    for (int i = 1; i < cmd->argc; i++) {
        unsigned mods;
        uint32_t cp;
        uint16_t key;
        if (!input_parse_chord(cmd->argv[i], &mods, &cp, &key))
            return cmd_fail(cmd, "cannot parse key chord '%s'", cmd->argv[i]);
        ok &= inject_key(cmd->window, mods | cmd->mods, cp, key);
    }
    json_int(j, cmd->argc - 1);
    return injected(cmd, ok);
}

/* One key press per ASCII character; other characters go as text. */
static enum dbg_status cmd_type(struct dbg_cmd *cmd, struct json_writer *j)
{
    int n = 0;
    bool ok = true;
    for (int i = 1; i < cmd->argc; i++) {
        if (i > 1) {
            ok &= inject_key(cmd->window, cmd->mods, ' ', INPUT_KEY_NONE);
            n++;
        }
        for (const char *s = cmd->argv[i]; *s; n++) {
            unsigned char c = (unsigned char)*s;
            if (c >= 0x20 && c < 0x7f) {
                bool upper = c >= 'A' && c <= 'Z';
                ok &= inject_key(cmd->window, cmd->mods | (upper ? INPUT_MOD_SHIFT : 0),
                                 upper ? (uint32_t)(c - 'A' + 'a') : c, INPUT_KEY_NONE);
                s++;
                continue;
            }
            uint32_t cp;
            int len = wsap_vtcore_utf8_decode_one(s, strlen(s), &cp);
            if (len <= 0)
                len = 1;
            struct input_event ev = { .kind = INPUT_TEXT };
            memcpy(ev.text.utf8, s, (size_t)len);
            ev.text.len = (uint8_t)len;
            ok &= dbg_inject(cmd->window, &ev);
            s += len;
        }
    }
    json_int(j, n);
    return injected(cmd, ok);
}

static enum dbg_status cmd_text(struct dbg_cmd *cmd, struct json_writer *j)
{
    struct window_debug *dbg = dbg_state(cmd->window);
    if (!dbg)
        return cmd_fail(cmd, "out of memory");
    size_t total = 0;
    bool ok = true;
    for (int i = 1; i < cmd->argc; i++) {
        if (i > 1) {
            input_queue_push_text(&dbg->scratch, " ", 1, INPUT_TEXT_COMMIT);
            total++;
        }
        size_t len = strlen(cmd->argv[i]);
        input_queue_push_text(&dbg->scratch, cmd->argv[i], len, INPUT_TEXT_COMMIT);
        ok &= dbg_inject_scratch(cmd->window);
        total += len;
    }
    json_uint(j, total);
    return injected(cmd, ok);
}

static bool parse_button(const char *s, int *out)
{
    if (!strcmp(s, "left")) *out = 0;
    else if (!strcmp(s, "middle")) *out = 1;
    else if (!strcmp(s, "right")) *out = 2;
    else if (s[0] >= '0' && s[0] <= '9' && !s[1]) *out = s[0] - '0';
    else return false;
    return true;
}

static bool inject_pointer(struct dbg_cmd *cmd, enum pointer_kind kind,
                           struct vector2i pos, int button, int scroll)
{
    return dbg_inject(cmd->window, &(struct input_event){
        .kind = INPUT_MOUSE,
        .mouse = {
            .pos = pos,
            .button = button,
            .mods = cmd->mods,
            .scroll_lines = scroll,
            .kind = kind,
            .popup = cmd->surface,
        },
    });
}

/* move X Y | down X Y [BUTTON] | up X Y [BUTTON] | click X Y [BUTTON]
 * | scroll X Y LINES | leave */
static enum dbg_status cmd_pointer(struct dbg_cmd *cmd, struct json_writer *j)
{
    const char *verb = cmd->argv[0];
    json_str(j, verb);
    if (!strcmp(verb, "leave"))
        return injected(cmd, inject_pointer(cmd, PTR_LEAVE, window_pointer(cmd->window).pos, 0, 0));
    bool scroll = !strcmp(verb, "scroll");
    if (cmd->argc < 3)
        return cmd_fail(cmd, "usage: %s X Y%s", verb, scroll ? " LINES" : " [BUTTON]");
    struct vector2i p;
    int button = 0, lines = 0;
    enum dbg_status st = int_arg(cmd, cmd->argv[1], &p.x);
    if (st == DBG_OK)
        st = int_arg(cmd, cmd->argv[2], &p.y);
    if (st == DBG_OK && scroll)
        st = cmd->argc < 4 ? cmd_fail(cmd, "usage: scroll X Y LINES")
                           : int_arg(cmd, cmd->argv[3], &lines);
    else if (st == DBG_OK && cmd->argc > 3 && !parse_button(cmd->argv[3], &button))
        st = cmd_fail(cmd, "unknown button '%s'", cmd->argv[3]);
    if (st != DBG_OK)
        return st;

    bool press = !strcmp(verb, "down") || !strcmp(verb, "click");
    bool release = !strcmp(verb, "up") || !strcmp(verb, "click");
    bool ok = inject_pointer(cmd, PTR_MOTION, p, 0, 0);
    if (press)
        ok &= inject_pointer(cmd, PTR_PRESS, p, button, 0);
    if (release)
        ok &= inject_pointer(cmd, PTR_RELEASE, p, button, 0);
    if (scroll)
        ok &= inject_pointer(cmd, PTR_SCROLL, p, 0, lines);
    return injected(cmd, ok);
}

static enum dbg_status cmd_shortcut(struct dbg_cmd *cmd, struct json_writer *j)
{
    enum input_shortcut sc;
    if (cmd->argc < 2 || !input_parse_action(cmd->argv[1], &sc))
        return cmd_fail(cmd, "usage: shortcut ACTION (keybind action name)");
    json_str(j, cmd->argv[1]);
    return injected(cmd, dbg_inject(cmd->window, &(struct input_event){
        .kind = INPUT_SHORTCUT, .shortcut = { .action = sc },
    }));
}

static enum dbg_status cmd_font(struct dbg_cmd *cmd, struct json_writer *j)
{
    if (cmd->argc > 1) {
        const char *new_family = cmd->argv[1];
        const struct window_debug_hooks *hooks = dbg_hooks();
        if (hooks->option_set) {
            hooks->option_set(hooks->ud, "font", new_family);
        } else {
            font_set_family(new_family);
        }
    }
    json_obj_begin(j);
    json_key(j, "family");
    json_str(j, font_get_family());
    json_key(j, "size");
    json_int(j, font_configured_size());
    json_key(j, "generation");
    json_uint(j, font_generation());
    json_obj_end(j);
    return DBG_OK;
}

/* ---- dispatch ----------------------------------------------------- */

enum {
    /* Acts on a window. */
    CMD_WINDOW = 1 << 0,
    /* Waits for a frame or feeds input, so it needs the running loop. */
    CMD_LOOP = 1 << 1,
    /* Queues input; the window drains it after the reply. */
    CMD_INJECTS = 1 << 2,
};

static enum dbg_status cmd_help(struct dbg_cmd *cmd, struct json_writer *j);

static const struct {
    const char *name;
    enum dbg_status (*fn)(struct dbg_cmd *cmd, struct json_writer *j);
    unsigned flags;
    const char *usage;
} COMMANDS[] = {
    { "help", cmd_help, 0, "help" },
    { "windows", cmd_windows, 0, "windows" },
    { "state", cmd_state, CMD_WINDOW, "state" },
    { "tree", cmd_tree, CMD_WINDOW, "tree [SELECTOR] [--depth N]" },
    { "widget", cmd_widget, CMD_WINDOW, "widget SELECTOR" },
    { "hit", cmd_hit, CMD_WINDOW, "hit X Y" },
    { "scene", cmd_scene, CMD_WINDOW, "scene [SELECTOR]" },
    { "lint", cmd_lint, CMD_WINDOW, "lint" },
    { "overlay", cmd_overlay, CMD_WINDOW,
      "overlay [on|off|toggle|all|FLAG|+FLAG|-FLAG] [select SELECTOR|none]" },
    { "frame", cmd_frame, CMD_WINDOW | CMD_LOOP, "frame" },
    { "screenshot", cmd_screenshot, CMD_WINDOW | CMD_LOOP, "screenshot PATH.png" },
    { "key", cmd_key, CMD_WINDOW | CMD_LOOP | CMD_INJECTS, "key CHORD..." },
    { "type", cmd_type, CMD_WINDOW | CMD_LOOP | CMD_INJECTS, "type TEXT..." },
    { "text", cmd_text, CMD_WINDOW | CMD_LOOP | CMD_INJECTS, "text TEXT..." },
    { "move", cmd_pointer, CMD_WINDOW | CMD_LOOP | CMD_INJECTS, "move X Y" },
    { "down", cmd_pointer, CMD_WINDOW | CMD_LOOP | CMD_INJECTS,
      "down X Y [left|middle|right]" },
    { "up", cmd_pointer, CMD_WINDOW | CMD_LOOP | CMD_INJECTS,
      "up X Y [left|middle|right]" },
    { "click", cmd_pointer, CMD_WINDOW | CMD_LOOP | CMD_INJECTS,
      "click X Y [left|middle|right]" },
    { "scroll", cmd_pointer, CMD_WINDOW | CMD_LOOP | CMD_INJECTS, "scroll X Y LINES" },
    { "leave", cmd_pointer, CMD_WINDOW | CMD_LOOP | CMD_INJECTS, "leave" },
    { "shortcut", cmd_shortcut, CMD_WINDOW | CMD_LOOP | CMD_INJECTS, "shortcut ACTION" },
    { "font", cmd_font, 0, "font [FAMILY]" },
};

#define N_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static enum dbg_status cmd_help(struct dbg_cmd *cmd, struct json_writer *j)
{
    (void)cmd;
    json_obj_begin(j);
    json_key(j, "options");
    json_str(j, "-w N: window index (default primary); -p L: menu popup "
                "level L; -m MODS: modifiers for input, e.g. shift+ctrl");
    json_key(j, "commands");
    json_arr_begin(j);
    for (size_t i = 0; i < N_COMMANDS; i++)
        json_str(j, COMMANDS[i].usage);
    json_arr_end(j);
    json_obj_end(j);
    return DBG_OK;
}

/* Splits on whitespace; "double quotes" group, backslash escapes. A line
 * of n bytes holds at most n / 2 + 1 words. */
static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *r = line, *w = line;
    while (*r) {
        while (*r == ' ' || *r == '\t')
            r++;
        if (!*r)
            break;
        argv[argc++] = w;
        bool quoted = false;
        while (*r && (quoted || (*r != ' ' && *r != '\t'))) {
            if (*r == '"') {
                quoted = !quoted;
                r++;
            } else if (*r == '\\' && r[1]) {
                *w++ = r[1];
                r += 2;
            } else {
                *w++ = *r++;
            }
        }
        if (*r)
            r++;
        *w++ = '\0';
    }
    return argc;
}

/* Pulls -w/-p/-m out of argv. */
static enum dbg_status take_options(struct dbg_cmd *cmd)
{
    int out = 0, window = -1;
    for (int i = 0; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        bool has_value = i + 1 < cmd->argc;
        enum dbg_status st = DBG_OK;
        if (!strcmp(a, "-w") && has_value) {
            st = int_arg(cmd, cmd->argv[++i], &window);
        } else if (!strcmp(a, "-p") && has_value) {
            int level;
            st = int_arg(cmd, cmd->argv[++i], &level);
            if (st == DBG_OK && (level < 0 || level >= UI_MENU_MAX_LEVELS))
                st = cmd_fail(cmd, "popup level %d out of range", level);
            if (st == DBG_OK)
                cmd->surface = level + 1;
        } else if (!strcmp(a, "-m") && has_value) {
            char chord[96];
            uint32_t cp;
            uint16_t key;
            snprintf(chord, sizeof(chord), "%s+a", cmd->argv[++i]);
            if (!input_parse_chord(chord, &cmd->mods, &cp, &key))
                st = cmd_fail(cmd, "cannot parse modifiers '%s'", cmd->argv[i]);
        } else {
            cmd->argv[out++] = cmd->argv[i];
        }
        if (st != DBG_OK)
            return st;
    }
    cmd->argc = out;
    cmd->window = window < 0 ? default_window() : window_at(window);
    if (window >= 0 && !cmd->window)
        return cmd_fail(cmd, "no window %d", window);
    return DBG_OK;
}

/* Find a command in the command table.
 * Compare the first character before strcmp() to reject mismatches quickly. */
static size_t find_command(const struct dbg_cmd *cmd)
{
    size_t i = 0;
    while (i < N_COMMANDS) {
        if (COMMANDS[i].name[0] == cmd->argv[0][0] &&
            !strcmp(COMMANDS[i].name, cmd->argv[0]))
            break;
        i++;
    }
    return i;
}

static enum dbg_status run(struct dbg_cmd *cmd, char *line, struct json_writer *j,
                           unsigned *flags)
{
    cmd->argc = tokenize(line, cmd->argv);
    if (!cmd->argc)
        return cmd_fail(cmd, "empty line");
    enum dbg_status st = take_options(cmd);
    if (st != DBG_OK)
        return st;
    if (!cmd->argc)
        return cmd_fail(cmd, "missing command; try 'help'");
    size_t i = find_command(cmd);
    if (i == N_COMMANDS)
        return cmd_fail(cmd, "unknown command '%s'; try 'help'", cmd->argv[0]);
    *flags = COMMANDS[i].flags;
    if ((*flags & CMD_WINDOW) && !cmd->window)
        return cmd_fail(cmd, "no window");
    if ((*flags & CMD_WINDOW) && !dbg_surface_exists(cmd->window, cmd->surface))
        return cmd_fail(cmd, "popup level %d is not open", cmd->surface - 1);
    if ((*flags & CMD_LOOP) && cmd->client->fd < 0)
        return cmd_fail(cmd, "'%s' needs a running loop; use the socket",
                        COMMANDS[i].name);
    return COMMANDS[i].fn(cmd, j);
}

bool dbg_run_line(struct dbg_client *c, char *line)
{
    if (!line[strspn(line, " \t")])
        return true;
    struct dbg_cmd cmd = { .client = c };
    cmd.argv = malloc((strlen(line) / 2 + 1) * sizeof(*cmd.argv));
    if (!cmd.argv) {
        dbg_reply_err(c, "out of memory");
        return true;
    }
    struct json_writer j = {0};
    unsigned flags = 0;
    enum dbg_status st = run(&cmd, line, &j, &flags);
    struct window *w = cmd.window;
    free(cmd.argv);
    if (st == DBG_PENDING) {
        json_reset(&j);
        return false;
    }
    if (st == DBG_FAILED) {
        json_reset(&j);
        dbg_reply_err(c, cmd.err);
        return true;
    }
    if (j.failed) {
        json_reset(&j);
        dbg_reply_err(c, "reply too large or out of memory");
        return true;
    }
    dbg_reply_ok(c, &j);
    /* The drain may close the window; the host frees it later. */
    if (flags & CMD_INJECTS)
        window_drain_input(w);
    return true;
}
