#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "terminal/internal.h"
#include "util/json.h"
#include "util/time.h"

const int terminal_kind;

static const char *op_title(struct window *w)
{
    struct session *s = terminal_active_session(w);
    return s ? session_title(s) : NULL;
}

/* Shows the active tab's title on the window. */
static void update_title(struct window *w)
{
    window_set_title(w, op_title(w));
}

static void tab_output(void *ud)
{
    struct tab *t = ud;
    struct window *w = t->window;
    if (!w || !window_surface(w) || terminal_active_tab(w) != t)
        return;
    window_redraw(w);
}

static void tab_title(void *ud)
{
    struct tab *t = ud;
    struct window *w = t->window;
    if (!w || !window_surface(w))
        return;
    if (terminal_active_tab(w) == t)
        update_title(w);
    window_redraw(w);
}

static void tab_clipboard(void *ud, const char *text)
{
    struct tab *t = ud;
    if (t->window && window_surface(t->window))
        window_copy(t->window, YASL_CLIPBOARD, text);
}

static void tab_exited(void *ud)
{
    struct tab *t = ud;
    struct window *w = t->window;
    t->exited = true;
    if (!w || window_closed(w))
        return;
    struct terminal *tw = terminal_of(w);
    for (int i = 0; i < tw->tab_count; i++) {
        if (tw->tabs[i] == t) {
            terminal_close_tab(w, i);
            return;
        }
    }
}

static void tab_bell(void *ud)
{
    struct tab *t = ud;
    if (!t || !t->window)
        return;
    struct window *w = t->window;
    struct terminal *tw = terminal_of(w);
    /* The view's tick ends the flash: its deadline wakes the next frame. */
    if (tw && tw->config->visual_bell) {
        tw->view.visual_bell_until_ms = yasl_time_ns() / 1000000 + 100;
        window_redraw(w);
    }
}

static const struct session_host TAB_SESSION_HOST = {
    .output = tab_output,
    .title = tab_title,
    .clipboard = tab_clipboard,
    .exited = tab_exited,
    .bell = tab_bell,
};

struct tab *tab_create(struct yasl_loop *loop, const struct session_spec *spec,
                       int font_size)
{
    struct tab *t = aligned_alloc(64, (sizeof(*t) + 63) & ~(size_t)63);
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));
    t->font_size = font_size;
    if (!session_init(&t->session, loop, spec)) {
        free(t);
        return NULL;
    }
    session_set_host(&t->session, &TAB_SESSION_HOST, t);
    return t;
}

void tab_destroy(struct tab *t)
{
    if (!t)
        return;
    session_fini(&t->session);
    font_instance_unref(t->font);
    free(t);
}

static struct vector2i terminal_size(const struct window *w)
{
    return window_surface(w) ? window_size(w) : window_default_size(window_host(w)->config);
}

/* The grid a tab's font fits in the window, under the tab bar. */
static struct vector2i session_grid(const struct window *w, const struct tab *t,
                                    struct vector2i px)
{
    struct terminal *tw = terminal_of(w);
    int content_h = px.y - ui_tabbar_steady_height(&tw->tabbar);
    const struct font_metrics *metrics = tab_metrics(t);
    struct vector2i content = { .x = px.x, .y = content_h < 1 ? 1 : content_h };
    struct vector2i grid = {
        .x = content.x > 0 ? content.x / metrics->cell_w : 0,
        .y = content.y > 0 ? content.y / metrics->cell_h : 0,
    };
    if (grid.x > WSAP_VTCORE_MAX_COLS)
        grid.x = WSAP_VTCORE_MAX_COLS;
    return vector2i_max(grid, vector2i_splat(1));
}

/* Gives t its size at w's scale. False when the font cannot be had; t
 * keeps the font it has. */
static bool tab_fit_font(struct tab *t, const struct window *w, int size_pt)
{
    uint32_t scale = window_scale_120(w);
    if (t->font && font_instance_size_pt(t->font) == size_pt &&
        font_instance_scale_120(t->font) == scale)
        return true;
    struct font_instance *fi = font_instance_acquire(size_pt, scale);
    if (!fi)
        return false;
    font_instance_unref(t->font);
    t->font = fi;
    t->font_size = font_instance_size_pt(fi);
    return true;
}

static void refresh_layout(struct window *w)
{
    if (!window_surface(w))
        return;
    terminal_resize_sessions(w, window_size(w));
    update_title(w);
    window_redraw(w);
}

/* The scrollbar shows the active tab; another tab starts with it hidden. */
static void deactivate_scrollbar(struct window *w)
{
    ui_scrollview_hide(&terminal_of(w)->scrollview);
}

int terminal_add_tab(struct window *w, struct tab *t, int slot)
{
    struct terminal *tw = terminal_of(w);
    if (tw->tab_count == tw->tab_cap) {
        int cap = tw->tab_cap ? tw->tab_cap * 2 : 4;
        struct tab **tabs = realloc(tw->tabs, (size_t)cap * sizeof(*tabs));
        if (!tabs)
            return -1;
        tw->tabs = tabs;
        tw->tab_cap = cap;
    }
    int old_count = tw->tab_count;
    if (slot < 0 || slot > old_count)
        slot = old_count;
    memmove(&tw->tabs[slot + 1], &tw->tabs[slot],
            (size_t)(old_count - slot) * sizeof(*tw->tabs));
    tw->tabs[slot] = t;
    tw->tab_count++;
    ui_tabbar_insert_tab(&tw->tabbar, slot);
    if (old_count && tw->active_tab >= slot)
        tw->active_tab++;
    else if (tw->active_tab < 0)
        tw->active_tab = slot;
    t->window = w;
    tab_fit_font(t, w, t->font_size);
    return slot;
}

struct tab *terminal_remove_tab(struct window *w, int index)
{
    struct terminal *tw = terminal_of(w);
    if (index < 0 || index >= tw->tab_count)
        return NULL;
    struct tab *t = tw->tabs[index];
    memmove(&tw->tabs[index], &tw->tabs[index + 1],
            (size_t)(tw->tab_count - index - 1) * sizeof(*tw->tabs));
    tw->tabs[--tw->tab_count] = NULL;
    ui_tabbar_remove_tab(&tw->tabbar, index);
    if (tw->active_tab > index)
        tw->active_tab--;
    if (tw->active_tab >= tw->tab_count)
        tw->active_tab = tw->tab_count - 1;
    t->window = NULL;
    return t;
}

bool terminal_is(const struct window *w)
{
    return terminal_of(w) != NULL;
}

void terminal_set_theme(struct window *w, const struct wsap_vtcore_theme *theme)
{
    struct terminal *tw = terminal_of(w);
    if (!tw || !theme)
        return;
    tw->theme = *theme;
    for (int i = 0; i < tw->tab_count; i++)
        wsap_vtcore_set_theme(tw->tabs[i]->session.term, &tw->theme);
    ui_tabbar_set_bg_color(&tw->tabbar, tw->theme.bg);
    window_redraw(w);
}

struct session *terminal_active_session(const struct window *w)
{
    struct tab *t = terminal_active_tab(w);
    return t ? &t->session : NULL;
}

int terminal_tab_count(const struct window *w)
{
    const struct terminal *tw = terminal_of(w);
    return tw ? tw->tab_count : 0;
}

int terminal_active_index(const struct window *w)
{
    const struct terminal *tw = terminal_of(w);
    return tw ? tw->active_tab : -1;
}

int terminal_tabbar_height(const struct window *w)
{
    struct terminal *tw = terminal_of(w);
    return tw ? ui_tabbar_height(&tw->tabbar) : 0;
}

void terminal_resize_sessions(struct window *w, struct vector2i px)
{
    struct terminal *tw = terminal_of(w);
    for (int i = 0; i < tw->tab_count; i++) {
        struct vector2i grid = session_grid(w, tw->tabs[i], px);
        if (i == tw->active_tab) {
            tw->tabs[i]->pending_grid = (struct vector2i){0};
            session_resize(&tw->tabs[i]->session, grid);
        } else {
            tw->tabs[i]->pending_grid = grid;
        }
    }
}

static bool new_tab_in(struct window *w, const char *cwd)
{
    struct terminal *tw = terminal_of(w);
    const struct terminal_config *config = tw->config;
    struct vector2i size = terminal_size(w);
    struct tab *cur = terminal_active_tab(w);
    /* A new tab starts at the size of the tab it opens next to. */
    int font_size = cur ? cur->font_size : window_host(w)->config->font_size;
    struct tab probe = { .font_size = font_size };
    tab_fit_font(&probe, w, font_size);
    const struct session_spec spec = {
        .command = config->command,
        .cwd = cwd,
        .grid = session_grid(w, &probe, size),
        .scrollback = config->scrollback,
        .theme = tw->theme,
        .cursor_style = config->cursor_style,
        .cursor_blink = config->cursor_blink,
    };
    font_instance_unref(probe.font);
    struct tab *t = tab_create(window_host(w)->loop, &spec, font_size);
    if (!t)
        return false;
    int slot = terminal_add_tab(w, t, -1);
    if (slot < 0) {
        tab_destroy(t);
        return false;
    }
    deactivate_scrollbar(w);
    tw->active_tab = slot;
    window_ui_restyle(w);
    terminal_resize_sessions(w, size);
    update_title(w);
    if (tw->tab_count > 1)
        ui_tabbar_begin_open(&tw->tabbar, slot, window_frame_time_ms(w));
    window_redraw(w);
    return true;
}

bool terminal_new_tab(struct window *w)
{
    char cwd[4096];
    struct session *cur = terminal_active_session(w);
    return new_tab_in(w, cur && session_cwd(cur, cwd, sizeof(cwd)) ? cwd : NULL);
}

static void finish_close_tab(struct window *w, int index)
{
    struct terminal *tw = terminal_of(w);
    struct tab *cur = terminal_active_tab(w);
    struct tab *t = terminal_remove_tab(w, index);
    if (t == cur)
        deactivate_scrollbar(w);
    tab_destroy(t);
    if (!tw->tab_count) {
        window_close(w);
        return;
    }
    refresh_layout(w);
    window_repaint_all(w);
    window_redraw(w);
}

void terminal_close_tab(struct window *w, int index)
{
    struct terminal *tw = terminal_of(w);
    if (index < 0 || index >= tw->tab_count)
        return;
    uint64_t now_ms = window_frame_time_ms(w);
    if (tw->tab_count > 1 && ui_tabbar_begin_close(&tw->tabbar, index, now_ms)) {
        if (index == tw->active_tab) {
            int next = index + 1 < tw->tab_count ? index + 1 : index - 1;
            if (ui_tabbar_is_closing(&tw->tabbar, next)) {
                int alt = next == index + 1 ? index - 1 : index + 1;
                if (alt >= 0 && alt < tw->tab_count &&
                    !ui_tabbar_is_closing(&tw->tabbar, alt))
                    next = alt;
            }
            terminal_select_tab(w, next);
        }
        return;
    }
    finish_close_tab(w, index);
}

void terminal_finish_closing_tab(struct window *w)
{
    struct terminal *tw = terminal_of(w);
    if (window_closed(w))
        return;
    int idx = ui_tabbar_closing_tab(&tw->tabbar);
    if (idx >= 0 && ui_tabbar_closing_done(&tw->tabbar))
        finish_close_tab(w, idx);
}

void terminal_select_tab(struct window *w, int index)
{
    struct terminal *tw = terminal_of(w);
    if (index < 0 || index >= tw->tab_count || index == tw->active_tab)
        return;
    deactivate_scrollbar(w);
    tw->active_tab = index;
    if (tw->tabs[index]->pending_grid.x > 0 && tw->tabs[index]->pending_grid.y > 0) {
        session_resize(&tw->tabs[index]->session, tw->tabs[index]->pending_grid);
        tw->tabs[index]->pending_grid = (struct vector2i){0};
    }
    update_title(w);
    window_repaint_all(w);
    window_redraw(w);
}

void terminal_reorder_tab(struct window *w, int from, int to)
{
    struct terminal *tw = terminal_of(w);
    if (from == to || from < 0 || to < 0 ||
        from >= tw->tab_count || to >= tw->tab_count)
        return;
    struct tab *t = tw->tabs[from];
    if (from < to)
        memmove(&tw->tabs[from], &tw->tabs[from + 1],
                (size_t)(to - from) * sizeof(*tw->tabs));
    else
        memmove(&tw->tabs[to + 1], &tw->tabs[to],
                (size_t)(from - to) * sizeof(*tw->tabs));
    tw->tabs[to] = t;
    ui_tabbar_move_tab(&tw->tabbar, from, to);

    if (tw->active_tab == from)
        tw->active_tab = to;
    else if (from < to && tw->active_tab > from && tw->active_tab <= to)
        tw->active_tab--;
    else if (from > to && tw->active_tab >= to && tw->active_tab < from)
        tw->active_tab++;
    update_title(w);
    window_redraw(w);
}

bool terminal_attach(struct window *w, struct tab *t, int slot)
{
    struct terminal *tw = terminal_of(w);
    if (!t || t->window)
        return false;
    slot = terminal_add_tab(w, t, slot);
    if (slot < 0)
        return false;
    deactivate_scrollbar(w);
    tw->active_tab = slot;
    window_ui_restyle(w);
    if (tw->tab_count > 1)
        ui_tabbar_begin_open(&tw->tabbar, slot, window_frame_time_ms(w));
    refresh_layout(w);
    return true;
}

struct tab *terminal_take_tab(struct window *w, int index)
{
    struct tab *cur = terminal_active_tab(w);
    struct tab *t = terminal_remove_tab(w, index);
    if (!t)
        return NULL;
    if (t == cur)
        deactivate_scrollbar(w);
    if (terminal_of(w)->tab_count == 0) {
        window_close(w);
        return t;
    }
    refresh_layout(w);
    window_redraw(w);
    return t;
}

struct window *terminal_detach_tab(struct window *w, int index)
{
    struct terminal *tw = terminal_of(w);
    if (index < 0 || index >= tw->tab_count || tw->tab_count <= 1)
        return NULL;
    struct tab *t = terminal_take_tab(w, index);
    struct window *nw = tw->open_window
        ? tw->open_window(tw->ud, t, NULL, terminal_size(w)) : NULL;
    if (!nw)
        terminal_attach(w, t, index);
    return nw;
}

struct window *terminal_new_window(struct window *from)
{
    struct terminal *tw = terminal_of(from);
    char cwd[4096];
    struct session *cur = terminal_active_session(from);
    bool has_cwd = cur && session_cwd(cur, cwd, sizeof(cwd));
    return tw->open_window
        ? tw->open_window(tw->ud, NULL, has_cwd ? cwd : NULL, terminal_size(from))
        : NULL;
}

/* Zoom is per tab: only the active tab changes size. */
void terminal_adjust_font_size(struct window *w, int delta_pt)
{
    struct tab *t = terminal_active_tab(w);
    if (!t || !tab_fit_font(t, w, t->font_size + delta_pt))
        return;
    if (window_surface(w))
        session_resize(&t->session, session_grid(w, t, window_size(w)));
    window_repaint_all(w);
    window_redraw(w);
}

/* ---- content ops -------------------------------------------------- */

static void op_resize(struct window *w, struct vector2i size)
{
    terminal_resize_sessions(w, size);
}

/* The blink resumes from the next frame's deadline. */
static void op_suspended(struct window *w, bool suspended)
{
    (void)suspended;
    window_redraw(w);
}

static bool same_text(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

/* The scale or config changed. Re-reads what the terminal depends on. */
static void op_restyle(struct window *w)
{
    struct terminal *tw = terminal_of(w);
    const struct window_config *wcfg = window_host(w)->config;
    const struct terminal_config *cfg = tw->config;
    bool family_changed = !same_text(tw->applied_font_family, wcfg->font_family);
    bool size_changed = tw->applied_font_size != wcfg->font_size;
    bool font_changed = family_changed || size_changed;
    bool cursor_changed = tw->applied_cursor_style != cfg->cursor_style ||
        tw->applied_cursor_blink != cfg->cursor_blink;

    for (int i = 0; i < tw->tab_count; i++) {
        struct tab *t = tw->tabs[i];
        int size = t->font_size + (size_changed ? wcfg->font_size - tw->applied_font_size : 0);
        if (family_changed) {
            font_instance_unref(t->font);
            t->font = NULL;
        }
        tab_fit_font(t, w, size);
        if (cursor_changed)
            wsap_vtcore_set_cursor_default(t->session.term, cfg->cursor_style, !cfg->cursor_blink);
    }
    if (family_changed) {
        /* On a failed copy the old name stays, so the next change tries again. */
        char *family = wcfg->font_family ? strdup(wcfg->font_family) : NULL;
        if (family || !wcfg->font_family) {
            free(tw->applied_font_family);
            tw->applied_font_family = family;
        }
    }
    tw->applied_font_size = wcfg->font_size;
    tw->applied_cursor_style = cfg->cursor_style;
    tw->applied_cursor_blink = cfg->cursor_blink;
    if (window_surface(w))
        terminal_resize_sessions(w, window_size(w));
    if (font_changed)
        window_repaint_all(w);
    /* A blink setting change takes effect from the next frame's deadline. */
    window_redraw(w);
}

static void op_debug_state(struct window *w, struct json_writer *j)
{
    struct terminal *tw = terminal_of(w);
    json_key(j, "tabs"); json_int(j, tw->tab_count);
    json_key(j, "active_tab"); json_int(j, tw->active_tab);
    json_key(j, "tabbar_height"); json_int(j, terminal_tabbar_height(w));
    json_key(j, "tab_titles");
    json_arr_begin(j);
    for (int i = 0; i < tw->tab_count; i++)
        json_str(j, session_title(&tw->tabs[i]->session));
    json_arr_end(j);
    json_key(j, "palette_open"); json_bool(j, ui_palette_is_open(&tw->palette));
    if (ui_palette_is_open(&tw->palette)) {
        json_key(j, "palette_query");
        json_str(j, ui_palette_query(&tw->palette));
    }
}

static void terminal_free(struct terminal *tw)
{
    for (int i = 0; i < tw->tab_count; i++)
        tab_destroy(tw->tabs[i]);
    free(tw->tabs);
    free(tw->tab_labels);
    terminal_root_release(tw);
    if (tw->preview_release && tw->preview_state)
        tw->preview_release(tw->preview_state);
    free(tw->menu_theme_items);
    free(tw->view.accessibility_text);
    wsap_vtcore_snapshot_free(tw->snapshot);
    free(tw->applied_font_family);
    free(tw);
}

static void op_destroy(struct window *w)
{
    terminal_tab_flight_forget(w);
    terminal_free(terminal_of(w));
}

static const struct window_drag_target TERMINAL_DRAG_TARGET = {
    .drag = terminal_tab_drag,
    .drop_hover = terminal_view_drop_hover,
};

static struct window_ui_spec terminal_ui_spec(struct terminal *tw)
{
    return (struct window_ui_spec){
        .name = "terminal",
        .kind = &terminal_kind,
        .root = &tw->root,
        .restyle = op_restyle,
        .resize = op_resize,
        .suspended = op_suspended,
        .debug_state = op_debug_state,
        .destroy = op_destroy,
        .content = tw,
    };
}

static struct terminal *terminal_new(struct window_host *host,
                                     const struct terminal_spec *spec)
{
    const struct terminal_config *config = spec->config;
    const struct window_config *cfg = host->config;
    char *family = NULL;
    if (cfg->font_family && !(family = strdup(cfg->font_family)))
        return NULL;
    struct terminal *tw = calloc(1, sizeof(*tw));
    if (!tw) {
        free(family);
        return NULL;
    }
    if (spec->theme)
        tw->theme = *spec->theme;
    tw->snapshot = wsap_vtcore_snapshot_new();
    if (!tw->snapshot) {
        free(family);
        free(tw);
        return NULL;
    }
    tw->config = config;
    tw->applied_font_family = family;
    tw->applied_font_size = cfg->font_size;
    tw->applied_cursor_style = config->cursor_style;
    tw->applied_cursor_blink = config->cursor_blink;
    tw->focused = true;
    tw->active_tab = -1;
    terminal_root_init(tw);
    ui_tabbar_set_bg_color(&tw->tabbar, tw->theme.bg);
    return tw;
}

/* Mounts a terminal on w, which has no UI yet. The caller closes w on
 * failure; a mounted terminal is freed with w. */
bool terminal_mount(struct window *w, const struct terminal_spec *spec)
{
    if (spec->tab && spec->tab->window)
        return false;
    struct terminal *tw = terminal_new(window_host(w), spec);
    if (!tw)
        return false;
    tw->window = w;
    tw->theme_picker = spec->theme_picker;
    tw->open_window = spec->open_window;
    tw->ud = spec->ud;
    tw->preview_capture = spec->preview_capture;
    tw->preview_release = spec->preview_release;
    tw->preview_ud = spec->preview_ud;
    tw->scene_paint = spec->scene_paint;
    tw->scene_paint_ud = spec->scene_paint_ud;
    window_set_drag_target(w, &TERMINAL_DRAG_TARGET, NULL);
    struct window_ui_spec ui = terminal_ui_spec(tw);
    if (!window_ui_mount(w, &ui)) {
        terminal_free(tw);
        return false;
    }
    terminal_root_attach(w);
    terminal_palette_init(w);
    /* With nothing focused, keys and text go to the grid. */
    ui_surface_set_default_focus(window_ui_surface(w), &tw->view.widget);
    /* Adopting the tab is the last step that can fail. */
    if (spec->tab) {
        if (terminal_add_tab(w, spec->tab, -1) < 0)
            return false;
        update_title(w);
        window_ui_restyle(w);
        refresh_layout(w);
        window_redraw(w);
        return true;
    }
    return spec->empty || new_tab_in(w, spec->cwd);
}
