#include "ui/widgets/palette.h"

#include "ui/debug.h"
#include "ui/paint.h"
#include "ui/surface.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define PALETTE_EMPTY_TEXT "No matching commands"

static struct ui_palette *palette_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_palette, widget);
}

static const struct css_scale_context *scale_of(const struct ui_palette *p)
{
    return &p->widget.style_scale;
}

static int cell_w(const struct ui_palette *p)
{
    return ui_widget_text_style(scale_of(p)).advance;
}

static int cell_h(const struct ui_palette *p)
{
    return ui_widget_text_style(scale_of(p)).cell_h;
}

static int border_width(const struct ui_palette *p)
{
    const struct css_style *s = &p->widget.style;
    return s->has_border_color
        ? ui_css_resolve_int(s->border_width, 0, 0, scale_of(p), true) : 0;
}

static int separator_height(const struct ui_palette *p)
{
    int sep = ui_ctx_px(scale_of(p), 1);
    return sep > 0 ? sep : 1;
}

static void free_cmds(struct ui_palette *p)
{
    for (int i = 0; i < p->impl.n_cmds; i++) {
        free((char *)p->impl.cmds[i].label);
        free((char *)p->impl.cmds[i].shortcut);
    }
    free(p->impl.cmds);
    p->impl.cmds = NULL;
    p->impl.n_cmds = 0;
}

static bool matches(const char *query, const char *label)
{
    for (; *query; query++) {
        while (*label && tolower((unsigned char)*label) !=
                         tolower((unsigned char)*query))
            label++;
        if (!*label++)
            return false;
    }
    return true;
}

static int fitting_rows(const struct ui_palette *p, int parent_height)
{
    const struct ui_list_metrics *m = &p->list.impl.metrics;
    if (!m->valid)
        return UI_PALETTE_MAX_ROWS;
    int row = m->cell_h + m->item_pad.t + m->item_pad.b;
    if (row < m->min_h)
        row = m->min_h;
    int rows = (parent_height / 2 - 2 * m->edge_pad) / (row > 0 ? row : 1);
    return rows < 1 ? 1 : rows > UI_PALETTE_MAX_ROWS ? UI_PALETTE_MAX_ROWS : rows;
}

static void refilter(struct ui_palette *p)
{
    struct ui_list_item items[UI_PALETTE_MAX_ROWS];
    int cap = p->impl.max_rows > 0 ? p->impl.max_rows : UI_PALETTE_MAX_ROWS;
    const char *query = ui_input_text(&p->input);
    int n = 0;
    for (int i = 0; i < p->impl.n_cmds && n < cap; i++)
        if (matches(query, p->impl.cmds[i].label))
            items[n++] = (struct ui_list_item){
                .label = p->impl.cmds[i].label,
                .shortcut = p->impl.cmds[i].shortcut,
                .action = p->impl.cmds[i].action,
            };
    ui_list_set_items(&p->list, items, n);
    ui_list_step(&p->list, 1);
    ui_widget_set_hidden(&p->list.widget, n == 0);
    ui_widget_set_hidden(&p->empty.widget, n > 0 || !query[0]);
}

static void palette_prepare(struct ui_widget *widget)
{
    struct ui_palette *p = palette_of(widget);
    if (p->impl.stale) {
        p->impl.stale = false;
        refilter(p);
    }
}

static void choose(struct ui_palette *p, int index)
{
    const struct ui_list_item *item = ui_list_item(&p->list, index);
    if (!item)
        return;
    ui_widget_notify(&p->widget, (struct ui_action){
        .kind = UI_ACTION_ACTIVATE, .id = item->action, .index = index,
    });
    ui_palette_close(p);
}

static bool input_action(struct ui_widget *source, const struct ui_action *action,
                         void *userdata)
{
    (void)source;
    struct ui_palette *p = userdata;
    if (action->kind == UI_ACTION_CHANGE) {
        p->impl.stale = true;
        ui_widget_invalidate(&p->widget, UI_WIDGET_INVALIDATE_MEASURE);
    }
    return true;
}

static bool list_action(struct ui_widget *source, const struct ui_action *action,
                        void *userdata)
{
    (void)source;
    if (action->kind == UI_ACTION_ACTIVATE)
        choose(userdata, action->index);
    return true;
}

static void step(struct ui_palette *p, int dir, int count)
{
    for (int i = 0; i < count; i++)
        ui_list_step(&p->list, dir);
}

static bool palette_key(struct ui_palette *p, const struct ui_key_event *key)
{
    switch (key->key) {
    case UI_KEY_ESCAPE:
        if (key->action == UI_KEY_PRESS)
            ui_palette_close(p);
        return true;
    case UI_KEY_UP:
    case UI_KEY_DOWN:
        step(p, key->key == UI_KEY_DOWN ? 1 : -1, 1);
        return true;
    case UI_KEY_PAGE_UP:
    case UI_KEY_PAGE_DOWN:
        step(p, key->key == UI_KEY_PAGE_DOWN ? 1 : -1, UI_PALETTE_MAX_ROWS);
        return true;
    case UI_KEY_TAB:
        if (key->mods & UI_MOD_SHIFT)
            step(p, -1, 1);
        return true;
    case UI_KEY_ENTER:
        if (key->action == UI_KEY_PRESS)
            choose(p, ui_list_selected(&p->list));
        return true;
    case UI_KEY_CHAR:
        if ((key->mods & UI_MOD_CTRL) && (key->text == 'c' || key->text == 'C')) {
            ui_palette_close(p);
            return true;
        }
        return false;
    default:
        return false;
    }
}

static bool palette_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_palette *p = palette_of(widget);
    if (event->kind == UI_EVENT_KEY) {
        if (event->data.key.action == UI_KEY_RELEASE)
            return false;
        return palette_key(p, &event->data.key);
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct pointer_event *ptr = &event->data.pointer;
    if (pointer_is_button_press(ptr) &&
        !rect2i_contains(widget->bounds, ptr->pos))
        ui_palette_close(p);
    return true;
}

static void palette_style_changed(struct ui_widget *widget)
{
    struct ui_palette *p = palette_of(widget);
    ui_widget_set_placement(widget, (struct ui_placement){
        .kind = UI_PLACE_CENTER, .y_fraction = 0.125f, .gap = cell_h(p),
    });
}

static int panel_width(const struct ui_palette *p, int parent_width)
{
    int margin = cell_h(p);
    int width = parent_width / 2;
    int min_w = 44 * cell_w(p);
    int max_w = parent_width - 2 * margin;
    if (max_w < 8 * cell_w(p))
        max_w = parent_width;
    if (width < min_w)
        width = min_w;
    if (width > max_w)
        width = max_w;
    return width > 1 ? width : 1;
}

static int empty_height(const struct ui_palette *p)
{
    return cell_h(p) + 2 * ui_ctx_px(scale_of(p), 6);
}

static struct vector2i palette_measure(const struct ui_widget *widget,
                                       struct ui_constraints constraints)
{
    struct ui_palette *p = palette_of(widget);
    int width = panel_width(p, constraints.max.x);
    int inner = width - 2 * border_width(p);
    struct ui_constraints child = { .max = { .x = inner, .y = constraints.max.y } };
    int height = 2 * border_width(p) + separator_height(p) +
                 ui_widget_measure(&p->input.widget, child).y;
    if (!ui_widget_is_hidden(&p->list.widget))
        height += ui_widget_measure(&p->list.widget, child).y;
    if (!ui_widget_is_hidden(&p->empty.widget)) {
        ui_widget_measure(&p->empty.widget, child);
        height += empty_height(p);
    }
    return ui_constraints_clamp(constraints,
                                (struct vector2i){ .x = width, .y = height });
}

static void palette_layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct ui_palette *p = palette_of(widget);
    struct rect2i inner = rect2i_inset(bounds, border_width(p));
    int offset = ui_list_label_inset(&p->list) - ui_input_text_inset(&p->input);
    if (offset < 0)
        offset = 0;
    int input_h = p->input.widget.measured_size.y;
    ui_widget_place(&p->input.widget, (struct rect2i){
        .pos = { .x = inner.x + offset, .y = inner.y },
        .size = { .x = inner.width - offset, .y = input_h },
    });
    int y = inner.y + input_h + separator_height(p);
    ui_widget_place(&p->list.widget, (struct rect2i){
        .pos = { .x = inner.x, .y = y },
        .size = { .x = inner.width, .y = p->list.widget.measured_size.y },
    });
    ui_widget_place(&p->empty.widget, (struct rect2i){
        .pos = { .x = inner.x + ui_list_label_inset(&p->list),
                 .y = y + (empty_height(p) - cell_h(p)) / 2 },
        .size = p->empty.widget.measured_size,
    });

    struct ui_surface *surface = ui_widget_surface(widget);
    int rows = fitting_rows(p, surface ? ui_surface_viewport(surface).y
                                       : bounds.height * 2);
    if (rows != p->impl.max_rows) {
        p->impl.max_rows = rows;
        p->impl.stale = true;
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_MEASURE);
    }
}

static void palette_paint(const struct ui_widget *widget,
                          struct ui_paint_context *paint)
{
    const struct ui_palette *p = palette_of(widget);
    const struct css_style *s = &widget->style;
    const struct css_scale_context *scale = scale_of(p);
    struct rect2i box = ui_paint_bounds(paint);
    int radius = ui_css_resolve_int(s->border_radius, 0, 0, scale, true);
    int border = border_width(p);
    if (border > 0) {
        if (radius > 0)
            ui_paint_rounded_rect(paint, box, radius, s->border_color);
        else
            ui_paint_rect(paint, box, s->border_color);
    }
    struct rect2i fill = border > 0 ? rect2i_inset(box, border) : box;
    if (rect2i_is_empty(fill))
        return;
    uint32_t bg = s->has_bg_color ? s->bg_color : 0xFF1E1E2Eu;
    int fill_radius = radius > border ? radius - border : 0;
    if (fill_radius > 0)
        ui_paint_rounded_rect(paint, fill, fill_radius, bg);
    else
        ui_paint_rect(paint, fill, bg);
    ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = fill.x,
                 .y = fill.y + p->input.widget.measured_size.y },
        .size = { .x = fill.width, .y = separator_height(p) },
    }, s->has_border_color ? s->border_color : 0xFF45475Au);
}

static void palette_destroy(struct ui_widget *widget)
{
    free_cmds(palette_of(widget));
}

static const struct ui_widget_ops PALETTE_OPS = {
    .name = "palette",
    .prepare = palette_prepare,
    .measure = palette_measure,
    .layout = palette_layout,
    .event = palette_event,
    .paint = palette_paint,
    .style_changed = palette_style_changed,
    .destroy = palette_destroy,
};

void ui_palette_init(struct ui_palette *p)
{
    if (!p)
        return;
    *p = (struct ui_palette){0};
    ui_widget_init(&p->widget, &PALETTE_OPS, NULL);
    ui_widget_set_style_identity(&p->widget, "context-menu command-palette",
                                 NULL, NULL);
    ui_widget_set_modal(&p->widget, true);
    ui_widget_set_hidden(&p->widget, true);

    ui_input_init(&p->input);
    ui_input_set_placeholder(&p->input, "Type a command");
    ui_widget_set_accessible_name(&p->input.widget, "Search commands");
    ui_widget_set_action_handler(&p->input.widget, input_action, p);

    ui_list_init(&p->list);
    ui_list_set_style_class(&p->list, "palette-list");
    ui_list_set_focusable(&p->list, false);
    ui_widget_set_action_handler(&p->list.widget, list_action, p);

    ui_label_init(&p->empty);
    ui_label_set_text(&p->empty, PALETTE_EMPTY_TEXT);
    ui_widget_set_style_identity(&p->empty.widget, "palette-empty", NULL,
                                 "label");
    ui_widget_set_hidden(&p->empty.widget, true);

    ui_widget_add_child(&p->widget, &p->input.widget);
    ui_widget_add_child(&p->widget, &p->list.widget);
    ui_widget_add_child(&p->widget, &p->empty.widget);
}

bool ui_palette_open(struct ui_palette *p, const struct ui_palette_cmd *cmds,
                     int n)
{
    if (!p || !cmds || n <= 0)
        return false;
    free_cmds(p);
    p->impl.cmds = calloc((size_t)n, sizeof(*p->impl.cmds));
    if (!p->impl.cmds)
        return false;
    for (int i = 0; i < n; i++) {
        if (!cmds[i].label)
            continue;
        struct ui_palette_cmd *c = &p->impl.cmds[p->impl.n_cmds++];
        c->action = cmds[i].action;
        c->label = wsap_vtcore_utf8_sanitize_dup(cmds[i].label);
        if (cmds[i].shortcut && cmds[i].shortcut[0])
            c->shortcut = wsap_vtcore_utf8_sanitize_dup(cmds[i].shortcut);
        if (!c->label) {
            free_cmds(p);
            return false;
        }
    }
    ui_input_clear(&p->input);
    p->impl.stale = true;
    ui_widget_set_hidden(&p->widget, false);
    return true;
}

void ui_palette_close(struct ui_palette *p)
{
    if (!ui_palette_is_open(p))
        return;
    ui_widget_set_hidden(&p->widget, true);
    ui_widget_notify(&p->widget, (struct ui_action){
        .kind = UI_ACTION_CLOSE, .id = -1, .index = -1,
    });
}

bool ui_palette_is_open(const struct ui_palette *p)
{
    return p && !ui_widget_is_hidden(&p->widget);
}

const char *ui_palette_query(const struct ui_palette *p)
{
    return p ? ui_input_text(&p->input) : "";
}
