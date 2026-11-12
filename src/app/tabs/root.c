#include <stdlib.h>
#include <string.h>

#include "app/tabs/internal.h"
#include "terminal/drop.h"
#include "ui/surface.h"
#include "util/json.h"

const int app_tabs_kind;

static struct app_tabs *root_tabs(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct app_tabs, root);
}

static struct vector2i root_measure(const struct ui_widget *widget,
                                    struct ui_constraints constraints)
{
    struct app_tabs *tabs = root_tabs(widget);
    ui_widget_measure(ui_tabbar_widget(&tabs->tabbar), constraints);
    struct ui_widget *content = tabs->mounted;
    if (content)
        ui_widget_measure(content, constraints);
    return ui_constraints_clamp(constraints, constraints.max);
}

/* The origin follows the visible bar height. Keep the content size fixed
 * until the app releases the closing entry's bar reservation. */
static void root_layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct app_tabs *tabs = root_tabs(widget);
    int bar_h = ui_tabbar_bar_height(&tabs->tabbar);
    if (bar_h > bounds.height)
        bar_h = bounds.height;
    int reserve = app_tabs_bar_height(tabs->window);
    if (reserve > bounds.height)
        reserve = bounds.height;
    int offset = ui_tabbar_height(&tabs->tabbar);
    if (offset > reserve)
        offset = reserve;
    ui_widget_place(ui_tabbar_widget(&tabs->tabbar), (struct rect2i){
        .pos = bounds.pos, .size = { .x = bounds.width, .y = bar_h },
    });
    if (tabs->mounted)
        ui_widget_place(tabs->mounted, (struct rect2i){
            .pos = { .x = bounds.x, .y = bounds.y + offset },
            .size = { .x = bounds.width, .y = bounds.height - reserve },
        });
    ui_widget_place_child(ui_tab_preview_widget(&tabs->preview), bounds);
    ui_widget_place_child(ui_palette_widget(&tabs->palette), bounds);
}

static void root_prepare(struct ui_widget *widget)
{
    struct app_tabs *tabs = root_tabs(widget);
    struct terminal *content = app_tabs_active_entry(tabs);
    uint32_t bg = content ? terminal_background(content)
        : tabs->spec.theme ? tabs->spec.theme->bg : 0;
    ui_tabbar_set_bg_color(&tabs->tabbar, bg);
    if (tabs->window)
        window_set_background(tabs->window, bg);
}

static bool root_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct app_tabs *tabs = root_tabs(widget);
    switch (event->kind) {
    case UI_EVENT_PASTE: {
        struct terminal *content = app_tabs_active_entry(tabs);
        if (content)
            terminal_paste(content, event->data.text.data,
                           event->data.text.len);
        return true;
    }
    case UI_EVENT_DROP_FILES: {
        struct terminal *content = app_tabs_active_entry(tabs);
        if (!content)
            return true;
        char *text = terminal_quote_uri_list(event->data.text.data,
                                             event->data.text.len);
        if (text) {
            terminal_paste(content, text, strlen(text));
            free(text);
        }
        return true;
    }
    case UI_EVENT_SHORTCUT:
        app_tabs_shortcut(tabs->window,
                          (enum input_shortcut)event->data.shortcut.action);
        return true;
    case UI_EVENT_WINDOW_FOCUS: {
        bool focused = event->data.window_focused;
        struct terminal *content = app_tabs_active_entry(tabs);
        if (content)
            terminal_focus(content, focused);
        if (!focused)
            ui_palette_close(&tabs->palette);
        return true;
    }
    default:
        return false;
    }
}

static uint64_t root_tick(struct ui_widget *widget, uint64_t now_ms)
{
    (void)now_ms;
    struct app_tabs *tabs = root_tabs(widget);
    uint64_t deadline = 0;
    for (int i = 0; i < tabs->count; i++) {
        const struct app_tab *entry = app_tabs_entry(tabs, i);
        uint64_t retire = entry ? entry->retire_at_ms : 0;
        if (retire && (!deadline || retire < deadline))
            deadline = retire;
    }
    return deadline;
}

static const struct ui_widget_ops TABS_ROOT_OPS = {
    .name = "app-tabs-root",
    .prepare = root_prepare,
    .measure = root_measure,
    .layout = root_layout,
    .tick = root_tick,
    .event = root_event,
};

void app_tabs_widgets_init(struct app_tabs *tabs)
{
    ui_widget_init(&tabs->root, &TABS_ROOT_OPS, NULL);
    ui_tabbar_init(&tabs->tabbar);
    ui_tab_preview_init(&tabs->preview);
    ui_palette_init(&tabs->palette);
    ui_widget_add_child(&tabs->root, ui_tabbar_widget(&tabs->tabbar));
    ui_widget_add_child(&tabs->root, ui_tab_preview_widget(&tabs->preview));
    ui_widget_add_child(&tabs->root, ui_palette_widget(&tabs->palette));
    app_tabs_actions_init(tabs);
}

static void op_restyle(struct window *w)
{
    struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs)
        return;
    app_tabs_restyle_contents(tabs);
    app_tabs_commit_geometry(tabs);
}

static void op_resize(struct window *w, struct vector2i size)
{
    (void)size;
    struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs)
        return;
    app_tabs_commit_geometry(tabs);
}

static void op_suspended(struct window *w, bool suspended)
{
    (void)suspended;
    window_redraw(w);
}

static void op_debug_state(struct window *w, struct json_writer *j)
{
    struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs)
        return;
    json_key(j, "tabs"); json_int(j, app_tabs_live_count(tabs));
    json_key(j, "active_tab"); json_int(j, tabs->active);
    json_key(j, "tabbar_height"); json_int(j, app_tabs_bar_height(w));
    json_key(j, "tab_titles");
    json_arr_begin(j);
    for (int i = 0; i < tabs->count; i++) {
        struct terminal *content = app_tabs_content(tabs, i);
        if (content)
            json_str(j, terminal_title(content));
    }
    json_arr_end(j);
    json_key(j, "palette_open"); json_bool(j, ui_palette_is_open(&tabs->palette));
    if (ui_palette_is_open(&tabs->palette)) {
        json_key(j, "palette_query");
        json_str(j, ui_palette_query(&tabs->palette));
    }
}

/* The window goes. Its tabs go back to the model, which destroys them,
 * except the tab of a running drag. */
static void op_destroy(struct window *w)
{
    struct app_tabs *tabs = app_tabs_of(w);
    if (!tabs)
        return;
    app_tabs_drag_forget(tabs->spec.model, w);
    app_tabs_release(tabs);
    free(tabs->ids);
    free(tabs->labels);
    if (tabs->spec.preview_release && tabs->preview_state)
        tabs->spec.preview_release(tabs->preview_state);
    ui_widget_destroy(&tabs->root);
    free(tabs);
}

struct window_ui_spec app_tabs_ui_spec(struct app_tabs *tabs)
{
    return (struct window_ui_spec){
        .name = "tabs",
        .kind = &app_tabs_kind,
        .root = &tabs->root,
        .restyle = op_restyle,
        .resize = op_resize,
        .suspended = op_suspended,
        .debug_state = op_debug_state,
        .destroy = op_destroy,
        .content = tabs,
    };
}

bool app_tabs_mount(struct window *w, const struct app_tabs_spec *spec)
{
    if (!w || !spec || !spec->config || !spec->model || window_ui_of(w))
        return false;
    struct app_tabs *tabs = calloc(1, sizeof(*tabs));
    if (!tabs)
        return false;
    tabs->window = w;
    tabs->spec = *spec;
    /* The initial directory and the initial tab are only for this mount. */
    tabs->spec.cwd = NULL;
    tabs->spec.tab = 0;
    tabs->active = -1;
    app_tabs_widgets_init(tabs);
    /* Drag events reach every window. The model owns the drag, so it is the
     * user data. */
    window_set_drag_target(w, &APP_TABS_DRAG_TARGET, spec->model);
    struct window_ui_spec ui = app_tabs_ui_spec(tabs);
    if (!window_ui_mount(w, &ui)) {
        free(tabs);
        return false;
    }
    /* Binds the initial tab or creates a new tab in the mounted window. */
    if (spec->tab)
        return app_tabs_bind(w, spec->tab, -1);
    if (spec->empty)
        return true;
    return app_tabs_new_in(w, spec->cwd);
}
