#define _GNU_SOURCE
#include <stdlib.h>

#include "platform/platform.h"
#include "terminal/internal.h"

static bool menu_action(struct ui_widget *source, const struct ui_action *event,
                        void *ud)
{
    (void)source;
    struct window *w = ud;
    struct terminal *tw = terminal_of(w);
    struct terminal_theme_picker *picker = &tw->theme_picker;
    int action = event->id;
    if (event->kind == UI_ACTION_CLOSE) {
        if (picker->cancel)
            picker->cancel(picker->ud);
        return true;
    }
    if (event->kind == UI_ACTION_HIGHLIGHT) {
        if (action >= CTX_ACTION_THEME_BASE && picker->preview) {
            int idx = action - CTX_ACTION_THEME_BASE;
            if (picker->previewed && picker->previewed(picker->ud) == idx)
                return true;
            picker->preview(picker->ud, idx);
        } else if (picker->previewed && picker->previewed(picker->ud) >= 0) {
            if (picker->cancel)
                picker->cancel(picker->ud);
        } else {
            return false;
        }
        return true;
    }
    if (event->kind != UI_ACTION_ACTIVATE)
        return false;
    if (action >= CTX_ACTION_THEME_BASE && picker->preview && picker->commit) {
        picker->preview(picker->ud, action - CTX_ACTION_THEME_BASE);
        picker->commit(picker->ud);
        return true;
    }
    terminal_ctx_menu_perform(w, action);
    return true;
}

static const ui_menu_item *menu_on_submenu(void *ud, int action, int *n)
{
    struct window *w = ud;
    struct terminal *tw = terminal_of(w);
    struct terminal_theme_picker *picker = &tw->theme_picker;
    *n = 0;
    if (action != CTX_ACTION_THEMES || !picker->count || !picker->label)
        return NULL;
    int n_themes = picker->count(picker->ud);
    free(tw->menu_theme_items);
    tw->menu_theme_items = calloc((size_t)(n_themes ? n_themes : 1),
                                 sizeof(*tw->menu_theme_items));
    if (!tw->menu_theme_items)
        return NULL;
    for (int i = 0; i < n_themes; i++) {
        const char *label = picker->label(picker->ud, i);
        tw->menu_theme_items[(*n)++] = (ui_menu_item){
            .label = label,
            .action = CTX_ACTION_THEME_BASE + i,
            .flags = picker->selected && picker->selected(picker->ud, i)
                ? UI_LIST_ITEM_CHECKED : 0,
        };
    }
    return tw->menu_theme_items;
}

void terminal_select_all(struct window *w)
{
    struct wsap_vtcore *t = terminal_term(w);
    if (!t)
        return;
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t);
    wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ 0 }, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = size.x - 1, .y = size.y - 1 }, true);
    window_redraw(w);
}

void terminal_ctx_menu_perform(struct window *w, int action)
{
    struct terminal *tw = terminal_of(w);
    struct wsap_vtcore *t = terminal_term(w);
    switch (action) {
    case CTX_ACTION_COPY:
        if (window_surface(w) && t)
            terminal_copy(w);
        break;
    case CTX_ACTION_PASTE:
        if (window_surface(w))
            window_paste(w, YASL_CLIPBOARD);
        break;
    case CTX_ACTION_SELECT_ALL:
        terminal_select_all(w);
        break;
    case CTX_ACTION_OPEN_LINK: {
            if (!t)
                break;
            char *url = wsap_vtcore_link_at(t, term_vec_from_geom(terminal_cell_at(w, tw->ctx_click)));
            if (url) {
                platform_open_url(url);
                wsap_vtcore_text_free(url);
            }
            break;
        }
    case CTX_ACTION_RESET_VIEW:
        if (t) {
            wsap_vtcore_scroll_to(t, 0);
            window_redraw(w);
        }
        break;
    case CTX_ACTION_SETTINGS:
        window_host_command(w, INPUT_SHORTCUT_SETTINGS);
        break;
    default:
        break;
    }
}

void terminal_open_ctx_menu(struct window *w, struct vector2i at)
{
    struct terminal *tw = terminal_of(w);
    if (!window_surface(w))
        return;

    struct wsap_vtcore *t = terminal_term(w);
    char *url = t ? wsap_vtcore_link_at(t, term_vec_from_geom(terminal_cell_at(w, at))) : NULL;
    bool has_sel = t ? wsap_vtcore_has_selection(t) : false;
    bool can_reset = t ? wsap_vtcore_scrollback(t).offset > 0 : false;

    struct input_keymap *km = input_keymap_active();
    char copy_sc[64] = {0};
    char paste_sc[64] = {0};
    char sel_all_sc[64] = {0};
    if (km) {
        input_keymap_format_action_chord(km, INPUT_SHORTCUT_COPY, copy_sc, sizeof(copy_sc));
        input_keymap_format_action_chord(km, INPUT_SHORTCUT_PASTE, paste_sc, sizeof(paste_sc));
        input_keymap_format_action_chord(km, INPUT_SHORTCUT_SELECT_ALL, sel_all_sc, sizeof(sel_all_sc));
    }

    ui_menu_item items[10];
    int n = 0;
    items[n++] = UI_MENU_ITEM("Copy", CTX_ACTION_COPY,
                              .flags = has_sel ? 0 : UI_LIST_ITEM_DISABLED,
                              .shortcut = copy_sc[0] ? copy_sc : NULL);
    items[n++] = UI_MENU_ITEM("Paste", CTX_ACTION_PASTE,
                              .shortcut = paste_sc[0] ? paste_sc : NULL);
    items[n++] = UI_MENU_ITEM("Select All", CTX_ACTION_SELECT_ALL,
                              .shortcut = sel_all_sc[0] ? sel_all_sc : NULL);
    if (url) {
        items[n++] = UI_MENU_SEPARATOR;
        items[n++] = UI_MENU_ITEM("Open Link", CTX_ACTION_OPEN_LINK);
    }
    if (can_reset) {
        items[n++] = UI_MENU_SEPARATOR;
        items[n++] = UI_MENU_ITEM("Reset View", CTX_ACTION_RESET_VIEW);
    }
    items[n++] = UI_MENU_SEPARATOR;
    items[n++] = UI_MENU_ITEM("Themes", CTX_ACTION_THEMES,
                              .flags = UI_LIST_ITEM_SUBMENU);
    items[n++] = UI_MENU_ITEM("Settings...", CTX_ACTION_SETTINGS);
    wsap_vtcore_text_free(url);

    struct terminal_theme_picker *picker = &tw->theme_picker;
    if (picker->rescan)
        picker->rescan(picker->ud);
    tw->ctx_click = at;
    window_menu_open(w, items, n, at, menu_action, menu_on_submenu, w);
}
