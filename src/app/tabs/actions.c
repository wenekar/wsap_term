#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

#include "app/tabs/internal.h"
#include "input/keybind.h"
#include "ui/surface.h"

enum palette_action {
    PALETTE_ACT_SCROLL_BOTTOM = INPUT_COMMAND_APP_COUNT,
    PALETTE_ACT_NEW_WINDOW,
    PALETTE_ACT_TAB_BASE = INPUT_COMMAND_APP_COUNT + 16,
};

static void fmt_shortcut(const struct window *w, input_command_id sc,
                         char *out, size_t sz)
{
    out[0] = '\0';
    const struct input_keymap *km = window_keymap(w);
    if (km)
        input_keymap_format_action_chord(km, sc, out, sz);
}

static int add_cmd(struct ui_palette_cmd *cmds, char (*sc)[64], int n,
                   const struct window *w, const char *label, int action,
                   input_command_id sc_of)
{
    fmt_shortcut(w, sc_of, sc[n], sizeof(sc[n]));
    cmds[n] = (struct ui_palette_cmd){
        .label = label,
        .shortcut = sc[n][0] ? sc[n] : NULL,
        .action = action,
    };
    return n + 1;
}

static void palette_toggle(struct app_tabs *tabs)
{
    struct window *w = tabs->window;
    if (!w || window_closed(w))
        return;
    if (ui_palette_is_open(&tabs->palette)) {
        ui_palette_close(&tabs->palette);
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

    n = add_cmd(cmds, sc, n, w, "New Tab", INPUT_COMMAND_NEW_TAB,
                INPUT_COMMAND_NEW_TAB);
    n = add_cmd(cmds, sc, n, w, "Close Tab", INPUT_COMMAND_CLOSE_TAB,
                INPUT_COMMAND_CLOSE_TAB);
    n = add_cmd(cmds, sc, n, w, "Next Tab", INPUT_COMMAND_NEXT_TAB,
                INPUT_COMMAND_NEXT_TAB);
    n = add_cmd(cmds, sc, n, w, "Previous Tab", INPUT_COMMAND_PREV_TAB,
                INPUT_COMMAND_PREV_TAB);
    n = add_cmd(cmds, sc, n, w, "Last Tab", INPUT_COMMAND_LAST_TAB,
                INPUT_COMMAND_LAST_TAB);

    int listed = 0;
    for (int i = 0; i < tabs->count && listed < TAB_CMDS; i++) {
        const struct app_tab *entry = app_tabs_entry(tabs, i);
        if (!entry || !entry->content)
            continue;
        if (asprintf(&tab_labels[listed], "Switch to Tab %d: %s", listed + 1,
                     app_tab_title(entry)) < 0) {
            tab_labels[listed] = NULL;
            continue;
        }
        n = add_cmd(cmds, sc, n, w, tab_labels[listed],
                    PALETTE_ACT_TAB_BASE + i, INPUT_COMMAND_NONE);
        listed++;
    }

    n = add_cmd(cmds, sc, n, w, "Copy", INPUT_COMMAND_COPY,
                INPUT_COMMAND_COPY);
    n = add_cmd(cmds, sc, n, w, "Paste", INPUT_COMMAND_PASTE,
                INPUT_COMMAND_PASTE);
    n = add_cmd(cmds, sc, n, w, "Select All", INPUT_COMMAND_SELECT_ALL,
                INPUT_COMMAND_SELECT_ALL);
    n = add_cmd(cmds, sc, n, w, "Zoom In", INPUT_COMMAND_FONT_LARGER,
                INPUT_COMMAND_FONT_LARGER);
    n = add_cmd(cmds, sc, n, w, "Zoom Out", INPUT_COMMAND_FONT_SMALLER,
                INPUT_COMMAND_FONT_SMALLER);
    n = add_cmd(cmds, sc, n, w, "Scroll to Bottom", PALETTE_ACT_SCROLL_BOTTOM,
                INPUT_COMMAND_NONE);
    n = add_cmd(cmds, sc, n, w, "New Window", PALETTE_ACT_NEW_WINDOW,
                INPUT_COMMAND_NONE);
    n = add_cmd(cmds, sc, n, w, "Settings", INPUT_COMMAND_SETTINGS,
                INPUT_COMMAND_SETTINGS);

    ui_palette_open(&tabs->palette, cmds, n);
    for (int i = 0; i < TAB_CMDS; i++)
        free(tab_labels[i]);
}

static bool palette_action(struct ui_widget *source,
                           const struct ui_action *action, void *ud)
{
    (void)source;
    struct app_tabs *tabs = ud;
    struct window *w = tabs ? tabs->window : NULL;
    if (!w || window_closed(w))
        return false;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    int id = action->id;
    if (id >= PALETTE_ACT_TAB_BASE) {
        app_tabs_select(w, id - PALETTE_ACT_TAB_BASE);
        return true;
    }
    if (id >= 0 && id < INPUT_COMMAND_APP_COUNT) {
        app_tabs_shortcut(w, (input_command_id)id);
        return true;
    }
    switch (id) {
    case PALETTE_ACT_SCROLL_BOTTOM: {
        const struct app_tab *entry = app_tabs_entry(tabs, tabs->active);
        if (entry && entry->content && entry->ops->scroll_bottom) {
            entry->ops->scroll_bottom(entry->content);
            if (window_surface(w))
                window_redraw(w);
        }
        break;
    }
    case PALETTE_ACT_NEW_WINDOW:
        (void)app_tabs_new_window(w);
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
        return event->data.shortcut.action != INPUT_COMMAND_COMMAND_PALETTE;
    default:
        return false;
    }
}

static void tab_moved(struct app_tabs *tabs, const struct ui_action *action)
{
    struct window *w = tabs->window;
    ui_tab_preview_suppress(&tabs->preview);
    if (action->arg >= 0) {
        app_tabs_reorder(w, action->index, action->arg);
        return;
    }
    struct window *next = app_tabs_detach(w, action->index);
    if (next && window_surface(next))
        window_place_under_pointer(next,
            window_logical(w, (struct vector2i){ .x = action->pos.x }));
}

static void handle_action(struct app_tabs *tabs, const struct ui_action *action)
{
    struct window *w = tabs->window;
    /* The switch holds core kinds and the tab kinds, which start at
     * UI_ACTION_USER. */
    switch ((int)action->kind) {
    case UI_ACTION_ACTIVATE:
        ui_tab_preview_suppress(&tabs->preview);
        if (action->id == UI_TABBAR_NEW_TAB)
            (void)app_tabs_new(w);
        else
            app_tabs_select(w, action->index);
        break;
    case UI_ACTION_CLOSE:
        ui_tab_preview_suppress(&tabs->preview);
        app_tabs_close(w, action->index);
        break;
    case UI_ACTION_HIGHLIGHT:
        app_tabs_preview_hover(tabs, action->index);
        break;
    case UI_TAB_ACTION_DRAG:
        app_tabs_preview_drag(tabs, action);
        break;
    case UI_TAB_ACTION_TEAR_OFF:
        (void)app_tabs_drag_begin(w, action->index, action->pos);
        break;
    case UI_TAB_ACTION_MOVE:
        tab_moved(tabs, action);
        break;
    default:
        break;
    }
}

static bool tabbar_action(struct ui_widget *source, const struct ui_action *action,
                          void *userdata)
{
    (void)source;
    struct app_tabs *tabs = userdata;
    if (!tabs)
        return false;
    handle_action(tabs, action);
    return true;
}

void app_tabs_actions_init(struct app_tabs *tabs)
{
    ui_widget_set_action_handler(ui_tabbar_widget(&tabs->tabbar),
                                 tabbar_action, tabs);
    ui_widget_set_action_handler(ui_palette_widget(&tabs->palette),
                                 palette_action, tabs);
    ui_widget_set_event_handler(ui_palette_widget(&tabs->palette),
                                palette_swallow, NULL);
    app_tabs_preview_init(tabs);
}

static int next_live(const struct app_tabs *tabs, int from)
{
    for (int step = 1; step < tabs->count; step++)
        if (app_tab_is_live(app_tabs_entry(tabs, (from + step) % tabs->count)))
            return (from + step) % tabs->count;
    return from;
}

static int previous_live(const struct app_tabs *tabs, int from)
{
    for (int step = 1; step < tabs->count; step++)
        if (app_tab_is_live(app_tabs_entry(
                tabs, (from - step + tabs->count) % tabs->count)))
            return (from - step + tabs->count) % tabs->count;
    return from;
}

static int last_live(const struct app_tabs *tabs)
{
    for (int i = tabs->count - 1; i >= 0; i--)
        if (app_tab_is_live(app_tabs_entry(tabs, i)))
            return i;
    return -1;
}

void app_tabs_shortcut(struct window *w, input_command_id sc)
{
    struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs)
        return;
    int live = app_tabs_live_count(tabs);
    switch (sc) {
    case INPUT_COMMAND_NEW_TAB:
        (void)app_tabs_new(w);
        return;
    case INPUT_COMMAND_CLOSE_TAB:
        app_tabs_close(w, tabs->active);
        return;
    case INPUT_COMMAND_NEXT_TAB:
        if (live > 1)
            app_tabs_select(w, next_live(tabs, tabs->active));
        return;
    case INPUT_COMMAND_PREV_TAB:
        if (live > 1)
            app_tabs_select(w, previous_live(tabs, tabs->active));
        return;
    case INPUT_COMMAND_LAST_TAB:
        app_tabs_select(w, last_live(tabs));
        return;
    case INPUT_COMMAND_TAB_1:
    case INPUT_COMMAND_TAB_2:
    case INPUT_COMMAND_TAB_3:
    case INPUT_COMMAND_TAB_4:
    case INPUT_COMMAND_TAB_5:
    case INPUT_COMMAND_TAB_6:
    case INPUT_COMMAND_TAB_7:
    case INPUT_COMMAND_TAB_8:
        app_tabs_select(w, (int)sc - INPUT_COMMAND_TAB_1);
        return;
    case INPUT_COMMAND_COMMAND_PALETTE:
        palette_toggle(tabs);
        return;
    case INPUT_COMMAND_FOCUS_TABS: {
        if (live > 1) {
            struct ui_widget *bar = ui_tabbar_widget(&tabs->tabbar);
            struct ui_surface *surface = window_ui_surface(w);
            ui_surface_focus(surface,
                ui_surface_focused(surface) == bar ? NULL : bar);
        }
        return;
    }
    default:
        break;
    }
    const struct app_tab *entry = app_tabs_entry(tabs, tabs->active);
    if (entry && entry->content && entry->ops->shortcut &&
        entry->ops->shortcut(entry->content, sc))
        app_tabs_relayout(w);
}
