#define _GNU_SOURCE
#include "ui/widgets/dropdown.h"

#include "ui/surface.h"
#include "ui/widget_impl.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_items(struct ui_dropdown_item *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free((char *)items[i].label);
        free((char *)items[i].value);
    }
    free(items);
}

static void show_selection(struct ui_dropdown *d)
{
    const char *text = d->impl.selected >= 0
        ? d->impl.items[d->impl.selected].label : "Choose";
    char label[UI_BUTTON_TEXT_CAP];
    snprintf(label, sizeof(label), "%s  v", text);
    ui_button_set_text(&d->button, label);
}

static bool filter(struct ui_dropdown *d)
{
    struct ui_list_item *items = calloc(d->impl.count ? d->impl.count : 1,
                                        sizeof(*items));
    if (!items)
        return false;
    const char *query = ui_input_text(&d->search);
    int count = 0, selected = -1;
    for (size_t i = 0; i < d->impl.count; i++) {
        if (!strcasestr(d->impl.items[i].label, query) &&
            !strcasestr(d->impl.items[i].value, query))
            continue;
        if ((int)i == d->impl.selected) selected = count;
        items[count++] = (struct ui_list_item){
            .label = d->impl.items[i].label, .action = (int)i,
            .flags = (int)i == d->impl.selected ? UI_LIST_ITEM_CHECKED : 0,
        };
    }
    bool ok = ui_list_set_items(&d->list, items, count);
    free(items);
    if (!ok)
        return false;
    ui_list_select(&d->list, selected >= 0 ? selected : count ? 0 : -1);
    ui_widget_set_hidden(ui_label_widget(&d->empty), count != 0);
    ui_scrollview_scroll_to(&d->scroll, 0);
    return true;
}

static void reveal_selection(struct ui_dropdown *d)
{
    struct ui_surface *surface = ui_widget_surface(&d->layer);
    if (!surface)
        return;
    ui_surface_prepare(surface);
    struct rect2i row = ui_list_item_rect(&d->list, ui_list_selected(&d->list));
    struct rect2i view = d->scroll.widget.bounds;
    int dy = row.y < view.y ? row.y - view.y :
        row.y + row.height > view.y + view.height
        ? row.y + row.height - view.y - view.height : 0;
    ui_scrollview_scroll_by(&d->scroll, dy);
}

static void step_page(struct ui_dropdown *d, int dir)
{
    int count = ui_list_count(&d->list);
    if (!count)
        return;
    int selected = ui_list_selected(&d->list);
    if (selected < 0)
        selected = dir > 0 ? -1 : count;
    struct rect2i row = ui_list_item_rect(&d->list, selected >= 0 ? selected : 0);
    int row_height = row.height;
    int page = row_height > 0
        ? d->scroll.widget.bounds.height / row_height : 1;
    if (page < 1)
        page = 1;
    int target = selected + (dir > 0 ? page : -page);
    if (target < 0)
        target = 0;
    if (target >= count)
        target = count - 1;
    for (int i = target; i >= 0 && i < count; i += dir) {
        const struct ui_list_item *item = ui_list_item(&d->list, i);
        if (item && !(item->flags & (UI_LIST_ITEM_DISABLED | UI_LIST_ITEM_SEPARATOR))) {
            ui_list_select(&d->list, i);
            return;
        }
    }
    for (int i = target; i >= 0 && i < count; i -= dir) {
        const struct ui_list_item *item = ui_list_item(&d->list, i);
        if (item && !(item->flags & (UI_LIST_ITEM_DISABLED | UI_LIST_ITEM_SEPARATOR))) {
            ui_list_select(&d->list, i);
            return;
        }
    }
}

static void step_selection(struct ui_dropdown *d, int dir)
{
    int count = ui_list_count(&d->list);
    if (!count)
        return;
    int selected = ui_list_selected(&d->list);
    if (selected < 0)
        selected = dir > 0 ? -1 : count;
    for (int next = selected + dir; next >= 0 && next < count; next += dir) {
        const struct ui_list_item *item = ui_list_item(&d->list, next);
        if (item && !(item->flags & (UI_LIST_ITEM_DISABLED | UI_LIST_ITEM_SEPARATOR))) {
            ui_list_select(&d->list, next);
            return;
        }
    }
}

static void choose(struct ui_dropdown *d, int index)
{
    if (index < 0 || (size_t)index >= d->impl.count)
        return;
    bool changed = d->impl.selected != index;
    d->impl.selected = index;
    show_selection(d);
    ui_dropdown_close(d);
    if (changed)
        ui_widget_notify(&d->widget, (struct ui_action){
            .kind = UI_ACTION_CHANGE, .id = d->impl.action_id,
            .index = index,
        });
}

static bool popup_action(struct ui_widget *source, const struct ui_action *action,
                          void *userdata)
{
    struct ui_dropdown *d = userdata;
    if (source == ui_input_widget(&d->search) && action->kind == UI_ACTION_CHANGE) {
        if (!filter(d))
            ui_dropdown_close(d);
        return true;
    }
    if (source != ui_list_widget(&d->list))
        return false;
    if (action->kind == UI_ACTION_ACTIVATE)
        choose(d, action->id);
    else if (action->kind == UI_ACTION_CHANGE)
        reveal_selection(d);
    return true;
}

static bool layer_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_dropdown *d = widget->userdata;
    if (event->kind == UI_EVENT_POINTER &&
        ui_pointer_is_button_press(&event->data.pointer) &&
        !rect2i_contains(d->panel.widget.bounds, event->data.pointer.pos)) {
        ui_dropdown_close(d);
        return true;
    }
    if (event->kind != UI_EVENT_KEY)
        return false;
    switch (event->data.key.key) {
    case UI_KEY_ESCAPE:
        if (event->data.key.action == UI_KEY_PRESS)
            ui_dropdown_close(d);
        return true;
    case UI_KEY_UP: case UI_KEY_DOWN:
        if (event->data.key.action == UI_KEY_RELEASE)
            return true;
        step_selection(d, event->data.key.key == UI_KEY_UP ? -1 : 1);
        reveal_selection(d);
        return true;
    case UI_KEY_HOME: case UI_KEY_END:
        if (event->data.key.action == UI_KEY_PRESS)
            ui_list_select_edge(&d->list,
                event->data.key.key == UI_KEY_HOME ? 1 : -1);
        reveal_selection(d);
        return true;
    case UI_KEY_PAGE_UP: case UI_KEY_PAGE_DOWN:
        if (event->data.key.action == UI_KEY_RELEASE)
            return true;
        step_page(d, event->data.key.key == UI_KEY_PAGE_DOWN ? 1 : -1);
        reveal_selection(d);
        return true;
    case UI_KEY_ENTER: {
        if (event->data.key.action != UI_KEY_PRESS)
            return true;
        int selected = ui_list_selected(&d->list);
        if (selected >= 0) {
            const struct ui_list_item *item =
                ui_list_item(&d->list, selected);
            if (item && !(item->flags & (UI_LIST_ITEM_DISABLED | UI_LIST_ITEM_SEPARATOR)))
                choose(d, item->action);
        }
        return true;
    }
    default:
        return false;
    }
}

static bool button_action(struct ui_widget *source, const struct ui_action *action,
                           void *userdata)
{
    (void)source;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    ui_dropdown_open(userdata);
    return true;
}

static bool dropdown_event(struct ui_widget *widget, const struct ui_event *event)
{
    if (event->kind != UI_EVENT_KEY ||
        (event->data.key.key != UI_KEY_DOWN && event->data.key.key != UI_KEY_UP))
        return false;
    return ui_dropdown_open(widget->userdata);
}

static bool dropdown_accessibility(
    const struct ui_widget *widget, struct ui_accessibility_node_spec *out)
{
    const struct ui_dropdown *d = widget->userdata;
    out->role = UI_ACCESSIBILITY_ROLE_COMBO_BOX;
    out->states |= UI_ACCESSIBILITY_STATE_FOCUSABLE;
    out->actions |= UI_ACCESSIBILITY_ACTION_FOCUS |
                    UI_ACCESSIBILITY_ACTION_INVOKE;
    if (ui_widget_has_focus(&d->button.widget))
        out->states |= UI_ACCESSIBILITY_STATE_FOCUSED;
    if (d->impl.open)
        out->states |= UI_ACCESSIBILITY_STATE_EXPANDED;
    if (d->impl.selected >= 0 && (size_t)d->impl.selected < d->impl.count)
        out->value = d->impl.items[d->impl.selected].label;
    return true;
}

static bool dropdown_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_dropdown *d = widget->userdata;
    if (node_id != ui_widget_accessibility_id(widget))
        return false;
    if (request->action == UI_ACCESSIBILITY_ACTION_FOCUS) {
        ui_widget_focus(&d->button.widget);
        return true;
    }
    if (request->action != UI_ACCESSIBILITY_ACTION_INVOKE)
        return false;
    if (d->impl.open) {
        ui_dropdown_close(d);
        return true;
    }
    return ui_dropdown_open(d);
}

static void dropdown_destroy(struct ui_widget *widget)
{
    struct ui_dropdown *d = widget->userdata;
    ui_dropdown_close(d);
    ui_widget_destroy(&d->layer);
    free_items(d->impl.items, d->impl.count);
    d->impl.items = NULL;
    d->impl.count = 0;
}

static const struct ui_widget_ops DROPDOWN_OPS = {
    .name = "dropdown", .event = dropdown_event, .destroy = dropdown_destroy,
    .accessibility = dropdown_accessibility,
    .accessibility_action = dropdown_accessibility_action,
};

static const struct ui_widget_ops LAYER_OPS = {
    .name = "dropdown-layer", .event = layer_event,
};

void ui_dropdown_init(struct ui_dropdown *d)
{
    *d = (struct ui_dropdown){0};
    d->impl.selected = -1;
    ui_widget_init(&d->widget, &DROPDOWN_OPS, d);
    ui_widget_set_style_identity(&d->widget, "dropdown", NULL, "dropdown");
    ui_button_init(&d->button);
    ui_widget_set_accessibility_ignored(&d->button.widget, true);
    ui_widget_set_style_identity(&d->button.widget, "button dropdown-button",
                                  NULL, "button");
    ui_widget_set_action_handler(&d->button.widget, button_action, d);
    ui_widget_add_child(&d->widget, &d->button.widget);
    ui_widget_init(&d->layer, &LAYER_OPS, d);
    ui_widget_set_hidden(&d->layer, true);
    ui_widget_set_modal(&d->layer, true);
    ui_widget_set_action_handler(&d->layer, popup_action, d);
    ui_box_init(&d->panel);
    ui_box_set_direction(&d->panel, UI_BOX_COLUMN);
    ui_box_set_align(&d->panel, UI_BOX_ALIGN_STRETCH);
    ui_widget_set_style_identity(&d->panel.widget, "dropdown-popup", NULL, "box");
    ui_input_init(&d->search);
    ui_input_set_placeholder(&d->search, "Search choices...");
    ui_widget_set_accessible_name(&d->search.widget, "Search choices");
    ui_scrollview_init(&d->scroll);
    ui_scrollview_set_scrollbar(&d->scroll, true);
    ui_widget_set_style_identity(&d->scroll.widget, "dropdown-scroll", NULL, "scrollview");
    ui_list_init(&d->list);
    ui_list_set_wrap_navigation(&d->list, false);
    ui_label_init(&d->empty);
    ui_label_set_text(&d->empty, "No matching choices");
    ui_widget_add_child(&d->layer, &d->panel.widget);
    ui_widget_add_children(&d->panel.widget, (struct ui_widget *[]){
        ui_input_widget(&d->search), ui_scrollview_widget(&d->scroll),
        ui_label_widget(&d->empty),
    }, 3);
    ui_widget_add_child(&d->scroll.widget, ui_list_widget(&d->list));
    show_selection(d);
}

bool ui_dropdown_set_items(struct ui_dropdown *d,
                            const struct ui_dropdown_item *items, size_t count)
{
    ui_dropdown_widget(d);
    if (count > INT_MAX || (count && !items))
        return false;
    struct ui_dropdown_item *copy = calloc(count ? count : 1, sizeof(*copy));
    if (!copy)
        return false;
    for (size_t i = 0; i < count; i++) {
        copy[i].label = strdup(items[i].label ? items[i].label : "");
        copy[i].value = strdup(items[i].value ? items[i].value : "");
        if (!copy[i].label || !copy[i].value) {
            free_items(copy, count);
            return false;
        }
    }
    ui_dropdown_close(d);
    free_items(d->impl.items, d->impl.count);
    d->impl.items = copy;
    d->impl.count = count;
    d->impl.selected = count ? 0 : -1;
    show_selection(d);
    return true;
}

void ui_dropdown_set_overlay(struct ui_dropdown *d, struct ui_widget *parent)
{
    if (!d)
        return;
    ui_dropdown_widget(d);
    ui_dropdown_close(d);
    d->impl.overlay = parent;
}

void ui_dropdown_set_value(struct ui_dropdown *d, const char *value)
{
    ui_dropdown_widget(d);
    d->impl.selected = -1;
    for (size_t i = 0; i < d->impl.count; i++)
        if (!strcmp(d->impl.items[i].value, value ? value : "")) {
            d->impl.selected = (int)i;
            break;
        }
    show_selection(d);
}

void ui_dropdown_set_action_id(struct ui_dropdown *d, int id)
{
    if (!d)
        return;
    ui_dropdown_widget(d);
    d->impl.action_id = id;
}

const char *ui_dropdown_value(const struct ui_dropdown *d)
{
    return d && d->impl.selected >= 0 ? d->impl.items[d->impl.selected].value : "";
}

bool ui_dropdown_open(struct ui_dropdown *d)
{
    ui_dropdown_widget(d);
    if (d->impl.open)
        return true;
    if (!d->impl.overlay || !d->impl.count ||
        ui_widget_surface(d->impl.overlay) != ui_widget_surface(&d->widget) ||
        !ui_widget_surface(&d->widget))
        return false;
    ui_input_clear(&d->search);
    if (!filter(d) || !ui_widget_add_child(d->impl.overlay, &d->layer))
        return false;
    ui_widget_set_placement(&d->panel.widget, (struct ui_placement){
        .kind = UI_PLACE_BELOW, .anchor = d->widget.bounds,
    });
    d->impl.open = true;
    ui_widget_set_hidden(&d->layer, false);
    struct ui_surface *surface = ui_widget_surface(&d->widget);
    ui_surface_prepare(surface);
    ui_widget_focus(ui_input_widget(&d->search));
    reveal_selection(d);
    return true;
}

void ui_dropdown_close(struct ui_dropdown *d)
{
    if (!d || !d->impl.open)
        return;
    d->impl.open = false;
    ui_widget_set_hidden(&d->layer, true);
    if (d->layer.parent)
        ui_widget_remove_child(d->layer.parent, &d->layer);
    struct ui_surface *surface = ui_widget_surface(&d->widget);
    if (surface) ui_surface_prepare(surface);
}

bool ui_dropdown_is_open(const struct ui_dropdown *d)
{
    return d && d->impl.open;
}
