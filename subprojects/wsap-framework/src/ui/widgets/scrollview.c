#include "ui/widgets/scrollview.h"

#include "ui/metrics.h"
#include "ui/debug.h"
#include "ui/paint.h"
#include "ui/css.h"
#include "ui/widget_ops.h"

#include <limits.h>
#include <math.h>

#define BAR_TRACK_IDLE 8
#define BAR_TRACK_HOVER 12
#define BAR_THUMB_MIN 24
#define BAR_THUMB_COLOR 0xFF7F849C
#define BAR_TRACK_HOVER_COLOR 0x1A7F849C
#define BAR_HIDE_AFTER_MS 900

enum bar_state { BAR_IDLE, BAR_HOVER, BAR_PRESSED };

struct scroll_parts {
    uint32_t epoch;
    ui_part_id track, thumb;
};

static struct scroll_parts scroll_parts_cache;

/* Returns the cached part handles. A handle from an earlier part table would
 * abort the name lookup, so the cache interns again after a table restart. */
static const struct scroll_parts *scroll_parts(void)
{
    uint32_t epoch = ui_part_epoch();
    if (scroll_parts_cache.epoch != epoch) {
        scroll_parts_cache.epoch = epoch;
        scroll_parts_cache.track = ui_part_intern("scrollbar");
        scroll_parts_cache.thumb = ui_part_intern("scrollbar-thumb");
    }
    return &scroll_parts_cache;
}

struct bar_skin {
    int track_w;
    int thumb_w;
    int thumb_min;
    struct insets2i margin;
    struct insets2i pad;
    struct ui_paint_box_style track_box;
    struct ui_paint_box_style thumb_box;
};

struct bar {
    struct rect2i track;
    struct rect2i thumb;
    int travel;
};

static struct ui_scrollview *sv_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_scrollview,
                           widget);
}

static struct ui_widget *sv_child(const struct ui_scrollview *sv)
{
    if (!sv)
        return NULL;
    for (struct ui_widget *c = sv->widget.first_child; c; c = c->next_sibling)
        if (c != &sv->bar)
            return c;
    return NULL;
}

static bool is_overlay(const struct ui_scrollview *sv)
{
    return !sv->state.fixed_bar;
}

static int sv_step(const struct ui_scrollview *sv)
{
    if (sv->state.scroll_step > 0)
        return sv->state.scroll_step;
    return ui_px(sv->widget.style_scale.dpi_scale, 3 * UI_DEFAULT_CELL_H);
}

static int sv_max_offset(const struct ui_scrollview *sv)
{
    if (sv->state.has_metrics)
        return 0;
    int64_t max = (int64_t)ui_scrollview_content_height(sv) -
                  sv->widget.bounds.height;
    return max > 0 ? geom_sat_i(max) : 0;
}

static struct bar_skin bar_skin(const struct ui_scrollview *sv, int state)
{
    const struct ui_widget *widget = &sv->widget;
    const struct css_scale_context *scale = &widget->style_scale;
    unsigned pseudo = state == BAR_HOVER ? CSS_PSEUDO_HOVER
                    : state == BAR_PRESSED ? CSS_PSEUDO_ACTIVE : CSS_PSEUDO_NONE;
    struct css_style track = ui_widget_part(widget, scroll_parts()->track, pseudo);
    struct css_style thumb = ui_widget_part(widget, scroll_parts()->thumb, pseudo);
    struct insets2i pad = ui_ctx_insets(scale, track.padding, 0);
    int track_w = ui_ctx_size(scale, track.width,
        ui_ctx_px(scale, state == BAR_IDLE ? BAR_TRACK_IDLE : BAR_TRACK_HOVER));
    int inner = track_w - pad.l - pad.r;
    float widget_opacity = css_style_effective_opacity(&widget->style);
    float track_opacity = widget_opacity * css_style_effective_opacity(&track);
    float thumb_opacity = widget_opacity * css_style_effective_opacity(&thumb);
    struct ui_paint_box_style track_box = ui_paint_box_style_of(&track, scale);
    track_box.bg_color = ui_color_opacity(
        track.has_bg_color ? track.bg_color
                           : (state == BAR_IDLE ? 0 : BAR_TRACK_HOVER_COLOR),
        track_opacity);
    track_box.border_color = ui_color_opacity(track_box.border_color,
                                              widget_opacity);
    struct ui_paint_box_style thumb_box = ui_paint_box_style_of(&thumb, scale);
    thumb_box.bg_color = ui_color_opacity(
        thumb.has_bg_color ? thumb.bg_color : BAR_THUMB_COLOR,
        thumb_opacity);
    thumb_box.border_color = ui_color_opacity(thumb_box.border_color,
                                              widget_opacity);
    return (struct bar_skin){
        .track_w = track_w,
        .thumb_w = ui_ctx_size(scale, thumb.width, (float)inner),
        .thumb_min = ui_ctx_size(scale, thumb.min_height,
                                 ui_ctx_px(scale, BAR_THUMB_MIN)),
        .margin = ui_ctx_insets(scale, track.margin, 0),
        .pad = pad,
        .track_box = track_box,
        .thumb_box = thumb_box,
    };
}

static struct ui_scroll_metrics bar_metrics(const struct ui_scrollview *sv)
{
    if (sv->state.has_metrics)
        return sv->state.metrics;
    int max = sv_max_offset(sv);
    return (struct ui_scroll_metrics){
        .history_lines = max,
        .visible_lines = sv->widget.bounds.height,
        .view_offset = max - sv->state.scroll_y,
    };
}

static bool bar_visible(const struct ui_scrollview *sv)
{
    return !sv->state.bar_off && (!is_overlay(sv) || sv->state.shown);
}

static int bar_reserve(const struct ui_scrollview *sv)
{
    if (sv->state.bar_off || is_overlay(sv))
        return 0;
    struct bar_skin s = bar_skin(sv, BAR_IDLE);
    return s.margin.l + s.track_w + s.margin.r;
}

static int geometry_state(const struct ui_scrollview *sv, int state)
{
    return is_overlay(sv) ? state : BAR_IDLE;
}

static bool bar_geometry(const struct ui_scrollview *sv, int state,
                         struct bar *out)
{
    if (sv->state.bar_off)
        return false;
    struct bar_skin s = bar_skin(sv, geometry_state(sv, state));
    struct ui_scroll_metrics m = bar_metrics(sv);
    struct vector2i size = sv->widget.bounds.size;
    int track_x = size.x - s.margin.r - s.track_w;
    int top = s.margin.t + s.pad.t;
    int run = size.y - top - s.margin.b - s.pad.b;
    int history = m.history_lines, rows = m.visible_lines;
    if (track_x < 0 || history <= 0 || rows <= 0 || run < 1)
        return false;
    int th = (int)((int64_t)run * rows / ((int64_t)history + rows));
    if (th < s.thumb_min)
        th = run < s.thumb_min ? run : s.thumb_min;
    int offset = m.view_offset < 0 ? 0
               : m.view_offset > history ? history : m.view_offset;
    int travel = run - th;
    int inner = s.track_w - s.pad.l - s.pad.r;
    int thumb_x = track_x + s.pad.l + (inner - s.thumb_w) / 2;
    if (thumb_x < track_x + s.pad.l)
        thumb_x = track_x + s.pad.l;
    *out = (struct bar){
        .track = {
            .pos = { .x = track_x, .y = s.margin.t },
            .size = { .x = s.track_w, .y = size.y - s.margin.t - s.margin.b },
        },
        .thumb = {
            .pos = { .x = thumb_x,
                     .y = top + (int)((int64_t)travel * (history - offset) / history) },
            .size = { .x = s.thumb_w, .y = th },
        },
        .travel = travel,
    };
    return true;
}

static void set_bar_state(struct ui_scrollview *sv, int state)
{
    if (sv->state.bar_state == state)
        return;
    sv->state.bar_state = state;
    if (state == BAR_IDLE)
        sv->state.hide_at = 0;
    else
        sv->state.shown = true;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

static void set_scroll(struct ui_scrollview *sv, int y)
{
    int clamped = geom_clamp_i(y, 0, sv_max_offset(sv));
    if (clamped == sv->state.scroll_y)
        return;
    sv->state.scroll_y = clamped;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

static void emit_offset(struct ui_scrollview *sv, int offset)
{
    if (offset == sv->state.metrics.view_offset)
        return;
    ui_widget_notify(&sv->widget, (struct ui_action){
        .kind = UI_ACTION_CHANGE, .index = offset,
    });
}

static void bar_drag_to(struct ui_scrollview *sv, int y)
{
    struct bar g = {0};
    int state = sv->state.bar_state;
    int travel = bar_geometry(sv, state, &g) ? g.travel : 0;
    struct bar_skin s = bar_skin(sv, geometry_state(sv, state));
    int top = travel > 0
        ? geom_clamp_i(y - sv->state.grab_y - s.margin.t - s.pad.t, 0, travel)
        : 0;
    if (sv->state.has_metrics) {
        int history = sv->state.metrics.history_lines;
        if (travel > 0)
            emit_offset(sv, history -
                (int)(((int64_t)top * history + travel / 2) / travel));
        return;
    }
    set_scroll(sv, travel > 0
        ? (int)((int64_t)top * sv_max_offset(sv) / travel) : 0);
}

static bool wheel(struct ui_scrollview *sv, const struct ui_pointer_event *p)
{
    if (sv->state.has_metrics || !rect2i_contains(sv->widget.bounds, p->pos) ||
        ui_pointer_is_horizontal_wheel(p))
        return false;
    int before = sv->state.scroll_y;
    float delta = p->scroll_delta != 0.0f
        ? p->scroll_delta : (float)p->scroll_lines;
    float pixels = sv->state.scroll_remainder - delta * sv_step(sv);
    const float limit = (float)(1 << 24);
    pixels = isnan(pixels) ? 0.0f : fminf(fmaxf(pixels, -limit), limit);
    int dy = (int)pixels;
    sv->state.scroll_remainder = pixels - (float)dy;
    ui_scrollview_scroll_by(sv, dy);
    return sv->state.scroll_y != before;
}

static int hit_state(const struct ui_scrollview *sv)
{
    return sv->state.bar_state == BAR_PRESSED ? BAR_PRESSED : BAR_HOVER;
}

static bool over_track(const struct ui_scrollview *sv, struct vector2i local,
                       int state)
{
    struct bar g = {0};
    return bar_geometry(sv, state, &g) && rect2i_contains(g.track, local);
}

static bool forward_to_child(struct ui_scrollview *sv,
                             const struct ui_event *event)
{
    struct ui_widget *child = sv_child(sv);
    return child && !ui_widget_is_captured(child) &&
           ui_widget_route_pointer(child, event);
}

static bool scrollview_event(struct ui_widget *widget,
                             const struct ui_event *event)
{
    struct ui_scrollview *sv = widget ? sv_of(widget) : NULL;
    if (!sv || !event)
        return false;
    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        set_bar_state(sv, BAR_IDLE);
        return true;
    }
    if (event->kind == UI_EVENT_KEY &&
        (event->data.key.key == UI_KEY_PAGE_UP || event->data.key.key == UI_KEY_PAGE_DOWN)) {
        if (sv->state.has_metrics)
            return false;
        ui_scrollview_scroll_by(sv, (event->data.key.key == UI_KEY_PAGE_UP ? -1 : 1) *
                                    widget->bounds.height);
        return true;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct ui_pointer_event *p = &event->data.pointer;
    struct vector2i local = ui_widget_to_local(widget, p->pos);
    bool captured = ui_widget_is_captured(widget);
    struct bar g = {0};
    switch (p->kind) {
    case UI_POINTER_LEAVE:
        if (!captured)
            set_bar_state(sv, BAR_IDLE);
        return false;
    case UI_POINTER_MOTION:
        if (captured) {
            bar_drag_to(sv, local.y);
            return true;
        }
        if (!over_track(sv, local, hit_state(sv))) {
            set_bar_state(sv, BAR_IDLE);
            return false;
        }
        set_bar_state(sv, BAR_HOVER);
        return forward_to_child(sv, event);
    case UI_POINTER_PRESS:
        if (!over_track(sv, local, hit_state(sv)))
            return false;
        if (!bar_visible(sv) || p->button != 0 || (p->mods & UI_MOD_SHIFT))
            return !captured && forward_to_child(sv, event);
        set_bar_state(sv, BAR_PRESSED);
        bar_geometry(sv, BAR_PRESSED, &g);
        sv->state.grab_y = local.y >= g.thumb.y && local.y < rect2i_end(g.thumb).y
            ? local.y - g.thumb.y : g.thumb.height / 2;
        ui_widget_capture_pointer(widget);
        bar_drag_to(sv, local.y);
        return true;
    case UI_POINTER_RELEASE:
        if (!captured)
            return over_track(sv, local, hit_state(sv)) &&
                   forward_to_child(sv, event);
        if (p->button != 0)
            return false;
        ui_widget_release_pointer(widget);
        set_bar_state(sv, over_track(sv, local, BAR_HOVER) ? BAR_HOVER : BAR_IDLE);
        return true;
    case UI_POINTER_SCROLL:
        if ((sv->state.has_metrics || !bar_visible(sv)) &&
            over_track(sv, local, hit_state(sv)) &&
            (forward_to_child(sv, event) || ui_widget_is_destroyed(widget)))
            return true;
        return wheel(sv, p);
    default:
        return false;
    }
}

static enum ui_cursor scrollview_cursor(const struct ui_widget *widget,
                                        struct vector2i pos)
{
    const struct ui_scrollview *sv = ui_container_of_const(widget, struct ui_scrollview, widget);
    struct ui_widget *child = sv_child(sv);
    if (!child || !child->ops || !child->ops->cursor)
        return UI_CURSOR_AUTO;
    if (ui_widget_is_captured(widget) ||
        over_track(sv, ui_widget_to_local(widget, pos), hit_state(sv)))
        return UI_CURSOR_AUTO;
    if (!rect2i_contains(child->bounds, pos))
        return UI_CURSOR_AUTO;
    return child->ops->cursor(child, pos);
}

/* The scroll bar takes the pointer and paints. Every event still bubbles to
 * the scroll view, which owns the drag. A hover leave goes to the bar alone,
 * so the bar drops its hover state here. */
static bool bar_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_scrollview *sv = widget->userdata;
    if (event->kind == UI_EVENT_POINTER &&
        event->data.pointer.kind == UI_POINTER_LEAVE &&
        !ui_widget_is_captured(&sv->widget))
        set_bar_state(sv, BAR_IDLE);
    return false;
}

static void bar_paint(const struct ui_widget *widget,
                      struct ui_paint_context *paint)
{
    struct ui_scrollview *sv = widget->userdata;
    struct bar g = {0};
    if (!bar_visible(sv) || !bar_geometry(sv, sv->state.bar_state, &g))
        return;
    struct bar_skin s = bar_skin(sv, sv->state.bar_state);
    struct rect2i track = { .size = widget->bounds.size };
    if (!ui_paint_box(paint, track, &s.track_box, NULL))
        return;
    struct rect2i thumb = rect2i_translate(g.thumb, (struct vector2i){
        .x = -g.track.pos.x, .y = -g.track.pos.y,
    });
    ui_paint_box(paint, thumb, &s.thumb_box, NULL);
}

/* The bar owns the track and the thumb, so the parts have one owner. */
static void bar_parts(const struct ui_widget *widget, struct ui_debug_parts *out)
{
    struct ui_scrollview *sv = widget->userdata;
    struct bar g = {0};
    if (!bar_visible(sv) || !bar_geometry(sv, sv->state.bar_state, &g))
        return;
    ui_debug_part(out, "scroll-track", -1, widget->bounds);
    struct rect2i thumb = rect2i_translate(g.thumb, (struct vector2i){
        .x = widget->bounds.pos.x - g.track.pos.x,
        .y = widget->bounds.pos.y - g.track.pos.y,
    });
    ui_debug_part(out, "scroll-thumb", -1, thumb);
}

static const struct ui_widget_ops BAR_OPS = {
    .name = "scrollbar",
    .event = bar_event,
    .paint = bar_paint,
    .debug_parts = bar_parts,
};

static uint64_t scrollview_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_scrollview *sv = sv_of(widget);
    if (!is_overlay(sv) || !sv->state.shown || sv->state.bar_state != BAR_IDLE)
        return 0;
    if (!sv->state.hide_at)
        sv->state.hide_at = now_ms + BAR_HIDE_AFTER_MS;
    if (now_ms < sv->state.hide_at)
        return sv->state.hide_at;
    sv->state.shown = false;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_LAYOUT);
    return 0;
}

static struct vector2i scrollview_measure(
    const struct ui_widget *widget, struct ui_constraints constraints)
{
    const struct ui_scrollview *sv = widget
        ? ui_container_of_const(widget, struct ui_scrollview, widget) : NULL;
    struct ui_widget *child = sv ? sv_child(sv) : NULL;
    struct vector2i content = {0};
    if (child) {
        int tw = bar_reserve(sv);
        int max_x = geom_clamp_i(geom_sat_i((int64_t)constraints.max.x - tw), 0, constraints.max.x);
        content = ui_widget_measure(child, (struct ui_constraints){
            .min = { .x = 0, .y = 0 },
            .max = { .x = max_x, .y = INT_MAX },
        });
        content.x = geom_sat_i((int64_t)content.x + tw);
    }
    return ui_constraints_clamp(constraints, content);
}

/* Keeps the bar as the last child, hides it when it has no geometry, and
 * gives it the track rectangle. */
static void sync_bar(struct ui_scrollview *sv)
{
    if (sv->widget.last_child != &sv->bar)
        ui_widget_insert_child(&sv->widget, &sv->bar, NULL);
    struct bar g = {0};
    /* The bar takes the pointer where the track is, also while the overlay
     * bar auto-hides. A hover over the track then shows it again. */
    bool placed = bar_geometry(sv, sv->state.bar_state, &g);
    ui_widget_set_hidden(&sv->bar, !placed);
    if (placed)
        ui_widget_place(&sv->bar,
                        rect2i_translate(g.track, sv->widget.bounds.pos));
}

static void scrollview_prepare(struct ui_widget *widget)
{
    struct ui_scrollview *sv = sv_of(widget);
    if (sv)
        sync_bar(sv);
}

static void scrollview_layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct ui_scrollview *sv = widget
        ? ui_container_of(widget, struct ui_scrollview, widget) : NULL;
    if (!sv)
        return;
    struct ui_widget *child = sv_child(sv);
    if (child) {
        int tw = bar_reserve(sv);
        int child_w = geom_clamp_i(geom_sat_i((int64_t)bounds.width - tw), 0, bounds.width);
        if (child->measured_size.x != child_w || ui_widget_needs_measure(child)) {
            ui_widget_measure(child, (struct ui_constraints){
                .min = { .x = child_w, .y = 0 },
                .max = { .x = child_w, .y = INT_MAX },
            });
        }
    }
    sv->state.scroll_y = geom_clamp_i(sv->state.scroll_y, 0, sv_max_offset(sv));
    if (!child)
        return;
    int tw = bar_reserve(sv);
    int child_w = geom_clamp_i(geom_sat_i((int64_t)bounds.width - tw), 0, bounds.width);
    int child_h = sv->state.has_metrics ? bounds.height : child->measured_size.y;
    ui_widget_place(child, (struct rect2i){
        .pos = { .x = bounds.pos.x,
                 .y = geom_sat_i((int64_t)bounds.pos.y - sv->state.scroll_y) },
        .size = { .x = child_w, .y = child_h },
    });
    sync_bar(sv);
}

static void scrollview_parts(const struct ui_widget *widget, struct ui_debug_parts *out)
{
    ui_debug_part(out, "content", -1, widget->bounds);
}

static void scrollview_reveal(struct ui_widget *widget, struct ui_widget *descendant)
{
    struct ui_scrollview *sv = ui_container_of(widget, struct ui_scrollview, widget);
    struct rect2i row = descendant->bounds;
    struct rect2i view = widget->bounds;
    if (row.height > view.height) return;
    int dy = row.y < view.y ? row.y - view.y :
        row.y + row.height > view.y + view.height
        ? row.y + row.height - view.y - view.height : 0;
    ui_scrollview_scroll_by(sv, dy);
}

static const struct ui_widget_ops SCROLLVIEW_OPS = {
    .name = "scrollview",
    .prepare = scrollview_prepare,
    .measure = scrollview_measure,
    .layout = scrollview_layout,
    .event = scrollview_event,
    .tick = scrollview_tick,
    .reveal = scrollview_reveal,
    .cursor = scrollview_cursor,
    .debug_parts = scrollview_parts,
    .clip_children = true,
    /* The content child and the bar. */
    .child_limit = 2,
};

static void scrollview_ensure_init(struct ui_scrollview *sv)
{
    if (!sv || sv->widget.ops)
        return;
    ui_widget_init(&sv->widget, &SCROLLVIEW_OPS, NULL);
    ui_widget_init(&sv->bar, &BAR_OPS, sv);
    ui_widget_set_keeps_press_focus(&sv->bar, true);
    ui_widget_set_accessibility_ignored(&sv->bar, true);
    ui_widget_set_hidden(&sv->bar, true);
    ui_widget_set_style_identity(&sv->widget, "scrollview", NULL, "scrollview");
    /* The bar takes one child slot now. The content takes the other. */
    ui_widget_add_child(&sv->widget, &sv->bar);
}

void ui_scrollview_init(struct ui_scrollview *sv)
{
    if (!sv)
        return;
    *sv = (struct ui_scrollview){0};
    scrollview_ensure_init(sv);
}

struct ui_widget *ui_scrollview_child(const struct ui_scrollview *sv)
{
    return sv_child(sv);
}

int ui_scrollview_offset(const struct ui_scrollview *sv)
{
    return sv ? sv->state.scroll_y : 0;
}

int ui_scrollview_content_height(const struct ui_scrollview *sv)
{
    struct ui_widget *child = sv_child(sv);
    return child ? child->measured_size.y : 0;
}

int ui_scrollview_viewport_height(const struct ui_scrollview *sv)
{
    return sv ? sv->widget.bounds.height : 0;
}

void ui_scrollview_scroll_to(struct ui_scrollview *sv, int y)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    set_scroll(sv, y);
}

void ui_scrollview_scroll_by(struct ui_scrollview *sv, int dy)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    int before = sv->state.scroll_y;
    set_scroll(sv, geom_sat_i((int64_t)sv->state.scroll_y + dy));
    if (sv->state.scroll_y != before && is_overlay(sv))
        ui_scrollview_flash(sv);
}

void ui_scrollview_set_scroll_step(struct ui_scrollview *sv, int px)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    sv->state.scroll_step = px;
}

void ui_scrollview_set_scrollbar(struct ui_scrollview *sv, bool enabled)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    if (sv->state.bar_off != !enabled) {
        sv->state.bar_off = !enabled;
        if (!enabled) {
            ui_widget_release_pointer(&sv->widget);
            set_bar_state(sv, BAR_IDLE);
            sv->state.shown = false;
        }
        ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_MEASURE);
    }
}

void ui_scrollview_set_metrics(struct ui_scrollview *sv,
                               struct ui_scroll_metrics metrics)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    struct ui_scroll_metrics *old = &sv->state.metrics;
    bool first = !sv->state.has_metrics;
    if (!first && old->history_lines == metrics.history_lines &&
        old->visible_lines == metrics.visible_lines &&
        old->view_offset == metrics.view_offset)
        return;
    sv->state.has_metrics = true;
    sv->state.metrics = metrics;
    if (first) {
        sv->state.scroll_y = 0;
        sv->state.scroll_remainder = 0.0f;
        ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
    } else if (bar_visible(sv)) {
        ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
    }
}

void ui_scrollview_clear_metrics(struct ui_scrollview *sv)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    if (!sv->state.has_metrics)
        return;
    ui_widget_release_pointer(&sv->widget);
    set_bar_state(sv, BAR_IDLE);
    sv->state.has_metrics = false;
    sv->state.metrics = (struct ui_scroll_metrics){0};
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

void ui_scrollview_set_overlay(struct ui_scrollview *sv, bool overlay)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    if (is_overlay(sv) == overlay)
        return;
    ui_widget_release_pointer(&sv->widget);
    set_bar_state(sv, BAR_IDLE);
    sv->state.fixed_bar = !overlay;
    sv->state.shown = false;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_scrollview_flash(struct ui_scrollview *sv)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    sv->state.shown = true;
    sv->state.hide_at = 0;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

void ui_scrollview_hide(struct ui_scrollview *sv)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    if (!sv->state.shown && sv->state.bar_state == BAR_IDLE)
        return;
    sv->state.shown = false;
    sv->state.bar_state = BAR_IDLE;
    sv->state.hide_at = 0;
    ui_widget_release_pointer(&sv->widget);
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

bool ui_scrollview_is_shown(const struct ui_scrollview *sv)
{
    return sv && bar_visible(sv);
}
