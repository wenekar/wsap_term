#include "ui/widgets/menu.h"

#include "ui/widget_impl.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

#define MENU_HOVER_DELAY_MS 400u
#define MENU_AIM_GRACE_MS   300u

static struct ui_menu_level *level_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_menu_level,
                           widget);
}

static struct ui_menu_level *top_level(struct ui_menu *m)
{
    return m->impl.n_levels > 0 ? &m->levels[m->impl.n_levels - 1] : NULL;
}

static void emit(struct ui_menu *m, struct ui_widget *source,
                 enum ui_action_kind kind, int id, int index)
{
    if (m->impl.actions.fn)
        m->impl.actions.fn(source, &(struct ui_action){
            .kind = kind, .id = id, .index = index,
        }, m->impl.actions.userdata);
}

/* Hosts derive what they show from the generation. The level widgets
 * invalidate, so a host in a surface gets a frame. */
static void changed(struct ui_menu *m)
{
    m->impl.generation++;
    ui_widget_invalidate(&m->levels[0].widget, UI_WIDGET_INVALIDATE_MEASURE);
}

/* The item under the pointer, else the selected one, of the top level. */
static int highlighted(struct ui_menu *m)
{
    struct ui_menu_level *top = top_level(m);
    if (!top)
        return -1;
    int index = ui_list_hovered(&top->list);
    return index >= 0 ? index : ui_list_selected(&top->list);
}

static void report_highlight(struct ui_menu *m)
{
    struct ui_menu_level *top = top_level(m);
    int index = highlighted(m);
    const struct ui_list_item *it = top ? ui_list_item(&top->list, index) : NULL;
    int action = it ? it->action : -1;
    if (m->impl.highlight_known && action == m->impl.highlight)
        return;
    m->impl.highlight = action;
    m->impl.highlight_known = true;
    emit(m, top ? &top->widget : NULL, UI_ACTION_HIGHLIGHT, action, index);
}

static void close_above(struct ui_menu *m, int depth)
{
    if (m->impl.n_levels > depth + 1) {
        m->impl.n_levels = depth + 1;
        changed(m);
    }
    m->impl.pending_level = 0;
    m->impl.aim_level = 0;
}

static bool open_submenu(struct ui_menu *m, int depth, int index,
                         bool select_first)
{
    if (depth + 1 >= UI_MENU_MAX_LEVELS || depth >= m->impl.n_levels)
        return false;
    struct ui_menu_level *parent = &m->levels[depth];
    const struct ui_list_item *it = ui_list_item(&parent->list, index);
    if (!it || !(it->flags & UI_LIST_ITEM_SUBMENU))
        return false;
    int n = 0;
    const ui_menu_item *items = m->impl.submenu
        ? m->impl.submenu(m->impl.submenu_userdata, it->action, &n) : NULL;
    close_above(m, depth);
    if (!items || n <= 0)
        return false;
    struct ui_menu_level *child = &m->levels[depth + 1];
    if (!ui_list_set_items(&child->list, items, n))
        return false;
    ui_list_pin_metrics(&child->list, &m->levels[0].list);
    child->parent_item = index;
    m->impl.n_levels = depth + 2;
    changed(m);
    if (select_first)
        ui_list_step(&child->list, 1);
    report_highlight(m);
    return true;
}

static void activate(struct ui_menu *m, struct ui_menu_level *level, int index)
{
    const struct ui_list_item *it = ui_list_item(&level->list, index);
    if (!it || (it->flags & (UI_LIST_ITEM_DISABLED | UI_LIST_ITEM_SEPARATOR)))
        return;
    if (it->flags & UI_LIST_ITEM_SUBMENU) {
        open_submenu(m, level->depth, index, false);
        return;
    }
    emit(m, &level->widget, UI_ACTION_ACTIVATE, it->action, index);
    ui_menu_close(m);
}

/* Where the child of level depth sits, in the surface space of depth. */
static struct rect2i child_rect(const struct ui_menu *m, int depth)
{
    const struct ui_menu_level *child = &m->levels[depth + 1];
    if (ui_widget_surface(&child->widget) ==
        ui_widget_surface(&m->levels[depth].widget))
        return child->widget.bounds;
    struct rect2i anchor = ui_menu_level_anchor(m, depth + 1);
    return (struct rect2i){
        .pos = { .x = anchor.x + anchor.width, .y = anchor.y },
        .size = child->widget.measured_size,
    };
}

static void pointer_moved(struct ui_menu *m, struct ui_menu_level *level,
                          struct vector2i pos)
{
    int depth = level->depth;
    if (m->impl.aim_level && depth >= m->impl.aim_level)
        m->impl.aim_level = 0;
    int hovered = ui_list_hovered(&level->list);
    bool child_open = depth + 1 < m->impl.n_levels;
    if (child_open && hovered != m->levels[depth + 1].parent_item) {
        /* Off the item that opened the child: keep the child open while
         * the pointer heads for it. */
        struct rect2i sub = child_rect(m, depth);
        int dx = pos.x - m->impl.last_pointer.x;
        bool toward = (sub.x > m->impl.last_pointer.x && dx > 0) ||
                      (rect2i_end(sub).x < m->impl.last_pointer.x && dx < 0);
        if (toward) {
            m->impl.aim_level = depth + 1;
            m->impl.aim_deadline = m->impl.now_ms + MENU_AIM_GRACE_MS;
        } else {
            close_above(m, depth);
            child_open = false;
        }
    } else if (child_open) {
        m->impl.aim_level = 0;
    }

    const struct ui_list_item *it = ui_list_item(&level->list, hovered);
    bool opens_child = child_open &&
                       m->levels[depth + 1].parent_item == hovered;
    if (it && (it->flags & UI_LIST_ITEM_SUBMENU) && !opens_child &&
        !m->impl.aim_level) {
        m->impl.pending_level = depth + 1;
        m->impl.pending_item = hovered;
        m->impl.hover_deadline = m->impl.now_ms +
            (m->impl.hover_delay_ms ? m->impl.hover_delay_ms
                                    : MENU_HOVER_DELAY_MS);
    } else if (m->impl.pending_level == depth + 1 &&
               m->impl.pending_item != hovered) {
        m->impl.pending_level = 0;
    }
    m->impl.last_pointer = pos;
    report_highlight(m);
}

static void pop_level(struct ui_menu *m)
{
    close_above(m, m->impl.n_levels - 2);
    report_highlight(m);
}

static bool mnemonic(struct ui_menu *m, struct ui_menu_level *top,
                     uint32_t ch)
{
    int want = tolower((int)ch);
    for (int i = 0; i < ui_list_count(&top->list); i++) {
        const struct ui_list_item *it = ui_list_item(&top->list, i);
        if (!it->label ||
            (it->flags & (UI_LIST_ITEM_DISABLED | UI_LIST_ITEM_SEPARATOR)))
            continue;
        const char *amp = strchr(it->label, '&');
        const char *key = amp && amp[1] ? amp + 1 : it->label;
        if (tolower((unsigned char)key[0]) != want)
            continue;
        ui_list_select(&top->list, i);
        report_highlight(m);
        if (it->flags & UI_LIST_ITEM_SUBMENU)
            open_submenu(m, top->depth, i, true);
        else
            activate(m, top, i);
        return true;
    }
    return false;
}

static void menu_key(struct ui_menu *m, const struct ui_key_event *key)
{
    if (key->action == UI_KEY_RELEASE)
        return;
    struct ui_menu_level *top = top_level(m);
    if (!top)
        return;
    int selected = ui_list_selected(&top->list);
    const struct ui_list_item *it = ui_list_item(&top->list, selected);
    bool ctrl = key->mods & UI_MOD_CTRL;
    switch (key->key) {
    case UI_KEY_ESCAPE:
        if (m->impl.n_levels > 1)
            pop_level(m);
        else
            ui_menu_close(m);
        return;
    case UI_KEY_LEFT:
    case UI_KEY_BACKSPACE:
        if (m->impl.n_levels > 1)
            pop_level(m);
        return;
    case UI_KEY_UP:
    case UI_KEY_DOWN:
        ui_list_step(&top->list, key->key == UI_KEY_DOWN ? 1 : -1);
        report_highlight(m);
        return;
    case UI_KEY_HOME:
    case UI_KEY_END:
        ui_list_select_edge(&top->list,
                            key->key == UI_KEY_HOME ? 1 : -1);
        report_highlight(m);
        return;
    case UI_KEY_PAGE_UP:
    case UI_KEY_PAGE_DOWN: {
        int direction = key->key == UI_KEY_PAGE_DOWN ? 1 : -1;
        int page = top->list.widget.bounds.height;
        int row = ui_list_item_rect(&top->list, selected).height;
        int steps = row > 0 ? page / row : 1;
        if (steps < 1)
            steps = 1;
        for (int i = 0; i < steps; i++)
            ui_list_step(&top->list, direction);
        report_highlight(m);
        return;
    }
    case UI_KEY_RIGHT:
    case UI_KEY_ENTER:
        if (it && (it->flags & UI_LIST_ITEM_SUBMENU))
            open_submenu(m, top->depth, selected, true);
        else if (it && key->key == UI_KEY_ENTER)
            activate(m, top, selected);
        return;
    case UI_KEY_CHAR:
        if (ctrl && (key->text == 'c' || key->text == 'C')) {
            if (m->impl.n_levels > 1)
                pop_level(m);
            else
                ui_menu_close(m);
            return;
        }
        if (key->text && key->text <= 0x7f && isprint((int)key->text) &&
            !(key->mods & (UI_MOD_ALT | UI_MOD_CTRL | UI_MOD_SUPER)))
            mnemonic(m, top, key->text);
        return;
    default:
        return;
    }
}

static bool level_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_menu_level *level = level_of(widget);
    struct ui_menu *m = level->menu;
    if (event->time_ms)
        m->impl.now_ms = event->time_ms;
    switch (event->kind) {
    case UI_EVENT_KEY:
        menu_key(m, &event->data.key);
        return true;
    case UI_EVENT_TEXT:
        for (size_t i = 0; m->impl.open && i < event->data.text.len; i++) {
            unsigned char c = (unsigned char)event->data.text.data[i];
            if (c < 0x80)
                menu_key(m, &(struct ui_key_event){
                    .key = UI_KEY_CHAR, .text = c,
                });
        }
        return true;
    case UI_EVENT_POINTER:
        if (event->data.pointer.kind == UI_POINTER_MOTION && m->impl.open)
            pointer_moved(m, level, event->data.pointer.pos);
        return true;
    case UI_EVENT_FOCUS_IN:
    case UI_EVENT_FOCUS_OUT:
        return true;
    default:
        return false;
    }
}

/* The level's list reports item changes; the menu turns them into its
 * own actions. */
static bool level_action(struct ui_widget *source, const struct ui_action *action,
                         void *userdata)
{
    (void)source;
    struct ui_menu_level *level = userdata;
    if (action->kind == UI_ACTION_ACTIVATE)
        activate(level->menu, level, action->index);
    return true;
}

/* Level 0 shows while the menu is open, wherever its host puts it: it
 * runs the menu's timers. */
static uint64_t level_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_menu_level *level = level_of(widget);
    return level->depth == 0 ? ui_menu_tick(level->menu, now_ms) : 0;
}

static const struct ui_widget_ops LEVEL_OPS = {
    .name = "menu-level",
    .event = level_event,
    .tick = level_tick,
};

void ui_menu_init(struct ui_menu *m)
{
    if (!m)
        return;
    *m = (struct ui_menu){0};
    for (int i = 0; i < UI_MENU_MAX_LEVELS; i++) {
        struct ui_menu_level *level = &m->levels[i];
        level->menu = m;
        level->depth = i;
        ui_widget_init(&level->widget, &LEVEL_OPS, NULL);
        ui_widget_set_focusable(&level->widget, true);
        ui_list_init(&level->list);
        ui_list_set_style_class(&level->list, "context-menu");
        ui_list_set_focusable(&level->list, false);
        ui_widget_set_action_handler(&level->list.widget, level_action, level);
        ui_widget_add_child(&level->widget, &level->list.widget);
    }
}

void ui_menu_destroy(struct ui_menu *m)
{
    if (!m)
        return;
    m->impl.open = false;
    m->impl.n_levels = 0;
    for (int i = 0; i < UI_MENU_MAX_LEVELS; i++)
        ui_widget_destroy(&m->levels[i].widget);
}

void ui_menu_set_action_handler(struct ui_menu *m, ui_action_fn fn,
                                void *userdata)
{
    if (m)
        m->impl.actions = (struct ui_action_handler){ fn, userdata };
}

void ui_menu_set_submenu_provider(struct ui_menu *m, ui_menu_submenu_fn fn,
                                  void *userdata)
{
    if (!m)
        return;
    m->impl.submenu = fn;
    m->impl.submenu_userdata = userdata;
}

void ui_menu_set_hover_delay(struct ui_menu *m, uint32_t ms)
{
    if (m)
        m->impl.hover_delay_ms = ms;
}

bool ui_menu_open(struct ui_menu *m, const ui_menu_item *items, int n)
{
    if (!m || !items || n <= 0)
        return false;
    ui_menu_close(m);
    struct ui_menu_level *root = &m->levels[0];
    if (!ui_list_set_items(&root->list, items, n))
        return false;
    /* A new menu lays out in the current theme, then keeps that geometry. */
    root->list.impl.metrics.valid = false;
    root->list.widget.impl.style_valid = false;
    ui_list_pin_metrics(&root->list, NULL);
    m->impl.n_levels = 1;
    m->impl.open = true;
    m->impl.highlight_known = false;
    m->impl.pending_level = 0;
    m->impl.aim_level = 0;
    changed(m);
    return true;
}

void ui_menu_close(struct ui_menu *m)
{
    if (!ui_menu_is_open(m))
        return;
    struct ui_widget *source = &m->levels[0].widget;
    m->impl.open = false;
    m->impl.n_levels = 0;
    m->impl.pending_level = 0;
    m->impl.aim_level = 0;
    changed(m);
    emit(m, source, UI_ACTION_CLOSE, -1, -1);
}

bool ui_menu_is_open(const struct ui_menu *m)
{
    return m && m->impl.open;
}

uint32_t ui_menu_generation(const struct ui_menu *m)
{
    return m ? m->impl.generation : 0;
}

int ui_menu_level_count(const struct ui_menu *m)
{
    return m ? m->impl.n_levels : 0;
}

struct ui_widget *ui_menu_level_widget(struct ui_menu *m, int depth)
{
    return m && depth >= 0 && depth < m->impl.n_levels
        ? &m->levels[depth].widget : NULL;
}

struct rect2i ui_menu_level_anchor(const struct ui_menu *m, int depth)
{
    if (!m || depth <= 0 || depth >= m->impl.n_levels)
        return (struct rect2i){0};
    const struct ui_menu_level *child = &m->levels[depth];
    struct rect2i row = ui_list_item_rect(&m->levels[depth - 1].list,
                                          child->parent_item);
    /* The first rows line up: the child sits one edge pad higher. */
    row.y -= child->list.impl.metrics.edge_pad;
    return row;
}

struct insets2i ui_menu_level_shadow(const struct ui_menu *m, int depth)
{
    return m && depth >= 0 && depth < m->impl.n_levels
        ? ui_list_shadow(&m->levels[depth].list) : (struct insets2i){0};
}

uint64_t ui_menu_tick(struct ui_menu *m, uint64_t now_ms)
{
    if (!ui_menu_is_open(m))
        return 0;
    m->impl.now_ms = now_ms;
    if (m->impl.pending_level && now_ms >= m->impl.hover_deadline) {
        int depth = m->impl.pending_level - 1;
        m->impl.pending_level = 0;
        open_submenu(m, depth, m->impl.pending_item, false);
    }
    if (m->impl.aim_level && now_ms >= m->impl.aim_deadline)
        close_above(m, m->impl.aim_level - 1);
    uint64_t next = m->impl.pending_level ? m->impl.hover_deadline : 0;
    if (m->impl.aim_level && (!next || m->impl.aim_deadline < next))
        next = m->impl.aim_deadline;
    return next;
}

static struct ui_menu_layer *layer_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_menu_layer,
                           widget);
}

/* The layer's children are the menu's inline levels, each placed beside
 * the item that opened it. */
static void layer_prepare(struct ui_widget *widget)
{
    struct ui_menu_layer *layer = layer_of(widget);
    struct ui_menu *m = layer->impl.menu;
    uint32_t gen = ui_menu_generation(m);
    if (!layer->impl.stale && layer->impl.shown == gen)
        return;
    layer->impl.stale = false;
    layer->impl.shown = gen;
    int first = layer->impl.first_level;
    int count = ui_menu_level_count(m);
    /* Levels that stay keep their place, and so the focus. */
    for (struct ui_widget *c = widget->first_child, *next; c; c = next) {
        next = c->next_sibling;
        int depth = level_of(c)->depth;
        if (depth < first || depth >= count)
            ui_widget_remove_child(widget, c);
    }
    for (int d = first; d < count; d++) {
        struct ui_widget *level = ui_menu_level_widget(m, d);
        struct rect2i anchor = d == 0
            ? (struct rect2i){ .pos = layer->impl.origin }
            : ui_menu_level_anchor(m, d);
        ui_widget_set_placement(level, (struct ui_placement){
            .kind = UI_PLACE_RIGHT, .anchor = anchor,
        });
        if (level->parent == widget)
            continue;
        if (level->parent)
            ui_widget_remove_child(level->parent, level);
        ui_widget_add_child(widget, level);
    }
    ui_widget_set_hidden(widget, widget->first_child == NULL);
}

static bool layer_event(struct ui_widget *widget, const struct ui_event *event)
{
    if (event->kind != UI_EVENT_POINTER)
        return false;
    if (ui_pointer_is_button_press(&event->data.pointer))
        ui_menu_close(layer_of(widget)->impl.menu);
    return true;
}

static const struct ui_widget_ops LAYER_OPS = {
    .name = "menu-layer",
    .prepare = layer_prepare,
    .event = layer_event,
};

void ui_menu_layer_init(struct ui_menu_layer *layer, struct ui_menu *menu)
{
    if (!layer)
        return;
    *layer = (struct ui_menu_layer){ .impl.menu = menu, .impl.stale = true };
    ui_widget_init(&layer->widget, &LAYER_OPS, NULL);
    ui_widget_set_modal(&layer->widget, true);
    ui_widget_set_hidden(&layer->widget, true);
}

void ui_menu_layer_set_origin(struct ui_menu_layer *layer,
                              struct vector2i origin)
{
    if (!layer)
        return;
    layer->impl.origin = origin;
    layer->impl.stale = true;
    ui_widget_invalidate(&layer->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_menu_layer_set_first_level(struct ui_menu_layer *layer, int first)
{
    if (!layer)
        return;
    if (first < 0)
        first = 0;
    if (first > UI_MENU_MAX_LEVELS)
        first = UI_MENU_MAX_LEVELS;
    if (layer->impl.first_level == first)
        return;
    layer->impl.first_level = first;
    layer->impl.stale = true;
    ui_widget_invalidate(&layer->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

bool ui_menu_show(struct ui_menu *menu, struct ui_menu_layer *layer,
                  const struct ui_menu_desc *desc)
{
    if (!menu || !desc)
        return false;
    ui_menu_close(menu);
    ui_menu_set_action_handler(menu, desc->on_action, desc->userdata);
    ui_menu_set_submenu_provider(menu, desc->on_submenu, desc->userdata);
    ui_menu_set_hover_delay(menu, desc->hover_delay_ms);
    if (!ui_menu_open(menu, desc->items, desc->count))
        return false;
    if (layer)
        ui_menu_layer_set_origin(layer, desc->pos);
    return true;
}
