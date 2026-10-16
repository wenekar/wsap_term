#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terminal/internal.h"
#include "input/keybind.h"
#include "session/session.h"

enum palette_action {
    PALETTE_ACT_SCROLL_BOTTOM = INPUT_SHORTCUT_COUNT,
    PALETTE_ACT_NEW_WINDOW,
};

static void fmt_shortcut(enum input_shortcut sc, char *out, size_t sz)
{
    out[0] = '\0';
    struct input_keymap *km = input_keymap_active();
    if (km)
        input_keymap_format_action_chord(km, sc, out, sz);
}

static int add_cmd(struct ui_palette_cmd *cmds, char (*sc)[64], int n,
                   const char *label, int action, enum input_shortcut sc_of)
{
    fmt_shortcut(sc_of, sc[n], sizeof(sc[n]));
    cmds[n] = (struct ui_palette_cmd){
        .label = label,
        .shortcut = sc[n][0] ? sc[n] : NULL,
        .action = action,
    };
    return n + 1;
}

static bool palette_action(struct ui_widget *source,
                           const struct ui_action *action, void *ud)
{
    (void)source;
    struct window *w = ud;
    if (!w || window_closed(w))
        return false;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    int id = action->id;
    if (id >= 0 && id < INPUT_SHORTCUT_COUNT) {
        terminal_handle_shortcut(w, (enum input_shortcut)id);
        return true;
    }
    switch (id) {
    case PALETTE_ACT_SCROLL_BOTTOM: {
        struct wsap_vtcore *t = terminal_term(w);
        if (t) {
            wsap_vtcore_scroll_to(t, 0);
            if (window_surface(w))
                window_redraw(w);
        }
        break;
    }
    case PALETTE_ACT_NEW_WINDOW:
        (void)terminal_new_window(w);
        break;
    default:
        break;
    }
    return true;
}

static bool palette_swallow(struct ui_widget *widget,
                            const struct ui_event *event, void *userdata)
{
    (void)widget;
    (void)userdata;
    switch (event->kind) {
    case UI_EVENT_PASTE:
    case UI_EVENT_DROP_FILES:
        return true;
    case UI_EVENT_SHORTCUT:
        return event->data.shortcut.action != INPUT_SHORTCUT_COMMAND_PALETTE;
    default:
        return false;
    }
}

void terminal_palette_init(struct window *w)
{
    struct ui_widget *palette = ui_palette_widget(&terminal_of(w)->palette);
    ui_widget_set_action_handler(palette, palette_action, w);
    ui_widget_set_event_handler(palette, palette_swallow, NULL);
}

void terminal_palette_toggle(struct window *w)
{
    struct terminal *tw = terminal_of(w);
    if (!w || window_closed(w))
        return;
    if (ui_palette_is_open(&tw->palette)) {
        ui_palette_close(&tw->palette);
        return;
    }
    if (!window_surface(w))
        return;
    window_menu_close(w);

    enum { TAB_CMDS = 8, MAX_CMDS = 5 + TAB_CMDS + 8 };
    struct ui_palette_cmd cmds[MAX_CMDS];
    char sc[MAX_CMDS][64];
    char *tab_labels[TAB_CMDS] = {0};
    int n = 0;

    n = add_cmd(cmds, sc, n, "New Tab", INPUT_SHORTCUT_NEW_TAB,
                INPUT_SHORTCUT_NEW_TAB);
    n = add_cmd(cmds, sc, n, "Close Tab", INPUT_SHORTCUT_CLOSE_TAB,
                INPUT_SHORTCUT_CLOSE_TAB);
    n = add_cmd(cmds, sc, n, "Next Tab", INPUT_SHORTCUT_NEXT_TAB,
                INPUT_SHORTCUT_NEXT_TAB);
    n = add_cmd(cmds, sc, n, "Previous Tab", INPUT_SHORTCUT_PREV_TAB,
                INPUT_SHORTCUT_PREV_TAB);
    n = add_cmd(cmds, sc, n, "Last Tab", INPUT_SHORTCUT_LAST_TAB,
                INPUT_SHORTCUT_LAST_TAB);

    for (int i = 0; i < tw->tab_count && i < TAB_CMDS; i++) {
        if (asprintf(&tab_labels[i], "Switch to Tab %d: %s", i + 1,
                     session_title(&tw->tabs[i]->session)) < 0) {
            tab_labels[i] = NULL;
            continue;
        }
        n = add_cmd(cmds, sc, n, tab_labels[i], INPUT_SHORTCUT_TAB_1 + i,
                    (enum input_shortcut)(INPUT_SHORTCUT_TAB_1 + i));
    }

    n = add_cmd(cmds, sc, n, "Copy", INPUT_SHORTCUT_COPY,
                INPUT_SHORTCUT_COPY);
    n = add_cmd(cmds, sc, n, "Paste", INPUT_SHORTCUT_PASTE,
                INPUT_SHORTCUT_PASTE);
    n = add_cmd(cmds, sc, n, "Select All", INPUT_SHORTCUT_SELECT_ALL,
                INPUT_SHORTCUT_SELECT_ALL);
    n = add_cmd(cmds, sc, n, "Zoom In", INPUT_SHORTCUT_FONT_LARGER,
                INPUT_SHORTCUT_FONT_LARGER);
    n = add_cmd(cmds, sc, n, "Zoom Out", INPUT_SHORTCUT_FONT_SMALLER,
                INPUT_SHORTCUT_FONT_SMALLER);
    n = add_cmd(cmds, sc, n, "Scroll to Bottom", PALETTE_ACT_SCROLL_BOTTOM,
                INPUT_SHORTCUT_NONE);
    n = add_cmd(cmds, sc, n, "New Window", PALETTE_ACT_NEW_WINDOW,
                INPUT_SHORTCUT_NONE);
    n = add_cmd(cmds, sc, n, "Settings", INPUT_SHORTCUT_SETTINGS,
                INPUT_SHORTCUT_SETTINGS);

    ui_palette_open(&tw->palette, cmds, n);
    for (int i = 0; i < TAB_CMDS; i++)
        free(tab_labels[i]);
}
