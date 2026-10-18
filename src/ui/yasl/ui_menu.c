#define _GNU_SOURCE
#include "ui/yasl/internal.h"
#include "ui/yasl/inspector.h"

static bool popup_root_event(struct ui_widget *widget,
                             const struct ui_event *event)
{
    struct window *w = widget->userdata;
    if (event->kind == UI_EVENT_POINTER &&
        event->data.pointer.kind == PTR_PRESS)
        ui_menu_close(&ui_of(w)->menu);
    return event->kind == UI_EVENT_POINTER;
}

static bool popup_root_accessibility(const struct ui_widget *widget,
                                     struct ui_accessibility_node_spec *out)
{
    (void)widget;
    out->role = UI_ACCESSIBILITY_ROLE_MENU;
    out->name = "Menu";
    return true;
}

static const struct ui_widget_ops POPUP_ROOT_OPS = {
    .name = "menu-popup-root",
    .event = popup_root_event,
    .accessibility = popup_root_accessibility,
    .child_limit = 1,
};

static bool popup_open(const struct window_ui_menu_popup *p)
{
    return p->native.surface != NULL;
}

static void close_popup(struct window *w, int depth)
{
    struct window_ui_menu_popup *p = &ui_of(w)->menu_popup[depth];
    if (!popup_open(p))
        return;
    window_accessibility_menu_popup_close(w, depth);
    ui_surface_destroy(&p->ui);
    while (p->root.first_child)
        ui_widget_remove_child(&p->root, p->root.first_child);
    ui_widget_destroy(&p->root);
    window_native_destroy(&p->native);
    *p = (struct window_ui_menu_popup){0};
}

static void close_popups_from(struct window *w, int depth)
{
    while (ui_of(w)->menu_popups > depth)
        close_popup(w, --ui_of(w)->menu_popups);
}

static struct vector2i measure_popup(struct window *w, int depth)
{
    struct window_ui_menu_popup *p = &ui_of(w)->menu_popup[depth];
    struct ui_widget *level = ui_menu_level_widget(&ui_of(w)->menu, depth);
    ui_surface_resize(&p->ui, window_size(w));
    struct insets2i shadow = ui_menu_level_shadow(&ui_of(w)->menu, depth);
    struct vector2i size = vector2i_add(level->measured_size,
                                        insets2i_extent(shadow));
    ui_widget_set_placement(level, (struct ui_placement){
        .kind = UI_PLACE_AT,
        .anchor = { .pos = { .x = shadow.l, .y = shadow.t } },
    });
    ui_surface_resize(&p->ui, size);
    return size;
}

static struct yasl_popup_options popup_options(struct window *w, int depth,
                                               struct vector2i size, struct rect2i anchor)
{
    return (struct yasl_popup_options){
        .width = size.x,
        .height = size.y,
        .x = anchor.x,
        .y = anchor.y,
        .anchor_width = anchor.width,
        .anchor_height = anchor.height,
        .anchor = depth > 0 ? YASL_EDGE_TOP | YASL_EDGE_RIGHT : YASL_EDGE_BOTTOM | YASL_EDGE_LEFT,
        .gravity = YASL_EDGE_BOTTOM | YASL_EDGE_RIGHT,
        .adjust = YASL_ADJUST_SLIDE_X | YASL_ADJUST_SLIDE_Y | YASL_ADJUST_FLIP_X | YASL_ADJUST_FLIP_Y,
        .userdata = &ui_of(w)->menu_popup[depth].native,
    };
}

static bool open_popup(struct window *w, int depth, struct rect2i anchor)
{
    if (!(yasl_get_features() & YASL_FEATURE_POPUPS))
        return false;
    struct window_ui_menu_popup *p = &ui_of(w)->menu_popup[depth];
    struct ui_widget *level = ui_menu_level_widget(&ui_of(w)->menu, depth);
    if (level->parent)
        ui_widget_remove_child(level->parent, level);
    ui_widget_init(&p->root, &POPUP_ROOT_OPS, w);
    ui_widget_set_modal(&p->root, true);
    ui_widget_add_child(&p->root, level);
    ui_surface_init(&p->ui, &window_ui_surface_host, w);
    ui_surface_set_sheet(&p->ui, ui_of(w)->sheet);
    ui_surface_set_scale(&p->ui, window_ui_scale(w));
    ui_surface_set_root(&p->ui, &p->root);

    struct vector2i size = measure_popup(w, depth);
    struct yasl_surface *parent = depth == 0 ? window_surface(w)
                                        : ui_of(w)->menu_popup[depth - 1].native.surface;
    struct yasl_popup_options options = popup_options(w, depth, size, anchor);
    p->native = (struct window_native){ .window = w, .id = depth + 1 };
    p->native.surface = yasl_popup_create(parent, &options);
    if (!p->native.surface) {
        ui_surface_destroy(&p->ui);
        ui_widget_remove_child(&p->root, level);
        ui_widget_destroy(&p->root);
        *p = (struct window_ui_menu_popup){0};
        return false;
    }
    window_accessibility_menu_popup_open(w, depth);
    p->anchor = anchor;
    return true;
}

static struct rect2i level_anchor(struct window *w, int depth)
{
    return depth == 0
        ? (struct rect2i){ .pos = ui_of(w)->menu_at, .size = { .x = 1, .y = 1 } }
        : ui_menu_level_anchor(&ui_of(w)->menu, depth);
}

void window_menu_sync(struct window *w)
{
    if (!window_ui_of(w))
        return;
    uint32_t gen = ui_menu_generation(&ui_of(w)->menu);
    if (ui_of(w)->menu_shown == gen)
        return;
    ui_of(w)->menu_shown = gen;
    int levels = ui_menu_level_count(&ui_of(w)->menu);
    for (int d = 0; d < ui_of(w)->menu_popups; d++) {
        if (d >= levels ||
            !rect2i_eq(ui_of(w)->menu_popup[d].anchor, level_anchor(w, d))) {
            close_popups_from(w, d);
            break;
        }
    }
    while (ui_of(w)->menu_popups < levels && ui_of(w)->menu_popups < ui_of(w)->menu_inline_from) {
        if (!window_surface(w) ||
            !open_popup(w, ui_of(w)->menu_popups, level_anchor(w, ui_of(w)->menu_popups))) {
            ui_of(w)->menu_inline_from = ui_of(w)->menu_popups;
            break;
        }
        ui_of(w)->menu_popups++;
    }
    ui_menu_layer_set_first_level(&ui_of(w)->menu_layer, ui_of(w)->menu_popups);
}

void window_menu_relayout(struct window *w)
{
    for (int d = 0; d < ui_of(w)->menu_popups; d++) {
        struct window_ui_menu_popup *p = &ui_of(w)->menu_popup[d];
        struct rect2i anchor = level_anchor(w, d);
        struct vector2i size = measure_popup(w, d);
        struct yasl_popup_options options = popup_options(w, d, size, anchor);
        if (yasl_popup_reposition(p->native.surface, &options) != 0) {
            close_popups_from(w, d);
            ui_of(w)->menu_inline_from = d;
            ui_menu_layer_set_first_level(&ui_of(w)->menu_layer, d);
            break;
        }
        p->anchor = anchor;
    }
}

void window_menu_close(struct window *w)
{
    if (!window_ui_of(w))
        return;
    ui_menu_close(&ui_of(w)->menu);
    window_menu_sync(w);
}

struct ui_surface *window_ui_surface_at(struct window *w, int popup)
{
    return popup > 0 && popup <= ui_of(w)->menu_popups
        ? &ui_of(w)->menu_popup[popup - 1].ui : &ui_of(w)->screen;
}

struct ui_surface *window_ui_key_surface(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return NULL;
    return ui->menu_popups > 0 ? &ui->menu_popup[ui->menu_popups - 1].ui : &ui->screen;
}

bool window_menu_open(struct window *w, const ui_menu_item *items, int n,
                      struct vector2i at, ui_action_fn fn,
                      ui_menu_submenu_fn submenu, void *userdata)
{
    if (!window_surface(w) || !window_ui_of(w))
        return false;
    window_menu_close(w);
    int delay = window_host(w)->config->menu_hover_delay_ms;
    ui_of(w)->menu_at = at;
    ui_of(w)->menu_inline_from = UI_MENU_MAX_LEVELS;
    if (!ui_menu_show(&ui_of(w)->menu, &ui_of(w)->menu_layer, &(struct ui_menu_desc){
            .pos = at,
            .items = items,
            .count = n,
            .on_action = fn,
            .on_submenu = submenu,
            .userdata = userdata,
            .hover_delay_ms = delay > 0 ? (uint32_t)delay : 0,
        }))
        return false;
    window_menu_sync(w);
    window_redraw(w);
    return true;
}

void window_menu_destroy(struct window *w)
{
    ui_menu_close(&ui_of(w)->menu);
    close_popups_from(w, 0);
    struct ui_widget *layer = ui_menu_layer_widget(&ui_of(w)->menu_layer);
    while (layer->first_child)
        ui_widget_remove_child(layer, layer->first_child);
    ui_menu_destroy(&ui_of(w)->menu);
}
