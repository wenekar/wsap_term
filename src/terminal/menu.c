#define _GNU_SOURCE
#include <stdlib.h>

#include "platform/platform.h"
#include "terminal/internal.h"

static bool menu_action(struct ui_widget *source, const struct ui_action *event,
                        void *ud)
{
    (void)source;
    struct terminal *t = ud;
    struct terminal_theme_picker *picker = &t->theme_picker;
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
    terminal_ctx_menu_perform(t, action);
    return true;
}

static const ui_menu_item *menu_on_submenu(void *ud, int action, int *n)
{
    struct terminal *t = ud;
    struct terminal_theme_picker *picker = &t->theme_picker;
    *n = 0;
    if (action != CTX_ACTION_THEMES || !picker->count || !picker->label)
        return NULL;
    int n_themes = picker->count(picker->ud);
    free(t->menu_theme_items);
    t->menu_theme_items = calloc((size_t)(n_themes ? n_themes : 1),
                                 sizeof(*t->menu_theme_items));
    if (!t->menu_theme_items)
        return NULL;
    for (int i = 0; i < n_themes; i++) {
        const char *label = picker->label(picker->ud, i);
        t->menu_theme_items[(*n)++] = (ui_menu_item){
            .label = label,
            .action = CTX_ACTION_THEME_BASE + i,
            .flags = picker->selected && picker->selected(picker->ud, i)
                ? UI_LIST_ITEM_CHECKED : 0,
        };
    }
    return t->menu_theme_items;
}

void terminal_select_all(struct terminal *t)
{
    if (!t || !t->session.term)
        return;
    struct wsap_vtcore *term = t->session.term;
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(term);
    wsap_vtcore_select_start(term, (struct wsap_vtcore_vector2i){ 0 }, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_select_extend(term, (struct wsap_vtcore_vector2i){ .x = size.x - 1, .y = size.y - 1 }, true);
    if (t->window)
        window_redraw(t->window);
}

void terminal_ctx_menu_perform(struct terminal *t, int action)
{
    if (!t)
        return;
    struct window *w = t->window;
    struct wsap_vtcore *term = t->session.term;
    switch (action) {
    case CTX_ACTION_COPY:
        if (w && window_surface(w) && term)
            terminal_copy(t);
        break;
    case CTX_ACTION_PASTE:
        if (w && window_surface(w))
            window_paste(w, YASL_CLIPBOARD);
        break;
    case CTX_ACTION_SELECT_ALL:
        terminal_select_all(t);
        break;
    case CTX_ACTION_OPEN_LINK: {
            if (!term)
                break;
            char *url = wsap_vtcore_link_at(term, term_vec_from_geom(
                terminal_cell_at(t, t->ctx_click)));
            if (url) {
                platform_open_url(url);
                wsap_vtcore_text_free(url);
            }
            break;
        }
    case CTX_ACTION_RESET_VIEW:
        if (term) {
            wsap_vtcore_scroll_to(term, 0);
            if (w)
                window_redraw(w);
        }
        break;
    case CTX_ACTION_SETTINGS:
        if (w)
            window_host_command(w, INPUT_SHORTCUT_SETTINGS);
        break;
    default:
        break;
    }
}

void terminal_open_ctx_menu(struct terminal *t, struct vector2i at)
{
    struct window *w = t ? t->window : NULL;
    if (!w || !window_surface(w))
        return;

    struct wsap_vtcore *term = t->session.term;
    char *url = term ? wsap_vtcore_link_at(term, term_vec_from_geom(
        terminal_cell_at(t, at))) : NULL;
    bool has_sel = term ? wsap_vtcore_has_selection(term) : false;
    bool can_reset = term ? wsap_vtcore_scrollback(term).offset > 0 : false;

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

    struct terminal_theme_picker *picker = &t->theme_picker;
    if (picker->rescan)
        picker->rescan(picker->ud);
    t->ctx_click = at;
    window_menu_open(w, items, n, at, menu_action, menu_on_submenu, t);
}
