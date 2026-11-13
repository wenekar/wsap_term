#include "ui/widgets/tabbar/internal.h"

#include "ui/debug.h"
#include "ui/paint.h"
#include "ui/surface.h"
#include "ui/widget_impl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum drag_finish {
    FINISH_NONE = 0,
    FINISH_REORDER,
    FINISH_DETACH,
};

static bool tb_ensure_capacity(struct ui_tabbar *tb, int n)
{
    if (!tb || n <= tb->impl.capacity)
        return true;
    int cap = tb->impl.capacity ? tb->impl.capacity * 2 : 8;
    while (cap < n)
        cap *= 2;

    struct ui_tab_state *tabs = realloc(tb->impl.tabs, (size_t)cap * sizeof(*tabs));
    if (!tabs)
        return false;

    memset(&tabs[tb->impl.capacity], 0, (size_t)(cap - tb->impl.capacity) * sizeof(*tabs));
    tb->impl.tabs = tabs;
    tb->impl.capacity = cap;
    return true;
}

bool tb_ensure_tabs(struct ui_tabbar *tb, int n)
{
    if (!tb || n < 0)
        return false;
    if (n > tb->impl.capacity && !tb_ensure_capacity(tb, n))
        return false;
    if (n > tb->impl.tab_cap) {
        memset(&tb->impl.tabs[tb->impl.tab_cap], 0, (size_t)(n - tb->impl.tab_cap) * sizeof(*tb->impl.tabs));
        for (int i = tb->impl.tab_cap; i < n; i++) {
            tb->impl.tabs[i].open.value = 1.0f;
            tb->impl.tabs[i].open.to = 1.0f;
            tb->impl.tabs[i].accessibility_id =
                ui_widget_accessibility_id_allocate();
        }
        tb->impl.tab_cap = n;
    }
    for (int i = 0; i < n; i++)
        if (!tb->impl.tabs[i].accessibility_id)
            tb->impl.tabs[i].accessibility_id =
                ui_widget_accessibility_id_allocate();
    return true;
}

static void ripple(struct ui_tabbar *tb, const struct tb_theme *t, int n_tabs,
                   struct vector2i size, int index, struct vector2i click_pos,
                   uint64_t now_ms)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return;
    struct rect2i r;
    if (!tb_tab_rect_at(tb, t, n_tabs, size, index, &r))
        return;
    if ((t->ripple_color >> 24) > 0) {
        ui_ripple_start(&tb->impl.tabs[index].ripple, r, click_pos,
                        t->ripple_color, now_ms,
                        t->ripple_duration_ms);
        ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    }
}

static void set_hover(struct ui_tabbar *tb, int hover_tab, int hover_close, uint64_t now_ms)
{
    if (!tb)
        return;
    tb->impl.hover_tab = hover_tab;
    tb->impl.hover_close = hover_close;
    int top = hover_tab > hover_close ? hover_tab : hover_close;
    if (top >= 0 && top >= tb->impl.tab_cap)
        tb_ensure_tabs(tb, top + 1);
    for (int i = 0; i < tb->impl.tab_cap; i++) {
        float target = (i == hover_close) ? 1.0f : 0.0f;
        ui_hover_anim_set_target(&tb->impl.tabs[i].close_hover, target, now_ms);
    }
}

static void set_hover_new(struct ui_tabbar *tb, bool hover_new, uint64_t now_ms)
{
    if (!tb)
        return;
    tb->impl.hover_new = hover_new;
    ui_hover_anim_set_target(&tb->impl.new_hover, hover_new ? 1.0f : 0.0f, now_ms);
}

static bool bar_tick(struct ui_tabbar *tb, struct vector2i size, uint64_t now_ms)
{
    (void)size;
    if (!tb)
        return false;
    tb->impl.last_tick_ms = now_ms;
    bool animating = false;

    for (int i = 0; i < tb->impl.tab_cap; i++) {
        if (ui_hover_anim_tick(&tb->impl.tabs[i].close_hover, now_ms,
                               UI_TABBAR_ANIM_DURATION_MS,
                               UI_EASE_OUT_CUBIC))
            animating = true;
        struct ui_ripple *rp = &tb->impl.tabs[i].ripple;
        if (rp->active) {
            if (ui_ripple_eval(rp, now_ms, NULL, NULL))
                animating = true;
            else
                rp->active = false;
        }
    }

    for (int i = 0; i < tb->impl.tab_cap; i++) {
        if (ui_hover_anim_tick(&tb->impl.tabs[i].slide, now_ms,
                               UI_TABBAR_SLIDE_ANIM_MS,
                               UI_EASE_OUT_CUBIC))
            animating = true;
    }
    if (ui_hover_anim_tick(&tb->impl.new_hover, now_ms,
                           UI_TABBAR_ANIM_DURATION_MS, UI_EASE_OUT_CUBIC))
        animating = true;

    if (tb->impl.reveal.start_ms) {

        int effective = tb_live_tabs(tb, tb->impl.tab_cap);
        float want = effective >= 2 || tb->impl.external_drag ? 1.0f : 0.0f;
        if (tb->impl.reveal.to != want)
            ui_hover_anim_set_target(&tb->impl.reveal, want, now_ms);
        if (ui_hover_anim_tick(&tb->impl.reveal, now_ms,
                               UI_TABBAR_RESIZE_ANIM_MS,
                               UI_EASE_IN_OUT_CUBIC))
            animating = true;
        if (tb->impl.reveal.value == tb->impl.reveal.to)
            tb->impl.reveal.start_ms = 0;
    }

    for (int i = 0; i < tb->impl.tab_cap; i++) {
        struct ui_tab_state *tab = &tb->impl.tabs[i];
        if (!tab->closing) {
            if (tab->open.value <= 0.0f && tab->open.to <= 0.0f) {
                tab->open.value = 1.0f;
                tab->open.to = 1.0f;
                continue;
            }
            if (tab->open.to != 1.0f)
                ui_hover_anim_set_target(&tab->open, 1.0f, now_ms);
            if (ui_hover_anim_tick(&tab->open, now_ms,
                                   UI_TABBAR_RESIZE_ANIM_MS,
                                   UI_EASE_IN_OUT_CUBIC))
                animating = true;
            continue;
        }
        if (ui_hover_anim_tick(&tab->open, now_ms,
                               UI_TABBAR_RESIZE_ANIM_MS,
                               UI_EASE_IN_OUT_CUBIC))
            animating = true;
    }
    return animating;
}

bool ui_tabbar_begin_close(struct ui_tabbar *tb, int index, uint64_t now_ms)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return false;
    struct ui_tab_state *tab = &tb->impl.tabs[index];
    if (tab->closing)
        return true;
    tab->closing = true;
    if (tab->open.value <= 0.0f)
        tab->open.value = 1.0f;
    ui_hover_anim_set_target(&tab->open, 0.0f, now_ms);

    if (tb_live_tabs(tb, tb->impl.tab_cap) <= 1) {
        if (!tb->impl.reveal.start_ms) {
            tb->impl.reveal.value = 1.0f;
            tb->impl.reveal.to = 1.0f;
        }
        ui_hover_anim_set_target(&tb->impl.reveal, 0.0f, now_ms);
    }
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

bool ui_tabbar_begin_open(struct ui_tabbar *tb, int index, uint64_t now_ms)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return false;
    struct ui_tab_state *tab = &tb->impl.tabs[index];
    if (tab->closing)
        return false;
    tab->open.value = 0.0f;
    tab->open.from = 0.0f;
    tab->open.to = 0.0f;
    ui_hover_anim_set_target(&tab->open, 1.0f, now_ms);

    if (tb->impl.tab_cap == 2 && !tb->impl.reveal.start_ms) {
        tb->impl.reveal.value = 0.0f;
        tb->impl.reveal.to = 0.0f;
    }
    if (tb->impl.tab_cap == 2)
        ui_hover_anim_set_target(&tb->impl.reveal, 1.0f, now_ms);
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

bool ui_tabbar_is_closing(const struct ui_tabbar *tb, int index)
{
    return tb && index >= 0 && index < tb->impl.tab_cap &&
           tb->impl.tabs[index].closing;
}

int ui_tabbar_closing_tab(const struct ui_tabbar *tb)
{
    if (!tb)
        return -1;
    for (int i = 0; i < tb->impl.tab_cap; i++)
        if (tb->impl.tabs[i].closing)
            return i;
    return -1;
}

bool ui_tabbar_closing_done(const struct ui_tabbar *tb)
{
    int i = ui_tabbar_closing_tab(tb);
    if (i < 0)
        return false;
    return tb->impl.tabs[i].open.value <= 0.0f;
}

static void tb_slide_reset(struct ui_tab_state *t)
{
    t->slide.value = 0.0f;
    t->slide.from = 0.0f;
    t->slide.to = 0.0f;
    t->slide.start_ms = 0;
}

static void tb_slide_tabs_to(struct ui_tabbar *tb, int n_tabs,
                                  int lo, int hi, float disp)
{
    if (!tb_ensure_tabs(tb, n_tabs))
        return;

    uint64_t now = tb->impl.last_tick_ms ? tb->impl.last_tick_ms : 1;
    for (int i = 0; i < n_tabs; i++)
        ui_hover_anim_set_target(&tb->impl.tabs[i].slide,
                                 (i >= lo && i < hi) ? disp : 0.0f, now);
}

static void drag_start(struct ui_tabbar *tb, int index, struct vector2i click_pos, struct rect2i tab_rect)
{
    if (!tb || index < 0)
        return;
    tb->impl.drag_state = UI_TAB_DRAG_PENDING;
    tb->impl.drag_tab = index;
    tb->impl.drag_target_slot = index;
    tb->impl.drag_origin = click_pos;
    tb->impl.drag_pos = click_pos;
    tb->impl.drag_grab_x = click_pos.x - tab_rect.x;
    for (int i = 0; i < tb->impl.tab_cap; i++) {
        tb_slide_reset(&tb->impl.tabs[i]);
    }
}

static bool drag_motion(struct ui_tabbar *tb, const struct tb_theme *t,
                        int n_tabs, struct vector2i size, struct vector2i p)
{
    if (!tb || tb->impl.drag_state == UI_TAB_DRAG_NONE || n_tabs < 1)
        return false;
    tb->impl.drag_pos = p;
    int dx = p.x - tb->impl.drag_origin.x;
    int dy = p.y - tb->impl.drag_origin.y;

    if (tb->impl.drag_state == UI_TAB_DRAG_PENDING) {
        if (dx * dx + dy * dy < 25)
            return false;
        tb->impl.drag_state = UI_TAB_DRAG_REORDER;
    }

    int h = tb_bar_height(tb, t->height, n_tabs);
    if (p.y > h + 25 || p.y < -25 || p.x > size.x + 25 || p.x < -25) {
        tb->impl.drag_state = UI_TAB_DRAG_DETACH;
    } else {
        tb->impl.drag_state = UI_TAB_DRAG_REORDER;
    }

    struct tb_layout l = tb_layout_for(tb, t, n_tabs, size);
    int stride = l.tab_width + l.gap;

    if (tb->impl.drag_state == UI_TAB_DRAG_REORDER) {
        int tab_center_x = p.x - tb->impl.drag_grab_x + l.tab_width / 2;
        int rel_x = tab_center_x - l.pad.l;
        int slot = stride > 0 ? (rel_x + stride / 2) / stride : 0;
        if (slot < 0) slot = 0;
        if (slot >= n_tabs) slot = n_tabs - 1;
        tb->impl.drag_target_slot = slot;

        int lo = 0, hi = 0;
        float disp = 0.0f;
        if (tb->impl.drag_tab < slot) {
            lo = tb->impl.drag_tab + 1;
            hi = slot + 1;
            disp = -(float)stride;
        } else if (tb->impl.drag_tab > slot) {
            lo = slot;
            hi = tb->impl.drag_tab;
            disp = +(float)stride;
        }
        tb_slide_tabs_to(tb, n_tabs, lo, hi, disp);
    } else if (tb->impl.drag_state == UI_TAB_DRAG_DETACH) {
        tb_slide_tabs_to(tb, n_tabs, tb->impl.drag_tab + 1, n_tabs, -(float)stride);
    }
    return true;
}

static enum drag_finish drag_finish(struct ui_tabbar *tb, int *out_target_slot)
{
    if (!tb || tb->impl.drag_state == UI_TAB_DRAG_NONE) {
        if (out_target_slot) *out_target_slot = -1;
        return FINISH_NONE;
    }
    enum ui_tab_drag_state st = tb->impl.drag_state;
    int target = tb->impl.drag_target_slot;
    int dragged = tb->impl.drag_tab;

    tb->impl.drag_state = UI_TAB_DRAG_NONE;
    tb->impl.drag_tab = -1;
    tb->impl.drag_target_slot = -1;
    for (int i = 0; i < tb->impl.tab_cap; i++) {
        tb_slide_reset(&tb->impl.tabs[i]);
    }

    if (st == UI_TAB_DRAG_DETACH) {
        if (out_target_slot) *out_target_slot = dragged;
        return FINISH_DETACH;
    }
    if (st == UI_TAB_DRAG_REORDER) {
        if (out_target_slot) *out_target_slot = target;
        return FINISH_REORDER;
    }
    if (out_target_slot) *out_target_slot = -1;
    return FINISH_NONE;
}

static void drag_cancel(struct ui_tabbar *tb)
{
    if (!tb) return;
    tb->impl.drag_state = UI_TAB_DRAG_NONE;
    tb->impl.drag_tab = -1;
    tb->impl.drag_target_slot = -1;
    for (int i = 0; i < tb->impl.tab_cap; i++) {
        tb_slide_reset(&tb->impl.tabs[i]);
    }
}

bool ui_tabbar_is_dragging(const struct ui_tabbar *tb)
{
    return tb && (tb->impl.drag_state == UI_TAB_DRAG_REORDER || tb->impl.drag_state == UI_TAB_DRAG_DETACH);
}

static void external_drag_at(struct ui_tabbar *tb, const struct tb_theme *t,
                             bool active, struct vector2i p, int n_tabs,
                             struct vector2i size)
{
    if (!tb) return;
    int h = tb_drop_height(tb, t, n_tabs);
    if (!active || n_tabs < 1 || p.x < 0 || p.x >= size.x ||
        p.y < 0 || p.y >= h) {
        tb->impl.external_drag = false;
        tb->impl.external_slot = -1;

        uint64_t now = tb->impl.last_tick_ms ? tb->impl.last_tick_ms : 1;
        for (int i = 0; i < tb->impl.tab_cap; i++)
            ui_hover_anim_set_target(&tb->impl.tabs[i].slide, 0.0f, now);
        return;
    }
    bool was_external = tb->impl.external_drag;
    tb->impl.external_drag = false;
    struct tb_layout hit_l = tb_layout_for(tb, t, n_tabs, size);
    tb->impl.external_drag = true;
    if (n_tabs == 1 && !was_external) {
        uint64_t now = tb->impl.last_tick_ms ? tb->impl.last_tick_ms : 1;
        ui_hover_anim_set_target(&tb->impl.reveal, 1.0f, now);
    }
    struct tb_layout l = tb_layout_for(tb, t, n_tabs, size);
    int slot = 0;
    float base_x = (float)hit_l.pad.l;
    for (int i = 0; i < n_tabs; i++) {
        int width = tb_tab_width_at(tb, &hit_l, i);
        int x = (int)roundf(base_x);
        base_x += tb_tab_width_f(tb, &hit_l, i) +
                  (float)hit_l.gap * tb_open_progress(tb, i);
        if (p.x < x + width / 2)
            break;
        slot = i + 1;
    }
    tb->impl.external_slot = slot;
    tb_slide_tabs_to(tb, n_tabs, slot, n_tabs, (float)(l.tab_width + l.gap));
}

static struct ui_tabbar *tb_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_tabbar, widget);
}

static void notify(struct ui_tabbar *tb, enum ui_action_kind kind, int id,
                   int index, int to, struct vector2i pos)
{
    ui_widget_notify(&tb->widget, (struct ui_action){
        .kind = kind, .id = id, .index = index, .to = to, .pos = pos,
    });
}

static int active_tab(const struct ui_tabbar *tb)
{
    for (int i = 0; tb && i < tb->impl.n_tabs; i++)
        if (tb->impl.labels[i].active)
            return i;
    return 0;
}

static void focus_item(struct ui_tabbar *tb, int index, bool activate)
{
    int n = tb->impl.n_tabs;
    if (n <= 0)
        return;
    if (index < 0)
        index = 0;
    if (index > n)
        index = n;
    if (tb->impl.keyboard_index != index) {
        tb->impl.keyboard_index = index;
        ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    }
    if (activate && index == n)
        notify(tb, UI_ACTION_ACTIVATE, UI_TABBAR_NEW_TAB, -1, 0,
               (struct vector2i){0});
    else if (activate)
        notify(tb, UI_ACTION_ACTIVATE, 0, index, 0, (struct vector2i){0});
}

static bool tabbar_key(struct ui_tabbar *tb, const struct ui_key_event *key)
{
    int n = tb->impl.n_tabs;
    if (n <= 0 || key->action == UI_KEY_RELEASE)
        return false;

    switch (key->key) {
    case UI_KEY_LEFT:
    case UI_KEY_RIGHT: {
        int count = n + 1;
        int index = tb->impl.keyboard_index;
        if (index < 0 || index >= count)
            index = active_tab(tb);
        index = (index + (key->key == UI_KEY_RIGHT ? 1 : count - 1)) % count;
        focus_item(tb, index, false);
        return true;
    }
    case UI_KEY_HOME:
        if (key->action != UI_KEY_PRESS)
            return true;
        focus_item(tb, 0, false);
        return true;
    case UI_KEY_END:
        if (key->action != UI_KEY_PRESS)
            return true;
        focus_item(tb, n - 1, false);
        return true;
    case UI_KEY_ENTER:
        if (key->action != UI_KEY_PRESS)
            return true;
        focus_item(tb, tb->impl.keyboard_index, true);
        return true;
    case UI_KEY_ESCAPE:
        ui_surface_focus(ui_widget_surface(&tb->widget), NULL);
        return true;
    case UI_KEY_CHAR:
        if (key->text == ' ' && key->action == UI_KEY_PRESS) {
            focus_item(tb, tb->impl.keyboard_index, true);
            return true;
        }
        if (key->text == '+' && key->action == UI_KEY_PRESS) {
            notify(tb, UI_ACTION_ACTIVATE, UI_TABBAR_NEW_TAB, -1, 0,
                   (struct vector2i){0});
            return true;
        }
        return false;
    case UI_KEY_DELETE:
        if (key->action == UI_KEY_PRESS) {
            int index = tb->impl.keyboard_index;
            if (index == n)
                return true;
            if (index < 0 || index > n)
                index = active_tab(tb);
            notify(tb, UI_ACTION_CLOSE, 0, index, 0, (struct vector2i){0});
        }
        return true;
    default:
        return false;
    }
}

static struct vector2i grab_point(const struct ui_tabbar *tb,
                                  const struct tb_theme *t)
{
    struct rect2i tab = {0};
    tb_tab_rect_at(tb, t, tb->impl.n_tabs, tb->impl.size, tb->impl.drag_tab, &tab);
    return (struct vector2i){ .x = tb->impl.drag_grab_x,
                              .y = tb->impl.drag_origin.y - tab.y };
}

static void hover(struct ui_tabbar *tb, struct tb_hit hit, uint64_t now_ms)
{
    int tab = hit.index;
    int close = hit.on_close ? hit.index : -1;
    if (tab == tb->impl.hover_tab && close == tb->impl.hover_close &&
        hit.on_new == tb->impl.hover_new)
        return;
    bool tab_changed = tab != tb->impl.hover_tab;
    set_hover(tb, tab, close, now_ms);
    set_hover_new(tb, hit.on_new, now_ms);
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    if (tab_changed)
        notify(tb, UI_ACTION_HIGHLIGHT, 0, tab, 0, (struct vector2i){0});
}

static void cancel_drag(struct ui_tabbar *tb)
{
    if (tb->impl.drag_state == UI_TAB_DRAG_NONE)
        return;
    int tab = tb->impl.drag_tab;
    bool reported = tb->impl.drag_state != UI_TAB_DRAG_PENDING;
    drag_cancel(tb);
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    if (reported)
        notify(tb, UI_ACTION_DRAG, UI_TAB_DRAG_NONE, tab, 0, (struct vector2i){0});
}

static bool drag_moved(struct ui_tabbar *tb, const struct tb_theme *t,
                       struct vector2i local, struct vector2i pos)
{
    int n = tb->impl.n_tabs;
    drag_motion(tb, t, n, tb->impl.size, local);
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    int tab = tb->impl.drag_tab;
    enum ui_tab_drag_state state = tb->impl.drag_state;
    if (state == UI_TAB_DRAG_PENDING)
        return true;
    if (state == UI_TAB_DRAG_DETACH) {
        struct ui_action tear = {
            .kind = UI_ACTION_TEAR_OFF, .index = tab, .pos = grab_point(tb, t),
        };
        if (ui_widget_notify(&tb->widget, tear)) {
            drag_cancel(tb);
            ui_widget_release_pointer(&tb->widget);
            return true;
        }
    }
    notify(tb, UI_ACTION_DRAG, state, tab, 0, pos);
    return true;
}

static bool drag_released(struct ui_tabbar *tb, const struct tb_theme *t)
{
    int tab = tb->impl.drag_tab;
    struct vector2i grab = grab_point(tb, t);
    int slot = -1;
    enum drag_finish finish = drag_finish(tb, &slot);
    ui_widget_release_pointer(&tb->widget);
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    if (finish == FINISH_REORDER)
        notify(tb, UI_ACTION_MOVE, 0, tab, slot, (struct vector2i){0});
    else if (finish == FINISH_DETACH)
        notify(tb, UI_ACTION_MOVE, 0, tab, -1, grab);
    return true;
}

static bool pressed(struct ui_tabbar *tb, const struct tb_theme *t,
                    const struct ui_pointer_event *pe,
                    struct vector2i local, uint64_t now_ms)
{
    int n = tb->impl.n_tabs;
    struct vector2i size = tb->impl.size;
    struct tb_hit hit = tb_hit_at(tb, t, n, size, local);
    if (hit.on_new) {
        if (pe->button == 0)
            notify(tb, UI_ACTION_ACTIVATE, UI_TABBAR_NEW_TAB, -1, 0,
                   (struct vector2i){0});
        return true;
    }
    if (hit.index < 0)
        return false;
    if (pe->button == 1 || (pe->button == 0 && hit.on_close)) {
        notify(tb, UI_ACTION_CLOSE, 0, hit.index, 0, (struct vector2i){0});
        return true;
    }
    if (pe->button != 0)
        return true;
    tb->impl.pressed_tab = hit.index;
    struct rect2i tab_r;
    if (tb_tab_rect_at(tb, t, n, size, hit.index, &tab_r)) {
        drag_start(tb, hit.index, local, tab_r);
        ui_widget_capture_pointer(&tb->widget);
    }
    ripple(tb, t, n, size, hit.index, local, now_ms);
    notify(tb, UI_ACTION_ACTIVATE, 0, hit.index, 0, (struct vector2i){0});
    return true;
}

static bool over_bar(const struct ui_tabbar *tb, const struct ui_widget *widget,
                     struct vector2i pos)
{
    struct vector2i local = ui_widget_to_local(widget, pos);
    int h = tb_bar_height(tb, tb_css_height(tb), tb->impl.n_tabs);
    return local.y >= 0 && local.y < h;
}

static bool tb_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_tabbar *tb = tb_of(widget);
    if (event->kind == UI_EVENT_FOCUS_IN) {
        tb->impl.keyboard_index = active_tab(tb);
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
        return true;
    }
    if (event->kind == UI_EVENT_FOCUS_OUT) {
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
        return true;
    }
    if (event->kind == UI_EVENT_KEY)
        return tabbar_key(tb, &event->data.key);
    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        tb->impl.pressed_tab = -1;
        cancel_drag(tb);
        return true;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct ui_pointer_event *pe = &event->data.pointer;
    if (pe->kind == UI_POINTER_RELEASE && tb->impl.pressed_tab >= 0) {
        tb->impl.pressed_tab = -1;
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    }
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    const struct tb_theme *t = &theme;
    struct vector2i local = ui_widget_to_local(widget, pe->pos);
    int h = tb_bar_height(tb, t->height, tb->impl.n_tabs);
    bool on_bar = local.y >= 0 && local.y < h;
    switch (pe->kind) {
    case UI_POINTER_LEAVE:
        hover(tb, (struct tb_hit){ .index = -1 }, event->time_ms);
        if (!ui_widget_is_captured(widget))
            cancel_drag(tb);
        return true;
    case UI_POINTER_MOTION:
        if (tb->impl.drag_state != UI_TAB_DRAG_NONE)
            return drag_moved(tb, t, local, pe->pos);
        hover(tb, tb_hit_at(tb, t, tb->impl.n_tabs, tb->impl.size, local),
              event->time_ms);
        return over_bar(tb, widget, pe->pos);
    case UI_POINTER_PRESS:
        if (!on_bar)
            return false;
        {
            struct tb_hit hit = tb_hit_at(tb, t, tb->impl.n_tabs,
                                          tb->impl.size, local);
            if (hit.index >= 0 || hit.on_new) {
                focus_item(tb, hit.on_new ? tb->impl.n_tabs : hit.index,
                           false);
            }
        }
        return pressed(tb, t, pe, local, event->time_ms);
    case UI_POINTER_RELEASE:
        if (pe->button == 0 && tb->impl.drag_state != UI_TAB_DRAG_NONE)
            return drag_released(tb, t);
        return on_bar;
    default:
        return on_bar;
    }
}

static bool tb_hit_test(const struct ui_widget *widget, struct vector2i pos)
{
    const struct ui_tabbar *tb = tb_of(widget);
    if (ui_tabbar_is_dragging(tb))
        return true;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    struct tb_hit hit = tb_hit_at(tb, &theme, tb->impl.n_tabs, tb->impl.size,
                                  ui_widget_to_local(widget, pos));
    return hit.index >= 0 || hit.on_new;
}

static enum ui_cursor tb_cursor(const struct ui_widget *widget,
                                struct vector2i pos)
{
    const struct ui_tabbar *tb = tb_of(widget);
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    struct tb_hit hit = tb_hit_at(tb, &theme, tb->impl.n_tabs, tb->impl.size,
                                  ui_widget_to_local(widget, pos));
    if (hit.on_close || hit.on_new)
        return UI_CURSOR_POINTER;
    return hit.index >= 0 ? UI_CURSOR_DEFAULT : UI_CURSOR_AUTO;
}

static uint64_t tb_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_tabbar *tb = tb_of(widget);
    int css_height = tb_css_height(tb);
    int before = tb_bar_height(tb, css_height, tb->impl.n_tabs);
    bool animating = bar_tick(tb, tb->impl.size, now_ms);
    if (tb_bar_height(tb, css_height, tb->impl.n_tabs) != before)
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_MEASURE);
    else if (animating)
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    return animating ? now_ms : 0;
}

static struct vector2i tb_measure(const struct ui_widget *widget,
                                  struct ui_constraints constraints)
{
    const struct ui_tabbar *tb = tb_of(widget);
    return ui_constraints_clamp(constraints, (struct vector2i){
        .x = constraints.min.x,
        .y = tb_bar_height(tb, tb_css_height(tb), tb->impl.n_tabs),
    });
}

static void tb_layout(struct ui_widget *widget, struct rect2i bounds)
{
    tb_of(widget)->impl.size = bounds.size;
}

static void tb_paint(const struct ui_widget *widget,
                     struct ui_paint_context *paint)
{
    struct ui_tabbar *tb = tb_of(widget);
    if (tb->impl.n_tabs > 0) {
        struct tb_theme theme;
        tb_theme_read(tb, &theme);
        tb_scene_emit(tb, &theme, tb->impl.labels, tb->impl.n_tabs,
                      tb->impl.size, paint->scene, tb->impl.last_tick_ms);
    }
}

static void tb_debug_parts(const struct ui_widget *widget,
                           struct ui_debug_parts *out)
{
    struct ui_tabbar *tb = tb_of(widget);
    int n = tb->impl.n_tabs;
    struct vector2i size = tb->impl.size;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    int h = tb_bar_height(tb, theme.height, n);
    if (h <= 0)
        return;
    struct vector2i o = widget->bounds.pos;
    ui_debug_part(out, "bar", -1, rect2i_translate((struct rect2i){
        .size = { .x = size.x, .y = h },
    }, o));
    struct rect2i r;
    for (int i = 0; i < n; i++) {
        if (tb_tab_rect_at(tb, &theme, n, size, i, &r))
            ui_debug_part(out, "tab", i, rect2i_translate(r, o));
        if (tb_close_rect_at(tb, &theme, n, size, i, &r))
            ui_debug_part(out, "tab-close", i, rect2i_translate(r, o));
    }
    struct tb_layout l = tb_layout_for(tb, &theme, n, size);
    if (tb_drop_rect(tb, &l, size, &r))
        ui_debug_part(out, "drop-preview", -1, rect2i_translate(r, o));
    if (tb_new_tab_rect_at(tb, &theme, n, size, &r))
        ui_debug_part(out, "tab-new", -1, rect2i_translate(r, o));
    if (tb_drag_rect(tb, &l, n, size, &r))
        ui_debug_part(out, "tab-drag", -1, rect2i_translate(r, o));
}

static void free_labels(struct ui_tabbar *tb)
{
    for (int i = 0; i < tb->impl.labels_cap; i++)
        free((char *)tb->impl.labels[i].text);
    free(tb->impl.labels);
    tb->impl.labels = NULL;
    tb->impl.labels_cap = 0;
}

static void tb_destroy(struct ui_widget *widget)
{
    struct ui_tabbar *tb = tb_of(widget);
    free_labels(tb);
    free(tb->impl.tabs);
    tb->impl.tabs = NULL;
    tb->impl.tab_cap = tb->impl.capacity = 0;
}

static bool tabbar_accessibility(const struct ui_widget *widget,
                                 struct ui_accessibility_node_spec *out)
{
    const struct ui_tabbar *tb = tb_of(widget);
    if (tb->impl.n_tabs < 1 || rect2i_is_empty(widget->bounds))
        return false;
    out->role = UI_ACCESSIBILITY_ROLE_TAB_LIST;
    return true;
}

static size_t tabbar_accessibility_child_count(const struct ui_widget *widget)
{
    const struct ui_tabbar *tb = tb_of(widget);
    if (tb->impl.n_tabs < 1 || rect2i_is_empty(widget->bounds))
        return 0;
    return (size_t)tb->impl.n_tabs + 1;
}

static bool tabbar_accessibility_child(const struct ui_widget *widget,
                                       size_t index,
                                       struct ui_accessibility_node_spec *out)
{
    const struct ui_tabbar *tb = tb_of(widget);
    if (index < (size_t)tb->impl.n_tabs) {
        int tab_index = (int)index;
        struct rect2i bounds;
        if (!tb->impl.tabs || tab_index >= tb->impl.tab_cap ||
            !tb->impl.labels || tab_index >= tb->impl.labels_cap ||
            !ui_tabbar_tab_rect(tb, tab_index, &bounds) ||
            rect2i_is_empty(bounds))
            return false;
        out->id = tb->impl.tabs[tab_index].accessibility_id;
        out->role = UI_ACCESSIBILITY_ROLE_TAB;
        out->name = tb->impl.labels[tab_index].text;
        out->bounds = bounds;
        if (tb->impl.labels[tab_index].active)
            out->states |= UI_ACCESSIBILITY_STATE_SELECTED;
        if (ui_widget_has_focus(widget) &&
            tb->impl.keyboard_index == tab_index)
            out->states |= UI_ACCESSIBILITY_STATE_FOCUSED;
        out->actions |= UI_ACCESSIBILITY_ACTION_INVOKE;
        if (widget->impl.focusable) {
            out->states |= UI_ACCESSIBILITY_STATE_FOCUSABLE;
            out->actions |= UI_ACCESSIBILITY_ACTION_FOCUS;
        }
        return out->id != 0;
    }
    if (index != (size_t)tb->impl.n_tabs ||
        !tb->impl.new_accessibility_id)
        return false;
    struct rect2i bounds;
    if (!ui_tabbar_new_tab_rect(tb, &bounds) || rect2i_is_empty(bounds))
        return false;
    out->id = tb->impl.new_accessibility_id;
    out->role = UI_ACCESSIBILITY_ROLE_BUTTON;
    out->name = "New tab";
    out->bounds = bounds;
    if (ui_widget_has_focus(widget) &&
        tb->impl.keyboard_index == tb->impl.n_tabs)
        out->states |= UI_ACCESSIBILITY_STATE_FOCUSED;
    out->actions |= UI_ACCESSIBILITY_ACTION_INVOKE;
    if (widget->impl.focusable) {
        out->states |= UI_ACCESSIBILITY_STATE_FOCUSABLE;
        out->actions |= UI_ACCESSIBILITY_ACTION_FOCUS;
    }
    return true;
}

static bool tabbar_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_tabbar *tb = tb_of(widget);
    if (node_id == tb->impl.new_accessibility_id) {
        if (request->action == UI_ACCESSIBILITY_ACTION_FOCUS &&
            widget->impl.focusable) {
            ui_widget_focus(widget);
            focus_item(tb, tb->impl.n_tabs, false);
            return true;
        }
        if (request->action == UI_ACCESSIBILITY_ACTION_INVOKE) {
            if (widget->impl.focusable)
                ui_widget_focus(widget);
            focus_item(tb, tb->impl.n_tabs, true);
            return true;
        }
        return false;
    }
    int n = tb->impl.n_tabs < tb->impl.tab_cap
        ? tb->impl.n_tabs : tb->impl.tab_cap;
    for (int i = 0; i < n; i++) {
        if (tb->impl.tabs[i].accessibility_id != node_id)
            continue;
        if (request->action == UI_ACCESSIBILITY_ACTION_FOCUS &&
            widget->impl.focusable) {
            ui_widget_focus(widget);
            focus_item(tb, i, false);
            return true;
        }
        if (request->action == UI_ACCESSIBILITY_ACTION_INVOKE) {
            if (widget->impl.focusable)
                ui_widget_focus(widget);
            focus_item(tb, i, true);
            return true;
        }
        return false;
    }
    return false;
}

static const struct ui_widget_ops TABBAR_OPS = {
    .name = "tabbar",
    .measure = tb_measure,
    .layout = tb_layout,
    .event = tb_event,
    .hit_test = tb_hit_test,
    .cursor = tb_cursor,
    .tick = tb_tick,
    .paint = tb_paint,
    .debug_parts = tb_debug_parts,
    .destroy = tb_destroy,
    .accessibility = tabbar_accessibility,
    .accessibility_child_count = tabbar_accessibility_child_count,
    .accessibility_child = tabbar_accessibility_child,
    .accessibility_action = tabbar_accessibility_action,
};

void ui_tabbar_init(struct ui_tabbar *tb)
{
    if (!tb)
        return;
    *tb = (struct ui_tabbar){
        .impl.hover_tab = -1,
        .impl.hover_close = -1,
        .impl.drag_tab = -1,
        .impl.drag_target_slot = -1,
        .impl.pressed_tab = -1,
        .impl.external_slot = -1,
    };
    ui_widget_init(&tb->widget, &TABBAR_OPS, NULL);
    tb->impl.new_accessibility_id = ui_widget_accessibility_id_allocate();
    ui_widget_set_style_identity(&tb->widget, NULL, NULL, "tab-bar");
}

/* Tests if the tab bar holds this label set already. A NULL label set names
 * the tabs and leaves their text empty. */
static bool same_labels(const struct ui_tabbar *tb,
                        const struct ui_tab_label *tabs, int n)
{
    if (tb->impl.n_tabs != n)
        return false;
    for (int i = 0; i < n; i++) {
        const char *have = tb->impl.labels[i].text;
        const char *want = "";
        bool want_active = false;
        if (tabs) {
            want = tabs[i].text ? tabs[i].text : "";
            want_active = tabs[i].active;
        }
        if (tb->impl.labels[i].active != want_active || strcmp(have, want))
            return false;
    }
    return true;
}

static bool reserve_labels(struct ui_tabbar *tb, int n)
{
    size_t have = tb->impl.labels ? (size_t)tb->impl.labels_cap : 0;
    if ((size_t)n <= have)
        return true;
    size_t want = (size_t)n < 8 ? 8 : (size_t)n;
    struct ui_tab_label *labels = calloc(want, sizeof(*labels));
    if (!labels)
        return false;
    if (have)
        memcpy(labels, tb->impl.labels, have * sizeof(*labels));
    free(tb->impl.labels);
    tb->impl.labels = labels;
    tb->impl.labels_cap = (int)want;
    return true;
}

void ui_tabbar_set_tabs(struct ui_tabbar *tb, const struct ui_tab_label *tabs,
                        int n_tabs)
{
    if (!tb)
        return;
    int n = n_tabs > 0 ? n_tabs : 0;
    if (same_labels(tb, tabs, n))
        return;
    if (!reserve_labels(tb, n))
        return;
    for (int i = 0; i < n; i++) {
        const char *text = "";
        bool active = false;
        if (tabs) {
            text = tabs[i].text ? tabs[i].text : "";
            active = tabs[i].active;
        }
        struct ui_tab_label *l = &tb->impl.labels[i];
        if (!l->text || strcmp(l->text, text)) {
            char *copy = strdup(text);
            if (!copy)
                break;
            free((char *)l->text);
            l->text = copy;
        }
        l->active = active;
    }
    bool count_changed = tb->impl.n_tabs != n;
    tb->impl.n_tabs = n;
    tb_ensure_tabs(tb, n);
    ui_widget_set_focusable(&tb->widget, n >= 2);
    if (n > 0) {
        if (ui_widget_has_focus(&tb->widget))
            tb->impl.keyboard_index = active_tab(tb);
        else if (tb->impl.keyboard_index >= n)
            tb->impl.keyboard_index = n - 1;
    } else {
        tb->impl.keyboard_index = 0;
    }
    ui_widget_invalidate(&tb->widget, count_changed
        ? UI_WIDGET_INVALIDATE_MEASURE : UI_WIDGET_INVALIDATE_PAINT);
}

void ui_tabbar_set_bg_color(struct ui_tabbar *tb, uint32_t bg)
{
    if (!tb || tb->impl.bg_color == bg)
        return;
    tb->impl.bg_color = bg;
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
}

bool ui_tabbar_insert_tab(struct ui_tabbar *tb, int slot)
{
    if (!tb || slot < 0 || slot > tb->impl.tab_cap ||
        !tb_ensure_capacity(tb, tb->impl.tab_cap + 1))
        return false;
    memmove(&tb->impl.tabs[slot + 1], &tb->impl.tabs[slot],
            (size_t)(tb->impl.tab_cap - slot) * sizeof(*tb->impl.tabs));
    tb->impl.tabs[slot] = (struct ui_tab_state){
        .accessibility_id = ui_widget_accessibility_id_allocate(),
        .open = { .value = 1.0f, .to = 1.0f },
    };
    tb->impl.tab_cap++;
    if (slot <= tb->impl.keyboard_index)
        tb->impl.keyboard_index++;
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

bool ui_tabbar_remove_tab(struct ui_tabbar *tb, int index)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return false;
    if (tb->impl.drag_state != UI_TAB_DRAG_NONE) {
        drag_cancel(tb);
        ui_widget_release_pointer(&tb->widget);
    }
    memmove(&tb->impl.tabs[index], &tb->impl.tabs[index + 1],
            (size_t)(tb->impl.tab_cap - index - 1) * sizeof(*tb->impl.tabs));
    tb->impl.tab_cap--;
    tb->impl.tabs[tb->impl.tab_cap] = (struct ui_tab_state){0};
    /* The label count can not exceed the tab states. A stale count would
     * make the bar height wrong until the next label update. */
    if (tb->impl.n_tabs > tb->impl.tab_cap)
        tb->impl.n_tabs = tb->impl.tab_cap;
    if (tb->impl.keyboard_index > index)
        tb->impl.keyboard_index--;
    else if (tb->impl.keyboard_index == index &&
             tb->impl.keyboard_index >= tb->impl.tab_cap)
        tb->impl.keyboard_index = tb->impl.tab_cap > 0
            ? tb->impl.tab_cap - 1 : 0;
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

void ui_tabbar_move_tab(struct ui_tabbar *tb, int from, int to)
{
    if (!tb || from == to || from < 0 || to < 0 ||
        from >= tb->impl.tab_cap || to >= tb->impl.tab_cap)
        return;
    struct ui_tab_state st = tb->impl.tabs[from];
    if (from < to)
        memmove(&tb->impl.tabs[from], &tb->impl.tabs[from + 1],
                (size_t)(to - from) * sizeof(*tb->impl.tabs));
    else
        memmove(&tb->impl.tabs[to + 1], &tb->impl.tabs[to],
                (size_t)(from - to) * sizeof(*tb->impl.tabs));
    tb->impl.tabs[to] = st;
    if (tb->impl.keyboard_index == from)
        tb->impl.keyboard_index = to;
    else if (from < to && tb->impl.keyboard_index > from &&
             tb->impl.keyboard_index <= to)
        tb->impl.keyboard_index--;
    else if (from > to && tb->impl.keyboard_index >= to &&
             tb->impl.keyboard_index < from)
        tb->impl.keyboard_index++;
    ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
}

int ui_tabbar_height(const struct ui_tabbar *tb)
{
    if (!tb)
        return 0;
    return tb_bar_height(tb, tb_css_height(tb), tb->impl.n_tabs);
}

int ui_tabbar_steady_height(const struct ui_tabbar *tb)
{
    if (!tb)
        return 0;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return tb_steady_height(tb, &theme, tb->impl.n_tabs);
}

int ui_tabbar_bar_height(const struct ui_tabbar *tb)
{
    if (!tb)
        return 0;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return theme.height;
}

static bool to_surface(const struct ui_tabbar *tb, bool ok, struct rect2i *out)
{
    if (ok)
        *out = ui_widget_rect_to_surface(&tb->widget, *out);
    return ok;
}

bool ui_tabbar_tab_rect(const struct ui_tabbar *tb, int index,
                        struct rect2i *out)
{
    if (!tb)
        return false;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return to_surface(tb, tb_tab_rect_at(tb, &theme, tb->impl.n_tabs,
                                         tb->impl.size, index, out), out);
}

bool ui_tabbar_close_rect(const struct ui_tabbar *tb, int index,
                          struct rect2i *out)
{
    if (!tb)
        return false;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return to_surface(tb, tb_close_rect_at(tb, &theme, tb->impl.n_tabs,
                                           tb->impl.size, index, out), out);
}

bool ui_tabbar_new_tab_rect(const struct ui_tabbar *tb, struct rect2i *out)
{
    if (!tb)
        return false;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return to_surface(tb, tb_new_tab_rect_at(tb, &theme, tb->impl.n_tabs,
                                             tb->impl.size, out), out);
}

void ui_tabbar_set_external_drag(struct ui_tabbar *tb, bool active,
                                 struct vector2i pos)
{
    if (!tb)
        return;
    bool was_drag = tb->impl.external_drag;
    int was_slot = tb->impl.external_slot;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    external_drag_at(tb, &theme, active, ui_widget_to_local(&tb->widget, pos),
                     tb->impl.n_tabs, tb->impl.size);
    if (tb->impl.external_drag != was_drag)
        ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_MEASURE);
    else if (tb->impl.external_slot != was_slot)
        ui_widget_invalidate(&tb->widget, UI_WIDGET_INVALIDATE_PAINT);
}

int ui_tabbar_external_slot(const struct ui_tabbar *tb)
{
    return tb && tb->impl.external_drag ? tb->impl.external_slot : -1;
}
