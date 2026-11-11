#include <stdlib.h>
#include <string.h>

#include "app/tabs/internal.h"
#include "session/session.h"
#include "ui/surface.h"

struct app_tabs *app_tabs_of(const struct window *w)
{
    return w && window_ui_kind(w) == &app_tabs_kind
        ? window_ui_content(w) : NULL;
}

struct terminal *app_tabs_active_entry(const struct app_tabs *tabs)
{
    if (!tabs || tabs->active < 0 || tabs->active >= tabs->count)
        return NULL;
    return tabs->tabs[tabs->active].content;
}

int app_tabs_live_count(const struct app_tabs *tabs)
{
    int n = 0;
    for (int i = 0; tabs && i < tabs->count; i++)
        if (app_tab_is_live(&tabs->tabs[i]))
            n++;
    return n;
}

int app_tabs_find(const struct app_tabs *tabs, const struct terminal *content)
{
    for (int i = 0; tabs && i < tabs->count; i++)
        if (tabs->tabs[i].content == content)
            return i;
    return -1;
}

struct terminal *app_tabs_active(const struct window *w)
{
    return app_tabs_active_entry(app_tabs_of(w));
}

int app_tabs_count(const struct window *w)
{
    return app_tabs_live_count(app_tabs_of(w));
}

int app_tabs_active_index(const struct window *w)
{
    const struct app_tabs *tabs = app_tabs_of(w);
    return tabs ? tabs->active : -1;
}

static int bar_css_height(const struct app_tabs *tabs)
{
    return tabs ? ui_tabbar_bar_height(&tabs->tabbar) : 0;
}

int app_tabs_bar_height(const struct window *w)
{
    const struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs || tabs->count < 2)
        return 0;
    return bar_css_height(tabs);
}

bool app_tabs_is(const struct window *w)
{
    return app_tabs_of(w) != NULL;
}

void app_tabs_update_title(struct app_tabs *tabs)
{
    if (!tabs || !tabs->window)
        return;
    struct terminal *content = app_tabs_active_entry(tabs);
    window_set_title(tabs->window, content ? terminal_title(content) : NULL);
}

void app_tabs_publish(struct app_tabs *tabs)
{
    if (!tabs)
        return;
    int n = tabs->count;
    if (n > tabs->labels_cap) {
        size_t want = (size_t)n < 8 ? 8 : (size_t)n;
        struct ui_tab_label *labels =
            realloc(tabs->labels, want * sizeof(*labels));
        if (!labels)
            return;
        tabs->labels = labels;
        tabs->labels_cap = (int)want;
    }
    for (int i = 0; i < n; i++) {
        const struct app_tab *entry = &tabs->tabs[i];
        const char *text = entry->content ? terminal_title(entry->content)
                                          : entry->label;
        tabs->labels[i] = (struct ui_tab_label){
            .text = text && text[0] ? text : "",
            .active = i == tabs->active && app_tab_is_live(entry),
        };
    }
    ui_tabbar_set_tabs(&tabs->tabbar, tabs->labels, n);
}

void app_tabs_mount_content(struct app_tabs *tabs)
{
    if (!tabs)
        return;
    struct terminal *content = app_tabs_active_entry(tabs);
    struct ui_surface *surface = tabs->window ? window_ui_surface(tabs->window) : NULL;
    /* The keyboard falls back to the input widget of the active content. */
    if (surface)
        ui_surface_set_default_focus(surface,
            content ? terminal_input_widget(content) : NULL);
    struct ui_widget *want = content ? terminal_widget(content) : NULL;
    if (want == tabs->mounted)
        return;
    if (tabs->mounted) {
        /* The content that leaves the UI keeps no scrollbar flash. */
        if (tabs->mounted_content && tabs->mounted_content != content)
            terminal_hide_scrollbar(tabs->mounted_content);
        ui_widget_remove_child(&tabs->root, tabs->mounted);
    }
    tabs->mounted = NULL;
    tabs->mounted_content = NULL;
    if (want) {
        ui_widget_insert_child(&tabs->root, want,
                               ui_tabbar_widget(&tabs->tabbar));
        tabs->mounted = want;
        tabs->mounted_content = content;
    }
    ui_widget_invalidate(&tabs->root, UI_WIDGET_INVALIDATE_MEASURE);
}

void app_tabs_restyle_contents(struct app_tabs *tabs)
{
    for (int i = 0; tabs && i < tabs->count; i++)
        if (tabs->tabs[i].content)
            terminal_restyle(tabs->tabs[i].content);
}

void app_tabs_commit_geometry(struct app_tabs *tabs)
{
    struct terminal *content = app_tabs_active_entry(tabs);
    if (content)
        terminal_commit_geometry(content);
}

static int insert_entry(struct app_tabs *tabs, struct terminal *content,
                        int slot)
{
    if (tabs->count == tabs->cap) {
        int cap = tabs->cap ? tabs->cap * 2 : 4;
        struct app_tab *entries =
            realloc(tabs->tabs, (size_t)cap * sizeof(*entries));
        if (!entries)
            return -1;
        tabs->tabs = entries;
        tabs->cap = cap;
    }
    if (slot < 0 || slot > tabs->count)
        slot = tabs->count;
    if (!ui_tabbar_insert_tab(&tabs->tabbar, slot))
        return -1;
    memmove(&tabs->tabs[slot + 1], &tabs->tabs[slot],
            (size_t)(tabs->count - slot) * sizeof(*tabs->tabs));
    tabs->tabs[slot] = (struct app_tab){ .content = content };
    tabs->count++;
    if (tabs->active >= slot)
        tabs->active++;
    return slot;
}

/* Removes one entry and its bar state. Returns the content of the entry, or
 * NULL when the entry was a closing display entry. */
static struct terminal *detach_entry(struct app_tabs *tabs, int index)
{
    if (index < 0 || index >= tabs->count)
        return NULL;
    struct terminal *content = tabs->tabs[index].content;
    free(tabs->tabs[index].label);
    memmove(&tabs->tabs[index], &tabs->tabs[index + 1],
            (size_t)(tabs->count - index - 1) * sizeof(*tabs->tabs));
    tabs->count--;
    memset(&tabs->tabs[tabs->count], 0, sizeof(*tabs->tabs));
    ui_tabbar_remove_tab(&tabs->tabbar, index);
    if (tabs->active > index)
        tabs->active--;
    else if (tabs->active == index)
        tabs->active = -1;
    return content;
}

/* The closest live entry from `index`, first forward, then backward. */
static int next_live_entry(const struct app_tabs *tabs, int index)
{
    for (int i = index; i < tabs->count; i++)
        if (app_tab_is_live(&tabs->tabs[i]))
            return i;
    for (int i = index - 1; i >= 0; i--)
        if (app_tab_is_live(&tabs->tabs[i]))
            return i;
    return -1;
}

void app_tabs_relayout(struct window *w)
{
    struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs)
        return;
    app_tabs_mount_content(tabs);
    window_ui_relayout(w);
    app_tabs_commit_geometry(tabs);
}

static struct vector2i new_content_size(const struct app_tabs *tabs)
{
    struct vector2i size = window_size(tabs->window);
    int reserve = tabs->count + 1 >= 2 ? bar_css_height(tabs) : 0;
    size.y -= reserve;
    if (size.y < 1)
        size.y = 1;
    return size;
}

/* The host reports. The content passes its handle, so the app finds the tab
 * by identity. */
static struct terminal_host content_host(struct app_tabs *tabs);

static void content_output(void *ud, struct terminal *content)
{
    struct app_tabs *tabs = ud;
    if (!tabs || !tabs->window)
        return;
    if (app_tabs_active_entry(tabs) != content)
        return;
    if (window_surface(tabs->window))
        window_redraw(tabs->window);
}

static void content_title_changed(void *ud, struct terminal *content)
{
    struct app_tabs *tabs = ud;
    if (!tabs || !tabs->window)
        return;
    if (app_tabs_active_entry(tabs) == content)
        app_tabs_update_title(tabs);
    app_tabs_publish(tabs);
    if (window_surface(tabs->window))
        window_redraw(tabs->window);
}

static void content_exited(void *ud, struct terminal *content)
{
    struct app_tabs *tabs = ud;
    int index = app_tabs_find(tabs, content);
    if (index < 0 || !app_tab_is_live(&tabs->tabs[index]))
        return;
    app_tabs_close(tabs->window, index);
}

static struct terminal_host content_host(struct app_tabs *tabs)
{
    return (struct terminal_host){
        .output = content_output,
        .title_changed = content_title_changed,
        .exited = content_exited,
        .ud = tabs,
    };
}

bool app_tabs_new_in(struct window *window, const char *cwd)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs)
        return false;
    struct terminal *current = app_tabs_active_entry(tabs);
    int font_size = current ? terminal_font_size(current) : 0;
    struct terminal *content = terminal_create(window, &(struct terminal_spec){
        .config = tabs->spec.config,
        .theme = tabs->spec.theme,
        .theme_picker = tabs->spec.theme_picker,
        .host = content_host(tabs),
        .cwd = cwd,
        .content_size = new_content_size(tabs),
        .font_size = font_size,
    });
    if (!content)
        return false;
    int slot = insert_entry(tabs, content, -1);
    if (slot < 0) {
        terminal_destroy(content);
        return false;
    }
    tabs->active = slot;
    app_tabs_publish(tabs);
    if (app_tabs_live_count(tabs) > 1)
        ui_tabbar_begin_open(&tabs->tabbar, slot, window_frame_time_ms(window));
    app_tabs_update_title(tabs);
    app_tabs_relayout(window);
    if (window_surface(window))
        window_redraw(window);
    return true;
}

bool app_tabs_new(struct window *window)
{
    struct terminal *current = app_tabs_active(window);
    struct session *session = terminal_session(current);
    char cwd[4096];
    return app_tabs_new_in(window,
        session && session_cwd(session, cwd, sizeof(cwd)) ? cwd : NULL);
}

bool app_tabs_new_window(struct window *window)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || !tabs->spec.open_window)
        return false;
    struct terminal *current = app_tabs_active_entry(tabs);
    char cwd_buf[4096];
    const char *cwd = NULL;
    if (current) {
        struct session *session = terminal_session(current);
        if (session && session_cwd(session, cwd_buf, sizeof(cwd_buf)))
            cwd = cwd_buf;
    }
    return tabs->spec.open_window(tabs->spec.ud, NULL, cwd,
                                  window_size(window)) != NULL;
}

void app_tabs_close(struct window *window, int index)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || index < 0 || index >= tabs->count)
        return;
    struct app_tab *entry = &tabs->tabs[index];
    if (!app_tab_is_live(entry))
        return;
    struct terminal *content = entry->content;
    uint64_t now = window_frame_time_ms(window);
    bool active = index == tabs->active;
    /* The entry becomes a closing display entry. It keeps the content until
     * the app retires it, so no callback frees the content. */
    const char *title = terminal_title(content);
    entry->label = title ? strdup(title) : NULL;
    entry->retire_at_ms = now + APP_TABS_CLOSE_MS;
    if (active)
        tabs->active = next_live_entry(tabs, index);
    bool more = app_tabs_live_count(tabs) > 0;
    if (more)
        ui_tabbar_begin_close(&tabs->tabbar, index, now);
    app_tabs_publish(tabs);
    if (!more) {
        app_tabs_mount_content(tabs);
        window_close(window);
        return;
    }
    /* Keep the bar reservation until the app retires the closing entry. */
    app_tabs_update_title(tabs);
    app_tabs_relayout(window);
    if (window_surface(window)) {
        window_repaint_all(window);
        window_redraw(window);
    }
}

void app_tabs_select(struct window *window, int index)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || index < 0 || index >= tabs->count)
        return;
    if (!app_tab_is_live(&tabs->tabs[index]) || index == tabs->active)
        return;
    tabs->active = index;
    app_tabs_publish(tabs);
    app_tabs_update_title(tabs);
    app_tabs_relayout(window);
    if (window_surface(window)) {
        window_repaint_all(window);
        window_redraw(window);
    }
}

void app_tabs_reorder(struct window *window, int from, int to)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || from == to || from < 0 || to < 0 ||
        from >= tabs->count || to >= tabs->count)
        return;
    if (!app_tab_is_live(&tabs->tabs[from]) ||
        !app_tab_is_live(&tabs->tabs[to]))
        return;
    struct app_tab entry = tabs->tabs[from];
    if (from < to)
        memmove(&tabs->tabs[from], &tabs->tabs[from + 1],
                (size_t)(to - from) * sizeof(*tabs->tabs));
    else
        memmove(&tabs->tabs[to + 1], &tabs->tabs[to],
                (size_t)(from - to) * sizeof(*tabs->tabs));
    tabs->tabs[to] = entry;
    ui_tabbar_move_tab(&tabs->tabbar, from, to);
    if (tabs->active == from)
        tabs->active = to;
    else if (from < to && tabs->active > from && tabs->active <= to)
        tabs->active--;
    else if (from > to && tabs->active >= to && tabs->active < from)
        tabs->active++;
    app_tabs_publish(tabs);
    app_tabs_update_title(tabs);
    if (window_surface(window))
        window_redraw(window);
}

struct terminal *app_tabs_take(struct window *window, int index)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || index < 0 || index >= tabs->count ||
        !app_tab_is_live(&tabs->tabs[index]))
        return NULL;
    struct terminal *content = tabs->tabs[index].content;
    /* Unmount the widget first. The window can then close without a child
     * of the destroyed UI. */
    if (tabs->mounted && tabs->mounted == terminal_widget(content)) {
        ui_widget_remove_child(&tabs->root, tabs->mounted);
        tabs->mounted = NULL;
        tabs->mounted_content = NULL;
        ui_widget_invalidate(&tabs->root, UI_WIDGET_INVALIDATE_MEASURE);
    }
    /* The content now reports to no owner and holds no window. */
    terminal_set_host(content, NULL);
    terminal_set_window(content, NULL);
    if (!detach_entry(tabs, index))
        return NULL;
    if (tabs->active < 0)
        tabs->active = next_live_entry(tabs, index);
    app_tabs_publish(tabs);
    if (tabs->active < 0) {
        window_close(window);
        return content;
    }
    app_tabs_update_title(tabs);
    app_tabs_relayout(window);
    if (window_surface(window)) {
        window_repaint_all(window);
        window_redraw(window);
    }
    return content;
}

bool app_tabs_attach(struct window *window, struct terminal *content, int slot)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || !content || app_tabs_find(tabs, content) >= 0)
        return false;
    /* A content whose process ended keeps no tab. The caller still owns it,
     * and it destroys the content once. */
    if (terminal_exited(content))
        return false;
    int at = insert_entry(tabs, content, slot);
    if (at < 0)
        return false;
    /* The window owns the content. Set the destination reports and the
     * destination native targets before input resumes. */
    struct terminal_host host = content_host(tabs);
    terminal_set_host(content, &host);
    terminal_set_window(content, window);
    tabs->active = at;
    app_tabs_publish(tabs);
    if (app_tabs_live_count(tabs) > 1)
        ui_tabbar_begin_open(&tabs->tabbar, at, window_frame_time_ms(window));
    app_tabs_update_title(tabs);
    app_tabs_relayout(window);
    if (window_surface(window))
        window_redraw(window);
    return true;
}

struct window *app_tabs_detach(struct window *window, int index)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || app_tabs_live_count(tabs) <= 1)
        return NULL;
    struct terminal *content = app_tabs_take(window, index);
    if (!content)
        return NULL;
    struct window *next = tabs->spec.open_window
        ? tabs->spec.open_window(tabs->spec.ud, content, NULL,
                                 window_size(window))
        : NULL;
    if (!next && !app_tabs_attach(window, content, index)) {
        /* No window took the content and the source did not take it back.
         * The content then has one owner, and it goes. */
        terminal_destroy(content);
        return NULL;
    }
    return next;
}

void app_tabs_set_theme(struct window *window,
                        const struct wsap_vtcore_theme *theme)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || !theme)
        return;
    tabs->spec.theme = theme;
    ui_tabbar_set_bg_color(&tabs->tabbar, theme->bg);
    for (int i = 0; i < tabs->count; i++)
        if (tabs->tabs[i].content)
            terminal_set_theme(tabs->tabs[i].content, theme);
    window_set_background(window, theme->bg);
    window_redraw(window);
}

void app_tabs_prepare(struct window *window, uint64_t now_ms)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs)
        return;
    bool changed = false;
    for (int i = tabs->count - 1; i >= 0; i--) {
        struct app_tab *entry = &tabs->tabs[i];
        if (app_tab_is_live(entry) || !entry->content ||
            now_ms < entry->retire_at_ms)
            continue;
        /* This runs at the frame boundary, never inside a content
         * callback. */
        struct terminal *gone = detach_entry(tabs, i);
        terminal_destroy(gone);
        changed = true;
    }
    if (changed) {
        app_tabs_publish(tabs);
        ui_widget_invalidate(&tabs->root, UI_WIDGET_INVALIDATE_MEASURE);
    }
    app_tabs_mount_content(tabs);
}
