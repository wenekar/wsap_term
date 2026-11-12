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

struct app_tab *app_tabs_entry(const struct app_tabs *tabs, int index)
{
    if (!tabs || index < 0 || index >= tabs->count)
        return NULL;
    return app_tab_model_get(tabs->spec.model, tabs->ids[index]);
}

struct terminal *app_tabs_content(const struct app_tabs *tabs, int index)
{
    struct app_tab *entry = app_tabs_entry(tabs, index);
    return entry ? entry->content : NULL;
}

struct terminal *app_tabs_active_entry(const struct app_tabs *tabs)
{
    return tabs ? app_tabs_content(tabs, tabs->active) : NULL;
}

int app_tabs_live_count(const struct app_tabs *tabs)
{
    int n = 0;
    for (int i = 0; tabs && i < tabs->count; i++)
        if (app_tab_is_live(app_tabs_entry(tabs, i)))
            n++;
    return n;
}

int app_tabs_index_of(const struct app_tabs *tabs, app_tab_id id)
{
    for (int i = 0; tabs && id && i < tabs->count; i++)
        if (tabs->ids[i] == id)
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
        const struct app_tab *entry = app_tabs_entry(tabs, i);
        const char *text = !entry ? NULL
            : entry->content ? terminal_title(entry->content) : entry->label;
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
    for (int i = 0; tabs && i < tabs->count; i++) {
        struct terminal *content = app_tabs_content(tabs, i);
        if (content)
            terminal_restyle(content);
    }
}

void app_tabs_commit_geometry(struct app_tabs *tabs)
{
    struct terminal *content = app_tabs_active_entry(tabs);
    if (content)
        terminal_commit_geometry(content);
}

/* Puts a tab ID in the bar at `slot`, and records the window in the tab.
 * Returns the index, or -1. */
static int insert_entry(struct app_tabs *tabs, app_tab_id id, int slot)
{
    struct app_tab *entry = app_tab_model_get(tabs->spec.model, id);
    if (!entry || entry->window)
        return -1;
    if (tabs->count == tabs->cap) {
        int cap = tabs->cap ? tabs->cap * 2 : 4;
        app_tab_id *ids = realloc(tabs->ids, (size_t)cap * sizeof(*ids));
        if (!ids)
            return -1;
        tabs->ids = ids;
        tabs->cap = cap;
    }
    if (slot < 0 || slot > tabs->count)
        slot = tabs->count;
    if (!ui_tabbar_insert_tab(&tabs->tabbar, slot))
        return -1;
    memmove(&tabs->ids[slot + 1], &tabs->ids[slot],
            (size_t)(tabs->count - slot) * sizeof(*tabs->ids));
    tabs->ids[slot] = id;
    tabs->count++;
    entry->window = tabs->window;
    if (tabs->active >= slot)
        tabs->active++;
    return slot;
}

/* Removes one ID and its bar state. The tab stays in the model, and no
 * window holds it. Returns the ID, or 0. */
static app_tab_id detach_entry(struct app_tabs *tabs, int index)
{
    if (index < 0 || index >= tabs->count)
        return 0;
    app_tab_id id = tabs->ids[index];
    struct app_tab *entry = app_tab_model_get(tabs->spec.model, id);
    if (entry && entry->window == tabs->window)
        entry->window = NULL;
    memmove(&tabs->ids[index], &tabs->ids[index + 1],
            (size_t)(tabs->count - index - 1) * sizeof(*tabs->ids));
    tabs->count--;
    tabs->ids[tabs->count] = 0;
    ui_tabbar_remove_tab(&tabs->tabbar, index);
    if (tabs->active > index)
        tabs->active--;
    else if (tabs->active == index)
        tabs->active = -1;
    return id;
}

/* The closest live entry from `index`, first forward, then backward. */
static int next_live_entry(const struct app_tabs *tabs, int index)
{
    for (int i = index; i < tabs->count; i++)
        if (app_tab_is_live(app_tabs_entry(tabs, i)))
            return i;
    for (int i = index - 1; i >= 0; i--)
        if (app_tab_is_live(app_tabs_entry(tabs, i)))
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

/* The host reports go to the model. The content passes its handle, so the
 * model finds the tab by identity. The tab names the window that holds it.
 * A tab that no window holds gets no report. */
static struct app_tabs *holder_of(struct app_tab_model *model,
                                  const struct terminal *content,
                                  struct app_tab **out)
{
    struct app_tab *entry = app_tab_model_find(model, content);
    *out = entry;
    return entry ? app_tabs_of(entry->window) : NULL;
}

static void content_output(void *ud, struct terminal *content)
{
    struct app_tab *entry;
    struct app_tabs *tabs = holder_of(ud, content, &entry);
    if (!tabs || !tabs->window)
        return;
    if (app_tabs_active_entry(tabs) != content)
        return;
    if (window_surface(tabs->window))
        window_redraw(tabs->window);
}

static void content_title_changed(void *ud, struct terminal *content)
{
    struct app_tab *entry;
    struct app_tabs *tabs = holder_of(ud, content, &entry);
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
    struct app_tab *entry;
    struct app_tabs *tabs = holder_of(ud, content, &entry);
    if (!tabs || !app_tab_is_live(entry))
        return;
    app_tabs_close(tabs->window, app_tabs_index_of(tabs, entry->id));
}

/* The model is the one host of every content. The content gets it once, at
 * creation. */
static struct terminal_host model_host(struct app_tab_model *model)
{
    return (struct terminal_host){
        .output = content_output,
        .title_changed = content_title_changed,
        .exited = content_exited,
        .ud = model,
    };
}

bool app_tabs_new_in(struct window *window, const char *cwd)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs)
        return false;
    struct app_tab_model *model = tabs->spec.model;
    struct terminal *current = app_tabs_active_entry(tabs);
    int font_size = current ? terminal_font_size(current) : 0;
    struct terminal *content = terminal_create(window, &(struct terminal_spec){
        .config = tabs->spec.config,
        .theme = tabs->spec.theme,
        .theme_picker = tabs->spec.theme_picker,
        .host = model_host(model),
        .cwd = cwd,
        .content_size = new_content_size(tabs),
        .font_size = font_size,
    });
    if (!content)
        return false;
    app_tab_id id = app_tab_model_add(model, content);
    if (!id)
        return false;
    int slot = insert_entry(tabs, id, -1);
    if (slot < 0) {
        app_tab_model_destroy(model, id);
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
    return tabs->spec.open_window(tabs->spec.ud, 0, cwd,
                                  window_size(window)) != NULL;
}

void app_tabs_close(struct window *window, int index)
{
    struct app_tabs *tabs = app_tabs_of(window);
    struct app_tab *entry = app_tabs_entry(tabs, index);
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
    if (!app_tab_is_live(app_tabs_entry(tabs, index)) || index == tabs->active)
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
    if (!app_tab_is_live(app_tabs_entry(tabs, from)) ||
        !app_tab_is_live(app_tabs_entry(tabs, to)))
        return;
    app_tab_id id = tabs->ids[from];
    if (from < to)
        memmove(&tabs->ids[from], &tabs->ids[from + 1],
                (size_t)(to - from) * sizeof(*tabs->ids));
    else
        memmove(&tabs->ids[to + 1], &tabs->ids[to],
                (size_t)(from - to) * sizeof(*tabs->ids));
    tabs->ids[to] = id;
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

app_tab_id app_tabs_unbind(struct window *window, int index)
{
    struct app_tabs *tabs = app_tabs_of(window);
    struct app_tab *entry = app_tabs_entry(tabs, index);
    if (!app_tab_is_live(entry))
        return 0;
    struct terminal *content = entry->content;
    /* Unmount the widget first. The window can then close without a child
     * of the destroyed UI. */
    if (tabs->mounted && tabs->mounted == terminal_widget(content)) {
        ui_widget_remove_child(&tabs->root, tabs->mounted);
        tabs->mounted = NULL;
        tabs->mounted_content = NULL;
        ui_widget_invalidate(&tabs->root, UI_WIDGET_INVALIDATE_MEASURE);
    }
    /* The content now holds no window. Its reports find no window, so they
     * reach no view. */
    terminal_set_window(content, NULL);
    app_tab_id id = detach_entry(tabs, index);
    if (!id)
        return 0;
    if (tabs->active < 0)
        tabs->active = next_live_entry(tabs, index);
    app_tabs_publish(tabs);
    if (tabs->active < 0) {
        window_close(window);
        return id;
    }
    app_tabs_update_title(tabs);
    app_tabs_relayout(window);
    if (window_surface(window)) {
        window_repaint_all(window);
        window_redraw(window);
    }
    return id;
}

bool app_tabs_bind(struct window *window, app_tab_id id, int slot)
{
    struct app_tabs *tabs = app_tabs_of(window);
    struct app_tab *entry = tabs ? app_tab_model_get(tabs->spec.model, id) : NULL;
    if (!entry || entry->window)
        return false;
    /* Rejects binding if the tab is closing or if its child process exited. */
    if (!app_tab_is_live(entry) || terminal_exited(entry->content))
        return false;
    struct terminal *content = entry->content;
    int at = insert_entry(tabs, id, slot);
    if (at < 0)
        return false;
    /* Set the destination native targets before input resumes. The host
     * stays the model, so the reports follow the tab. */
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

bool app_tabs_move(struct window *from, int index, struct window *to, int slot)
{
    struct app_tabs *source = app_tabs_of(from);
    struct app_tabs *target = app_tabs_of(to);
    if (!source || !target || source == target || window_closed(to) ||
        source->spec.model != target->spec.model)
        return false;
    struct app_tab *entry = app_tabs_entry(source, index);
    if (!app_tab_is_live(entry) || terminal_exited(entry->content))
        return false;
    struct app_tab_model *model = source->spec.model;
    app_tab_id id = app_tabs_unbind(from, index);
    if (!id)
        return false;
    if (app_tabs_bind(to, id, slot))
        return true;
    /* No window took the tab, so it goes back to the source. A tab that no
     * window holds then goes. */
    if (window_closed(from) || !app_tabs_bind(from, id, index))
        app_tab_model_destroy(model, id);
    return false;
}

struct window *app_tabs_detach(struct window *window, int index)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || app_tabs_live_count(tabs) <= 1)
        return NULL;
    app_tab_id id = app_tabs_unbind(window, index);
    if (!id)
        return NULL;
    struct window *next = tabs->spec.open_window
        ? tabs->spec.open_window(tabs->spec.ud, id, NULL, window_size(window))
        : NULL;
    if (!next && !app_tabs_bind(window, id, index)) {
        /* No window took the tab and the source did not take it back. The
         * tab then goes. */
        app_tab_model_destroy(tabs->spec.model, id);
        return NULL;
    }
    return next;
}

void app_tabs_release(struct app_tabs *tabs)
{
    if (!tabs)
        return;
    struct app_tab_model *model = tabs->spec.model;
    /* The root goes after this call and takes its children with it. A tab
     * in flight keeps its widget, so unmount the content first. */
    if (tabs->mounted) {
        ui_widget_remove_child(&tabs->root, tabs->mounted);
        tabs->mounted = NULL;
        tabs->mounted_content = NULL;
    }
    for (int i = 0; i < tabs->count; i++) {
        app_tab_id id = tabs->ids[i];
        struct app_tab *entry = app_tab_model_get(model, id);
        if (!entry)
            continue;
        entry->window = NULL;
        if (id == model->flight.tab) {
            /* The drag still moves this tab. It outlives the window. */
            terminal_set_window(entry->content, NULL);
            continue;
        }
        app_tab_model_destroy(model, id);
    }
    tabs->count = 0;
    tabs->active = -1;
}

void app_tabs_set_theme(struct window *window,
                        const struct wsap_vtcore_theme *theme)
{
    struct app_tabs *tabs = app_tabs_of(window);
    if (!tabs || !theme)
        return;
    tabs->spec.theme = theme;
    ui_tabbar_set_bg_color(&tabs->tabbar, theme->bg);
    for (int i = 0; i < tabs->count; i++) {
        struct terminal *content = app_tabs_content(tabs, i);
        if (content)
            terminal_set_theme(content, theme);
    }
    window_set_background(window, theme->bg);
    window_redraw(window);
}

/* Takes the due closing entries out of one window. Their tabs stay in the
 * model, and no window holds them. */
static void retire_entries(struct app_tabs *tabs, uint64_t now_ms)
{
    bool changed = false;
    for (int i = tabs ? tabs->count - 1 : -1; i >= 0; i--) {
        struct app_tab *entry = app_tabs_entry(tabs, i);
        if (!entry || app_tab_is_live(entry) || now_ms < entry->retire_at_ms)
            continue;
        (void)detach_entry(tabs, i);
        changed = true;
    }
    if (!changed)
        return;
    /* The bar gives back the space of the closing entries. The content takes
     * it at the next layout. No frame follows by itself, so ask for one. */
    app_tabs_publish(tabs);
    ui_widget_invalidate(&tabs->root, UI_WIDGET_INVALIDATE_MEASURE);
    if (window_surface(tabs->window))
        window_redraw(tabs->window);
}

void app_tabs_service(struct app_tab_model *model, uint64_t now_ms)
{
    if (!model)
        return;
    /* First each window drops its due entries. This changes no model entry
     * count, so the table stays in place. */
    for (int i = 0; i < model->count; i++) {
        const struct app_tab *entry = &model->tabs[i];
        if (!app_tab_is_live(entry) && entry->retire_at_ms <= now_ms &&
            entry->window && !window_closed(entry->window))
            retire_entries(app_tabs_of(entry->window), now_ms);
    }
    /* Then the model destroys each closing tab that no window holds. The tab
     * of a running drag stays, because the end of the drag settles it. */
    for (int i = model->count - 1; i >= 0; i--) {
        if (i >= model->count)
            continue;
        const struct app_tab *entry = &model->tabs[i];
        if (!app_tab_is_live(entry) && !entry->window &&
            entry->id != model->flight.tab)
            app_tab_model_destroy(model, entry->id);
    }
}
