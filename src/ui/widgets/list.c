#include "ui/widgets/list.h"

#include "ui/debug.h"
#include "ui/paint.h"
#include "paint/shadow.h"
#include "ui/surface.h"
#include "paint/text_run.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const ITEM_CLASSES[4] = {
    "menu-item",
    "menu-item submenu",
    "menu-item disabled",
    "menu-item submenu disabled",
};

static struct {
    ui_part_id item[4];
    ui_part_id separator;
    ui_part_id label;
    ui_part_id shortcut;
} parts;

static void list_parts_init(void)
{
    if (parts.item[0])
        return;
    for (int i = 0; i < 4; i++)
        parts.item[i] = ui_part_intern(ITEM_CLASSES[i]);
    parts.separator = ui_part_intern("menu-separator");
    parts.label = ui_part_intern("menu-label");
    parts.shortcut = ui_part_intern("menu-shortcut");
}

static ui_part_id item_part(const struct ui_list_item *it)
{
    unsigned variant = ((it->flags & UI_LIST_ITEM_SUBMENU) ? 1u : 0u) |
                       ((it->flags & UI_LIST_ITEM_DISABLED) ? 2u : 0u);
    return parts.item[variant];
}

static struct ui_list *list_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_list, widget);
}

static bool selectable(const struct ui_list_item *it)
{
    return it->label &&
           !(it->flags & (UI_LIST_ITEM_DISABLED | UI_LIST_ITEM_SEPARATOR));
}

static bool has_selectable_item(const struct ui_list *l)
{
    for (int i = 0; i < l->impl.n; i++)
        if (selectable(&l->impl.items[i]))
            return true;
    return false;
}

static char *shortcut_text(const struct ui_list_item *it)
{
    const char *check = (it->flags & UI_LIST_ITEM_CHECKED) ? "\xe2\x9c\x93" : "";
    const char *arrow = (it->flags & UI_LIST_ITEM_SUBMENU) ? "\xe2\x80\xba" : "";
    char *keys = wsap_vtcore_utf8_sanitize_dup(it->shortcut ? it->shortcut : "");
    if (!keys)
        return NULL;
    const char *parts[] = { check, keys, arrow };
    size_t len = 0;
    for (size_t i = 0; i < 3; i++)
        if (parts[i][0])
            len += strlen(parts[i]) + 1;
    char *out = len ? malloc(len) : NULL;
    if (out) {
        out[0] = '\0';
        for (size_t i = 0; i < 3; i++) {
            if (!parts[i][0])
                continue;
            if (out[0])
                strcat(out, " ");
            strcat(out, parts[i]);
        }
    }
    free(keys);
    return out;
}

static void free_items(struct ui_list *l)
{
    for (int i = 0; i < l->impl.n; i++) {
        free((char *)l->impl.items[i].label);
        free((char *)l->impl.items[i].shortcut);
    }
    free(l->impl.items);
    l->impl.items = NULL;
}

static void free_rows(struct ui_list *l)
{
    for (int i = 0; i < l->impl.n; i++)
        ui_widget_destroy(&l->impl.rows[i].widget);
    free(l->impl.rows);
    l->impl.rows = NULL;
}

static int edge_or(struct css_value v, int fallback,
                   const struct css_scale_context *scale)
{
    return v.unit == CSS_UNIT_AUTO || v.unit == CSS_UNIT_NONE
        ? fallback : ui_css_resolve_int(v, 0, fallback, scale, true);
}

static struct insets2i insets_or(struct css_box box, int vertical,
                                 int horizontal,
                                 const struct css_scale_context *scale)
{
    return (struct insets2i){
        .t = edge_or(box.top, ui_ctx_px(scale, vertical), scale),
        .r = edge_or(box.right, ui_ctx_px(scale, horizontal), scale),
        .b = edge_or(box.bottom, ui_ctx_px(scale, vertical), scale),
        .l = edge_or(box.left, ui_ctx_px(scale, horizontal), scale),
    };
}

static struct ui_list_metrics compute_metrics(const struct ui_list *l)
{
    const struct ui_widget *w = &l->widget;
    const struct css_scale_context *scale = &w->style_scale;
    struct ui_text_style text = ui_widget_text_style(scale);
    struct css_style item = ui_widget_part(w, parts.item[0], 0);
    struct css_style sep = ui_widget_part(w, parts.separator, 0);
    int radius = ui_css_resolve_int(w->style.border_radius, 0, 0, scale, true);
    int min_pad = ui_ctx_px(scale, 4);
    int thickness = edge_or(sep.height, ui_ctx_px(scale, 1), scale);
    struct ui_list_metrics m = {
        .scale = *scale,
        .item_pad = insets_or(item.padding, 3, 8, scale),
        .sep_margin = insets_or(sep.margin, 4, 6, scale),
        .cell_w = text.advance,
        .cell_h = text.cell_h,
        .min_h = edge_or(item.min_height, 0, scale),
        .gap = edge_or(item.gap, 2 * text.advance, scale),
        .sep_thickness = thickness > 0 ? thickness : 1,
        .edge_pad = radius > min_pad ? radius : min_pad,
        .valid = true,
    };
    if (w->style.has_shadow) {
        int sx = ui_css_resolve_int(w->style.shadow_x, 0, 0, scale, true);
        int sy = ui_css_resolve_int(w->style.shadow_y, 0, 0, scale, true);
        int extent = ui_shadow_extent(
            ui_css_resolve_int(w->style.shadow_blur, 0, 0, scale, true));
        m.shadow = (struct insets2i){
            .l = extent - sx > 0 ? extent - sx : 0,
            .r = extent + sx > 0 ? extent + sx : 0,
            .t = extent - sy > 0 ? extent - sy : 0,
            .b = extent + sy > 0 ? extent + sy : 0,
        };
    }
    return m;
}

static bool same_scale(struct css_scale_context a, struct css_scale_context b)
{
    return a.dpi_scale == b.dpi_scale && a.cell_w == b.cell_w &&
           a.cell_h == b.cell_h;
}

static void list_style_changed(struct ui_widget *widget)
{
    struct ui_list *l = list_of(widget);
    if (l->impl.pinned && l->impl.metrics.valid &&
        same_scale(l->impl.metrics.scale, widget->style_scale))
        return;
    l->impl.metrics = compute_metrics(l);
}

static struct vector2i row_size(const struct ui_list *l, int i)
{
    const struct ui_list_metrics *m = &l->impl.metrics;
    const struct ui_list_item *it = &l->impl.items[i];
    if (it->flags & UI_LIST_ITEM_SEPARATOR)
        return (struct vector2i){
            .x = m->sep_margin.l + m->sep_margin.r,
            .y = m->sep_margin.t + m->sep_thickness + m->sep_margin.b,
        };
    int h = m->cell_h + m->item_pad.t + m->item_pad.b;
    int w = m->item_pad.l + m->item_pad.r +
            ui_text_run_cells(it->label, strlen(it->label)) * m->cell_w;
    if (it->shortcut)
        w += m->gap + ui_text_run_cells(it->shortcut, strlen(it->shortcut)) *
                      m->cell_w;
    return (struct vector2i){ .x = w, .y = h > m->min_h ? h : m->min_h };
}

static struct vector2i list_measure(const struct ui_widget *widget,
                                    struct ui_constraints constraints)
{
    const struct ui_list *l = list_of(widget);
    int pad = l->impl.metrics.edge_pad;
    struct vector2i size = { .x = 2 * pad, .y = 2 * pad };
    int width = 0;
    for (int i = 0; i < l->impl.n; i++) {
        struct vector2i row = row_size(l, i);
        if (row.x > width)
            width = row.x;
        size.y += row.y;
    }
    size.x += width;
    return ui_constraints_clamp(constraints, size);
}

static void list_layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct ui_list *l = list_of(widget);
    int pad = l->impl.metrics.edge_pad;
    int y = bounds.y + pad;
    for (int i = 0; i < l->impl.n; i++) {
        int h = row_size(l, i).y;
        ui_widget_place(&l->impl.rows[i].widget, (struct rect2i){
            .pos = { .x = bounds.x + pad, .y = y },
            .size = { .x = bounds.width - 2 * pad, .y = h },
        });
        y += h;
    }
}

static void paint_box(struct ui_paint_context *paint, struct rect2i box,
                      const struct css_style *style,
                      const struct css_scale_context *scale, int max_radius)
{
    int border = style->has_border_color
        ? ui_css_resolve_int(style->border_width, 0, 0, scale, true) : 0;
    int radius = ui_css_resolve_int(style->border_radius, 0, 0, scale, true);
    if (max_radius >= 0 && radius > max_radius)
        radius = max_radius;
    float opacity = css_style_effective_opacity(style);
    if (style->has_border_color && border > 0) {
        uint32_t color = ui_color_opacity(style->border_color, opacity);
        if (radius > 0)
            ui_paint_rounded_rect(paint, box, radius, color);
        else
            ui_paint_rect(paint, box, color);
    }
    if (!style->has_bg_color)
        return;
    struct rect2i fill = border > 0 ? rect2i_inset(box, border) : box;
    int fill_radius = radius > border ? radius - border : 0;
    uint32_t bg = ui_color_opacity(style->bg_color, opacity);
    if (rect2i_is_empty(fill))
        return;
    if (fill_radius > 0)
        ui_paint_rounded_rect(paint, fill, fill_radius, bg);
    else
        ui_paint_rect(paint, fill, bg);
}

static void list_paint(const struct ui_widget *widget,
                       struct ui_paint_context *paint)
{
    const struct ui_list *l = list_of(widget);
    const struct css_style *style = &widget->style;
    const struct css_scale_context *scale = &widget->style_scale;
    struct rect2i bounds = ui_paint_bounds(paint);
    int max_radius = l->impl.metrics.edge_pad;
    if (style->has_shadow && !insets2i_eq(l->impl.metrics.shadow,
                                          (struct insets2i){0})) {
        int sx = ui_css_resolve_int(style->shadow_x, 0, 0, scale, true);
        int sy = ui_css_resolve_int(style->shadow_y, 0, 0, scale, true);
        int blur = ui_css_resolve_int(style->shadow_blur, 0, 0, scale, true);
        int radius = ui_css_resolve_int(style->border_radius, 0, 0, scale, true);
        ui_paint_shadow(paint,
                        rect2i_translate(bounds, (struct vector2i){ .x = sx, .y = sy }),
                        radius < max_radius ? radius : max_radius, blur,
                        ui_color_opacity(style->shadow_color,
                                         css_style_effective_opacity(style)));
    }
    paint_box(paint, bounds, style, scale, max_radius);
}

static const struct ui_list_row *row_of(const struct ui_widget *widget)
{
    return ui_container_of_const(widget, struct ui_list_row, widget);
}

static bool activate(struct ui_list *l, int index);

static void row_paint(const struct ui_widget *widget,
                      struct ui_paint_context *paint)
{
    const struct ui_list_row *row = row_of(widget);
    const struct ui_list *l = row->list;
    const struct ui_list_item *it = &l->impl.items[row->index];
    const struct ui_list_metrics *m = &l->impl.metrics;
    const struct css_scale_context *scale = &l->widget.style_scale;
    struct rect2i box = ui_paint_bounds(paint);
    if (it->flags & UI_LIST_ITEM_SEPARATOR) {
        struct css_style sep = ui_widget_part(&l->widget, parts.separator, 0);
        struct rect2i line = rect2i_apply_insets(box, m->sep_margin);
        line.height = m->sep_thickness;
        if (sep.has_bg_color && !rect2i_is_empty(line))
            ui_paint_rect(paint, line, ui_color_opacity(sep.bg_color, sep.opacity));
        return;
    }
    unsigned pseudo = 0;
    if (l->impl.hovered == row->index) pseudo |= CSS_PSEUDO_HOVER;
    if (l->impl.selected == row->index) pseudo |= CSS_PSEUDO_FOCUS;
    if (it->flags & UI_LIST_ITEM_DISABLED) pseudo |= CSS_PSEUDO_DISABLED;
    if (it->flags & UI_LIST_ITEM_CHECKED) pseudo |= CSS_PSEUDO_CHECKED;
    struct css_style item = ui_widget_part(&l->widget, item_part(it), pseudo);
    paint_box(paint, box, &item, scale, -1);

    struct ui_text_style text = { .advance = m->cell_w, .cell_h = m->cell_h };
    int text_y = box.y + (box.height - m->cell_h) / 2;
    struct css_style label = ui_widget_part(&l->widget, parts.label, pseudo);
    uint32_t fg = label.has_text_color ? label.text_color
                : item.has_text_color ? item.text_color : 0xFFFFFFFF;
    ui_paint_text(paint, (struct rect2i){
        .pos = { .x = box.x + m->item_pad.l, .y = text_y },
        .size = { .x = ui_text_run_cells(it->label, strlen(it->label)) * m->cell_w,
                  .y = m->cell_h },
    }, text, fg, it->label, strlen(it->label));
    if (!it->shortcut || !it->shortcut[0])
        return;
    struct css_style sc = ui_widget_part(&l->widget, parts.shortcut, pseudo);
    int width = ui_text_run_cells(it->shortcut, strlen(it->shortcut)) * m->cell_w;
    ui_paint_text(paint, (struct rect2i){
        .pos = { .x = box.x + box.width - m->item_pad.r - width, .y = text_y },
        .size = { .x = width, .y = m->cell_h },
    }, text, sc.has_text_color ? sc.text_color : fg,
       it->shortcut, strlen(it->shortcut));
}

static void row_debug_parts(const struct ui_widget *widget,
                            struct ui_debug_parts *out)
{
    const struct ui_list_row *row = row_of(widget);
    const struct ui_list_item *it = &row->list->impl.items[row->index];
    ui_debug_part_text(out,
                       (it->flags & UI_LIST_ITEM_SEPARATOR) ? "separator" : "item",
                       row->index, widget->bounds, it->label,
                       it->label ? strlen(it->label) : 0);
}

static bool row_accessibility(const struct ui_widget *widget,
                              struct ui_accessibility_node_spec *out)
{
    const struct ui_list_row *row = row_of(widget);
    const struct ui_list *list = row->list;
    if (row->index < 0 || row->index >= list->impl.n)
        return false;
    const struct ui_list_item *item = &list->impl.items[row->index];
    if (!item->label || (item->flags & UI_LIST_ITEM_SEPARATOR))
        return false;
    out->role = UI_ACCESSIBILITY_ROLE_LIST_ITEM;
    out->name = item->label;
    out->description = item->shortcut && item->shortcut[0]
        ? item->shortcut : NULL;
    if (row->index == list->impl.selected) {
        out->states |= UI_ACCESSIBILITY_STATE_SELECTED;
        if (ui_widget_has_focus(&list->widget))
            out->states |= UI_ACCESSIBILITY_STATE_FOCUSED;
    }
    if (item->flags & UI_LIST_ITEM_CHECKED)
        out->states |= UI_ACCESSIBILITY_STATE_CHECKED;
    if (item->flags & UI_LIST_ITEM_DISABLED)
        out->states |= UI_ACCESSIBILITY_STATE_DISABLED;
    if (selectable(item)) {
        out->states |= UI_ACCESSIBILITY_STATE_FOCUSABLE;
        out->actions |= UI_ACCESSIBILITY_ACTION_FOCUS |
                        UI_ACCESSIBILITY_ACTION_INVOKE;
    }
    return true;
}

static bool row_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_list_row *row = ui_container_of(widget, struct ui_list_row,
                                              widget);
    struct ui_list *list = row->list;
    int index = row->index;
    if (node_id != ui_widget_accessibility_id(widget) ||
        index < 0 || index >= list->impl.n ||
        !selectable(&list->impl.items[index]))
        return false;
    if (request->action == UI_ACCESSIBILITY_ACTION_FOCUS) {
        ui_widget_focus(&list->widget);
        if (!list->widget.impl.destroyed)
            ui_list_select(list, index);
        return true;
    }
    if (request->action == UI_ACCESSIBILITY_ACTION_INVOKE)
        return activate(list, index);
    return false;
}

static const struct ui_widget_ops ROW_OPS = {
    .name = "list-row",
    .paint = row_paint,
    .debug_parts = row_debug_parts,
    .accessibility = row_accessibility,
    .accessibility_action = row_accessibility_action,
};

static void notify(struct ui_list *l, enum ui_action_kind kind, int index)
{
    ui_widget_notify(&l->widget, (struct ui_action){
        .kind = kind,
        .id = index >= 0 ? l->impl.items[index].action : -1,
        .index = index,
    });
}

static int row_at(const struct ui_list *l, struct vector2i pos)
{
    for (int i = 0; i < l->impl.n; i++)
        if (rect2i_contains(l->impl.rows[i].widget.bounds, pos))
            return i;
    return -1;
}

static void set_hovered(struct ui_list *l, int index)
{
    if (l->impl.hovered == index)
        return;
    l->impl.hovered = index;
    ui_widget_invalidate(&l->widget, UI_WIDGET_INVALIDATE_PAINT);
    notify(l, UI_ACTION_HIGHLIGHT, index);
}

void ui_list_select_edge(struct ui_list *l, int dir)
{
    if (!l || !l->impl.n || !dir)
        return;
    dir = dir > 0 ? 1 : -1;
    int start = dir > 0 ? 0 : l->impl.n - 1;
    for (int i = start; i >= 0 && i < l->impl.n; i += dir)
        if (selectable(&l->impl.items[i])) {
            ui_list_select(l, i);
            return;
        }
}

static bool activate(struct ui_list *l, int index)
{
    if (index < 0 || !selectable(&l->impl.items[index]))
        return false;
    ui_list_select(l, index);
    if (!l->widget.impl.destroyed)
        notify(l, UI_ACTION_ACTIVATE, index);
    return true;
}

static void step_without_wrap(struct ui_list *l, int dir)
{
    int index = l->impl.selected;
    if (index < 0)
        index = dir > 0 ? -1 : l->impl.n;
    for (index += dir; index >= 0 && index < l->impl.n; index += dir)
        if (selectable(&l->impl.items[index])) {
            ui_list_select(l, index);
            return;
        }
}

static bool list_key(struct ui_list *l, const struct ui_key_event *key)
{
    switch (key->key) {
    case UI_KEY_UP:
    case UI_KEY_DOWN:
        if (key->action == UI_KEY_RELEASE)
            return true;
        if (l->impl.stop_at_edges)
            step_without_wrap(l, key->key == UI_KEY_DOWN ? 1 : -1);
        else
            ui_list_step(l, key->key == UI_KEY_DOWN ? 1 : -1);
        return true;
    case UI_KEY_HOME:
    case UI_KEY_END:
        ui_list_select_edge(l, key->key == UI_KEY_HOME ? 1 : -1);
        return true;
    case UI_KEY_ENTER: {
        if (key->action != UI_KEY_PRESS)
            return true;
        int index = l->impl.selected;
        if (index < 0 ||
            (l->impl.items[index].flags & UI_LIST_ITEM_SUBMENU))
            return false;
        return activate(l, index);
    }
    default:
        return false;
    }
}

static bool list_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_list *l = list_of(widget);
    if (event->kind == UI_EVENT_KEY)
        return list_key(l, &event->data.key);
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct pointer_event *p = &event->data.pointer;
    switch (p->kind) {
    case PTR_MOTION:
        set_hovered(l, row_at(l, p->pos));
        return false;
    case PTR_LEAVE:
        set_hovered(l, -1);
        return true;
    case PTR_PRESS:
        if (p->button != 0)
            return false;
        set_hovered(l, row_at(l, p->pos));
        if (l->impl.hovered >= 0 &&
            selectable(&l->impl.items[l->impl.hovered])) {
            if (l->impl.allow_focus)
                ui_widget_focus(widget);
        } else {
            struct ui_surface *surface = ui_widget_surface(widget);
            if (surface && !ui_surface_has_modal(surface))
                ui_surface_focus(surface, NULL);
        }
        return l->impl.hovered >= 0;
    case PTR_RELEASE:
        return p->button == 0 && activate(l, l->impl.hovered);
    default:
        return false;
    }
}

static enum ui_cursor list_cursor(const struct ui_widget *widget,
                                  struct vector2i pos)
{
    const struct ui_list *l = list_of(widget);
    (void)pos;
    int index = l->impl.hovered;
    if (index < 0 || index >= l->impl.n)
        return UI_CURSOR_AUTO;
    struct css_style style = ui_widget_part(widget, parts.item[0],
                                            CSS_PSEUDO_HOVER);
    return style.has_cursor ? style.cursor : UI_CURSOR_POINTER;
}

static void list_destroy(struct ui_widget *widget)
{
    struct ui_list *l = list_of(widget);
    free_rows(l);
    free_items(l);
    l->impl.n = 0;
}

static bool list_accessibility(const struct ui_widget *widget,
                               struct ui_accessibility_node_spec *out)
{
    (void)widget;
    out->role = UI_ACCESSIBILITY_ROLE_LIST;
    return true;
}

static const struct ui_widget_ops LIST_OPS = {
    .name = "list",
    .measure = list_measure,
    .layout = list_layout,
    .event = list_event,
    .cursor = list_cursor,
    .paint = list_paint,
    .style_changed = list_style_changed,
    .destroy = list_destroy,
    .accessibility = list_accessibility,
    .clip_children = true,
};

void ui_list_init(struct ui_list *l)
{
    if (!l)
        return;
    list_parts_init();
    *l = (struct ui_list){0};
    l->impl.hovered = -1;
    l->impl.selected = -1;
    l->impl.allow_focus = true;
    ui_widget_init(&l->widget, &LIST_OPS, NULL);
    ui_widget_set_focusable(&l->widget, false);
    ui_list_set_style_class(l, NULL);
}

void ui_list_set_focusable(struct ui_list *l, bool focusable)
{
    if (!l)
        return;
    l->impl.allow_focus = focusable;
    ui_widget_set_focusable(&l->widget, focusable && has_selectable_item(l));
}

void ui_list_set_style_class(struct ui_list *l, const char *class_name)
{
    if (!l)
        return;
    snprintf(l->impl.style_class, sizeof(l->impl.style_class), "%s%smenu-list",
             class_name ? class_name : "", class_name ? " " : "");
    ui_widget_set_style_identity(&l->widget, l->impl.style_class, NULL, NULL);
}

void ui_list_set_wrap_navigation(struct ui_list *l, bool wrap)
{
    if (!l)
        return;
    l->impl.stop_at_edges = !wrap;
}

bool ui_list_set_items(struct ui_list *l, const struct ui_list_item *items,
                       int n)
{
    if (!l)
        return false;
    free_rows(l);
    free_items(l);
    l->impl.n = 0;
    l->impl.hovered = -1;
    l->impl.selected = -1;
    ui_widget_invalidate(&l->widget, UI_WIDGET_INVALIDATE_MEASURE);
    if (!items || n <= 0) {
        ui_widget_set_focusable(&l->widget, false);
        return true;
    }
    l->impl.items = calloc((size_t)n, sizeof(*l->impl.items));
    l->impl.rows = calloc((size_t)n, sizeof(*l->impl.rows));
    if (!l->impl.items || !l->impl.rows) {
        free(l->impl.items);
        free(l->impl.rows);
        l->impl.items = NULL;
        l->impl.rows = NULL;
        ui_widget_set_focusable(&l->widget, false);
        return false;
    }
    for (int i = 0; i < n; i++) {
        struct ui_list_item *it = &l->impl.items[i];
        *it = (struct ui_list_item){
            .action = items[i].action,
            .flags = items[i].flags,
        };
        if (items[i].label)
            it->label = wsap_vtcore_utf8_sanitize_dup(items[i].label);
        if (!it->label)
            it->flags |= UI_LIST_ITEM_SEPARATOR;
        else if (!(it->flags & UI_LIST_ITEM_SEPARATOR))
            it->shortcut = shortcut_text(&(struct ui_list_item){
                .shortcut = items[i].shortcut, .flags = it->flags });
        struct ui_list_row *row = &l->impl.rows[i];
        row->list = l;
        row->index = i;
        ui_widget_init(&row->widget, &ROW_OPS, NULL);
        ui_widget_add_child(&l->widget, &row->widget);
        l->impl.n = i + 1;
    }
    ui_widget_set_focusable(&l->widget, l->impl.allow_focus && has_selectable_item(l));
    return true;
}

void ui_list_pin_metrics(struct ui_list *l, const struct ui_list *like)
{
    if (!l)
        return;
    if (like && like->impl.metrics.valid)
        l->impl.metrics = like->impl.metrics;
    l->impl.pinned = true;
    ui_widget_invalidate(&l->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

int ui_list_count(const struct ui_list *l)
{
    return l ? l->impl.n : 0;
}

const struct ui_list_item *ui_list_item(const struct ui_list *l, int index)
{
    return l && index >= 0 && index < l->impl.n ? &l->impl.items[index] : NULL;
}

int ui_list_selected(const struct ui_list *l)
{
    return l ? l->impl.selected : -1;
}

int ui_list_hovered(const struct ui_list *l)
{
    return l ? l->impl.hovered : -1;
}

void ui_list_select(struct ui_list *l, int index)
{
    if (!l)
        return;
    if (index < 0 || index >= l->impl.n || !selectable(&l->impl.items[index]))
        index = -1;
    if (l->impl.selected == index)
        return;
    l->impl.selected = index;
    ui_widget_invalidate(&l->widget, UI_WIDGET_INVALIDATE_PAINT);
    notify(l, UI_ACTION_CHANGE, index);
}

int ui_list_step(struct ui_list *l, int dir)
{
    if (!l || l->impl.n <= 0 || !dir)
        return -1;
    dir = dir > 0 ? 1 : -1;
    int i = l->impl.selected;
    if (i < 0)
        i = dir > 0 ? -1 : 0;
    for (int k = 0; k < l->impl.n; k++) {
        i = (i + dir + l->impl.n) % l->impl.n;
        if (selectable(&l->impl.items[i])) {
            ui_list_select(l, i);
            return i;
        }
    }
    return -1;
}

struct rect2i ui_list_item_rect(const struct ui_list *l, int index)
{
    return l && index >= 0 && index < l->impl.n
        ? l->impl.rows[index].widget.bounds : (struct rect2i){0};
}

int ui_list_label_inset(const struct ui_list *l)
{
    return l ? l->impl.metrics.edge_pad + l->impl.metrics.item_pad.l : 0;
}

struct insets2i ui_list_shadow(const struct ui_list *l)
{
    return l ? l->impl.metrics.shadow : (struct insets2i){0};
}
