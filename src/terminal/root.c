#include <stdlib.h>
#include <string.h>

#include "terminal/drop.h"
#include "terminal/internal.h"

static struct terminal *root_terminal(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct terminal, root);
}

static struct vector2i root_measure(const struct ui_widget *widget,
                                    struct ui_constraints constraints)
{
    struct terminal *tw = root_terminal(widget);
    struct ui_constraints bar = constraints;
    bar.min.y = 0;
    ui_widget_measure(ui_tabbar_widget(&tw->tabbar), bar);
    for (struct ui_widget *c = ui_scrollview_widget(&tw->scrollview); c;
         c = c->next_sibling)
        if (!ui_widget_is_hidden(c))
            ui_widget_measure(c, constraints);
    return ui_constraints_clamp(constraints, constraints.max);
}

static void root_layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct terminal *tw = root_terminal(widget);
    struct ui_widget *bar = ui_tabbar_widget(&tw->tabbar);
    int bar_h = bar->measured_size.y < bounds.height
        ? bar->measured_size.y : bounds.height;
    ui_widget_place(bar, (struct rect2i){
        .pos = bounds.pos, .size = { .x = bounds.width, .y = bar_h },
    });
    ui_widget_place(ui_scrollview_widget(&tw->scrollview), (struct rect2i){
        .pos = { .x = bounds.x, .y = bounds.y + bar_h },
        .size = { .x = bounds.width, .y = bounds.height - bar_h },
    });
    ui_widget_place_child(ui_tab_preview_widget(&tw->tab_preview), bounds);
    ui_widget_place_child(ui_palette_widget(&tw->palette), bounds);
}

static void push_labels(struct terminal *tw)
{
    int n = tw->tab_count;
    if (n > tw->tab_labels_cap) {
        size_t want = (size_t)n < 8 ? 8 : (size_t)n;
        struct ui_tab_label *labels = realloc(tw->tab_labels,
                                              want * sizeof(*labels));
        if (!labels)
            return;
        tw->tab_labels = labels;
        tw->tab_labels_cap = (int)want;
    }
    for (int i = 0; i < n; i++)
        tw->tab_labels[i] = (struct ui_tab_label){
            .text = session_title(&tw->tabs[i]->session),
            .active = i == tw->active_tab,
        };
    ui_tabbar_set_tabs(&tw->tabbar, tw->tab_labels, n);
}

static void root_prepare(struct ui_widget *widget)
{
    struct terminal *tw = root_terminal(widget);
    struct window *w = tw->window;
    push_labels(tw);
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(tw->snapshot);
    uint32_t bg = view->size.x
        ? view->palette[WSAP_VTCORE_PALETTE_BG]
        : tw->theme.bg;
    ui_tabbar_set_bg_color(&tw->tabbar, bg);
    window_set_background(w, bg);
    struct wsap_vtcore *t = terminal_term(w);
    ui_scrollview_set_metrics(&tw->scrollview, t ? terminal_scroll_metrics(t)
                                                 : (struct ui_scroll_metrics){0});
}

static bool root_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct window *w = root_terminal(widget)->window;
    switch (event->kind) {
    case UI_EVENT_PASTE:
        terminal_paste(w, event->data.text.data, event->data.text.len);
        return true;
    case UI_EVENT_DROP_FILES: {
        char *text = terminal_quote_uri_list(event->data.text.data,
                                             event->data.text.len);
        if (text) {
            terminal_paste(w, text, strlen(text));
            free(text);
        }
        return true;
    }
    case UI_EVENT_SHORTCUT:
        terminal_handle_shortcut(w, (enum input_shortcut)event->data.shortcut.action);
        return true;
    case UI_EVENT_WINDOW_FOCUS:
        terminal_focus(w, event->data.window_focused);
        return true;
    default:
        return false;
    }
}

static const struct ui_widget_ops TERMINAL_ROOT_OPS = {
    .name = "terminal-root",
    .prepare = root_prepare,
    .measure = root_measure,
    .layout = root_layout,
    .event = root_event,
};

static void preview_hover(struct window *w, int index)
{
    struct terminal *tw = terminal_of(w);
    struct rect2i anchor = {0};
    if (index == tw->active_tab ||
        !ui_tabbar_tab_rect(&tw->tabbar, index, &anchor))
        index = -1;
    ui_tab_preview_hover(&tw->tab_preview, index, anchor);
}

static void preview_drag(struct window *w, const struct ui_action *a)
{
    struct terminal *tw = terminal_of(w);
    if (a->id == UI_TAB_DRAG_DETACH)
        ui_tab_preview_follow(&tw->tab_preview, a->index, a->pos);
    else
        ui_tab_preview_suppress(&tw->tab_preview);
}

static void tab_moved(struct window *w, const struct ui_action *a)
{
    struct terminal *tw = terminal_of(w);
    ui_tab_preview_suppress(&tw->tab_preview);
    if (a->to >= 0) {
        terminal_reorder_tab(w, a->index, a->to);
        return;
    }
    struct window *nw = terminal_detach_tab(w, a->index);
    if (nw && window_surface(nw))
        window_place_under_pointer(nw, window_logical(w, (struct vector2i){ .x = a->pos.x }));
}

static bool tabbar_action(struct ui_widget *source, const struct ui_action *a,
                          void *userdata)
{
    (void)source;
    struct window *w = userdata;
    struct terminal *tw = terminal_of(w);
    switch (a->kind) {
    case UI_ACTION_ACTIVATE:
        ui_tab_preview_suppress(&tw->tab_preview);
        if (a->id == UI_TABBAR_NEW_TAB)
            (void)terminal_new_tab(w);
        else
            terminal_select_tab(w, a->index);
        return true;
    case UI_ACTION_CLOSE:
        ui_tab_preview_suppress(&tw->tab_preview);
        terminal_close_tab(w, a->index);
        return true;
    case UI_ACTION_HIGHLIGHT:
        preview_hover(w, a->index);
        return true;
    case UI_ACTION_DRAG:
        preview_drag(w, a);
        return true;
    case UI_ACTION_TEAR_OFF:
        return terminal_tab_drag_begin(w, a->index, a->pos);
    case UI_ACTION_MOVE:
        tab_moved(w, a);
        return true;
    default:
        return false;
    }
}

void terminal_root_init(struct terminal *tw)
{
    ui_widget_init(&tw->root, &TERMINAL_ROOT_OPS, NULL);
    ui_tabbar_init(&tw->tabbar);
    terminal_view_init(tw);
    ui_tab_preview_init(&tw->tab_preview);
    ui_palette_init(&tw->palette);
    ui_widget_add_child(&tw->root, ui_tabbar_widget(&tw->tabbar));
    ui_widget_add_child(&tw->root, ui_scrollview_widget(&tw->scrollview));
    ui_widget_add_child(&tw->root, ui_tab_preview_widget(&tw->tab_preview));
    ui_widget_add_child(&tw->root, ui_palette_widget(&tw->palette));
}

void terminal_root_attach(struct window *w)
{
    struct terminal *tw = terminal_of(w);
    ui_widget_set_action_handler(ui_tabbar_widget(&tw->tabbar),
                                 tabbar_action, w);
    terminal_view_attach(w);
    terminal_tab_preview_init(w);
}

void terminal_root_release(struct terminal *tw)
{
    ui_widget_destroy(&tw->root);
}
