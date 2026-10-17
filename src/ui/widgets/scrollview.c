#include "ui/widgets/scrollview.h"

#include "ui/metrics.h"
#include "ui/debug.h"
#include "ui/internal/tree.h"
#include "ui/paint.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"

#include <limits.h>
#include <math.h>

#define BAR_TRACK_IDLE 8
#define BAR_TRACK_HOVER 12
#define BAR_THUMB_MIN 24
#define BAR_THUMB_COLOR 0xFF7F849C
#define BAR_TRACK_HOVER_COLOR 0x1A7F849C
#define BAR_HIDE_AFTER_MS 900

enum bar_state { BAR_IDLE, BAR_HOVER, BAR_PRESSED };

static struct {
    ui_part_id track, thumb;
} sv_parts;

static void sv_parts_init(void)
{
    if (sv_parts.thumb)
        return;
    sv_parts.track = ui_part_intern("scrollbar");
    sv_parts.thumb = ui_part_intern("scrollbar-thumb");
}

struct bar_skin {
    int track_w;
    int thumb_w;
    int thumb_min;
    int thumb_radius;
    struct insets2i margin;
    struct insets2i pad;
    uint32_t track_color;
    uint32_t thumb_color;
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
    return sv ? sv->widget.first_child : NULL;
}

static bool is_overlay(const struct ui_scrollview *sv)
{
    return !sv->impl.fixed_bar;
}

static int sv_step(const struct ui_scrollview *sv)
{
    if (sv->impl.scroll_step > 0)
        return sv->impl.scroll_step;
    return ui_px(sv->widget.style_scale.dpi_scale, 3 * UI_DEFAULT_CELL_H);
}

static int sv_max_offset(const struct ui_scrollview *sv)
{
    if (sv->impl.has_metrics)
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
    struct css_style track = ui_widget_part(widget, sv_parts.track, pseudo);
    struct css_style thumb = ui_widget_part(widget, sv_parts.thumb, pseudo);
    struct insets2i pad = ui_ctx_insets(scale, track.padding, 0);
    int track_w = ui_ctx_size(scale, track.width,
        ui_ctx_px(scale, state == BAR_IDLE ? BAR_TRACK_IDLE : BAR_TRACK_HOVER));
    int inner = track_w - pad.l - pad.r;
    return (struct bar_skin){
        .track_w = track_w,
        .thumb_w = ui_ctx_size(scale, thumb.width, (float)inner),
        .thumb_min = ui_ctx_size(scale, thumb.min_height,
                                 ui_ctx_px(scale, BAR_THUMB_MIN)),
        .thumb_radius = ui_ctx_size(scale, thumb.border_radius, 0),
        .margin = ui_ctx_insets(scale, track.margin, 0),
        .pad = pad,
        .track_color = track.has_bg_color
            ? ui_color_opacity(track.bg_color, track.opacity)
            : state == BAR_IDLE ? 0 : BAR_TRACK_HOVER_COLOR,
        .thumb_color = thumb.has_bg_color
            ? ui_color_opacity(thumb.bg_color, thumb.opacity) : BAR_THUMB_COLOR,
    };
}

static struct ui_scroll_metrics bar_metrics(const struct ui_scrollview *sv)
{
    if (sv->impl.has_metrics)
        return sv->impl.metrics;
    int max = sv_max_offset(sv);
    return (struct ui_scroll_metrics){
        .history_lines = max,
        .visible_lines = sv->widget.bounds.height,
        .view_offset = max - sv->impl.scroll_y,
    };
}

static bool bar_visible(const struct ui_scrollview *sv)
{
    return !sv->impl.bar_off && (!is_overlay(sv) || sv->impl.shown);
}

static int bar_reserve(const struct ui_scrollview *sv)
{
    if (sv->impl.bar_off || is_overlay(sv))
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
    if (sv->impl.bar_off)
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
    if (sv->impl.bar_state == state)
        return;
    sv->impl.bar_state = state;
    if (state == BAR_IDLE)
        sv->impl.hide_at = 0;
    else
        sv->impl.shown = true;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_PAINT);
}

static void set_scroll(struct ui_scrollview *sv, int y)
{
    int clamped = geom_clamp_i(y, 0, sv_max_offset(sv));
    if (clamped == sv->impl.scroll_y)
        return;
    sv->impl.scroll_y = clamped;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

static void emit_offset(struct ui_scrollview *sv, int offset)
{
    if (offset == sv->impl.metrics.view_offset)
        return;
    ui_widget_notify(&sv->widget, (struct ui_action){
        .kind = UI_ACTION_CHANGE, .index = offset,
    });
}

static void bar_drag_to(struct ui_scrollview *sv, int y)
{
    struct bar g = {0};
    int state = sv->impl.bar_state;
    int travel = bar_geometry(sv, state, &g) ? g.travel : 0;
    struct bar_skin s = bar_skin(sv, geometry_state(sv, state));
    int top = travel > 0
        ? geom_clamp_i(y - sv->impl.grab_y - s.margin.t - s.pad.t, 0, travel)
        : 0;
    if (sv->impl.has_metrics) {
        int history = sv->impl.metrics.history_lines;
        if (travel > 0)
            emit_offset(sv, history -
                (int)(((int64_t)top * history + travel / 2) / travel));
        return;
    }
    set_scroll(sv, travel > 0
        ? (int)((int64_t)top * sv_max_offset(sv) / travel) : 0);
}

static bool wheel(struct ui_scrollview *sv, const struct pointer_event *p)
{
    if (sv->impl.has_metrics || !rect2i_contains(sv->widget.bounds, p->pos) ||
        pointer_wheel_is_horizontal(p->button))
        return false;
    int before = sv->impl.scroll_y;
    float delta = p->scroll_delta != 0.0f
        ? p->scroll_delta : (float)p->scroll_lines;
    float pixels = sv->impl.scroll_remainder - delta * sv_step(sv);
    const float limit = (float)(1 << 24);
    pixels = isnan(pixels) ? 0.0f : fminf(fmaxf(pixels, -limit), limit);
    int dy = (int)pixels;
    sv->impl.scroll_remainder = pixels - (float)dy;
    ui_scrollview_scroll_by(sv, dy);
    return sv->impl.scroll_y != before;
}

static int hit_state(const struct ui_scrollview *sv)
{
    return sv->impl.bar_state == BAR_PRESSED ? BAR_PRESSED : BAR_HOVER;
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
    if (!child || ui_widget_is_captured(child))
        return false;
    struct ui_widget *w = ui_widget_hit_test(child, event->data.pointer.pos);
    for (; w && w != &sv->widget; w = w->parent) {
        if (w->impl.destroyed)
            return false;
        if (w->ops && w->ops->event && w->ops->event(w, event))
            return true;
        if (w->impl.destroyed)
            return false;
        if (w->impl.event_fn &&
            w->impl.event_fn(w, event, w->impl.event_userdata))
            return true;
    }
    return false;
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
        if (sv->impl.has_metrics)
            return false;
        ui_scrollview_scroll_by(sv, (event->data.key.key == UI_KEY_PAGE_UP ? -1 : 1) *
                                    widget->bounds.height);
        return true;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct pointer_event *p = &event->data.pointer;
    struct vector2i local = ui_widget_to_local(widget, p->pos);
    bool captured = ui_widget_is_captured(widget);
    struct bar g = {0};
    switch (p->kind) {
    case PTR_LEAVE:
        if (!captured)
            set_bar_state(sv, BAR_IDLE);
        return false;
    case PTR_MOTION:
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
    case PTR_PRESS:
        if (!over_track(sv, local, hit_state(sv)))
            return false;
        if (!bar_visible(sv) || p->button != 0 || (p->mods & UI_MOD_SHIFT))
            return !captured && forward_to_child(sv, event);
        set_bar_state(sv, BAR_PRESSED);
        bar_geometry(sv, BAR_PRESSED, &g);
        sv->impl.grab_y = local.y >= g.thumb.y && local.y < rect2i_end(g.thumb).y
            ? local.y - g.thumb.y : g.thumb.height / 2;
        ui_widget_capture_pointer(widget);
        bar_drag_to(sv, local.y);
        return true;
    case PTR_RELEASE:
        if (!captured)
            return over_track(sv, local, hit_state(sv)) &&
                   forward_to_child(sv, event);
        if (p->button != 0)
            return false;
        ui_widget_release_pointer(widget);
        set_bar_state(sv, over_track(sv, local, BAR_HOVER) ? BAR_HOVER : BAR_IDLE);
        return true;
    case PTR_SCROLL:
        if ((sv->impl.has_metrics || !bar_visible(sv)) &&
            over_track(sv, local, hit_state(sv)) && forward_to_child(sv, event))
            return true;
        return wheel(sv, p);
    default:
        return false;
    }
}

static bool scrollview_hit_front(const struct ui_widget *widget,
                                 struct vector2i pos)
{
    const struct ui_scrollview *sv = ui_container_of_const(widget, struct ui_scrollview, widget);
    return over_track(sv, ui_widget_to_local(widget, pos), hit_state(sv));
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

static bool scrollview_keeps_press_focus(const struct ui_widget *widget,
                                         struct vector2i pos)
{
    const struct ui_scrollview *sv = ui_container_of_const(widget, struct ui_scrollview, widget);
    return over_track(sv, ui_widget_to_local(widget, pos), hit_state(sv));
}

static void scrollview_paint_front(const struct ui_widget *widget,
                                   struct ui_paint_context *paint)
{
    const struct ui_scrollview *sv = ui_container_of_const(widget, struct ui_scrollview, widget);
    struct bar g = {0};
    if (!bar_visible(sv) || !bar_geometry(sv, sv->impl.bar_state, &g))
        return;
    struct bar_skin s = bar_skin(sv, sv->impl.bar_state);
    if (s.track_color)
        ui_paint_rect(paint, g.track, s.track_color);
    if (!s.thumb_color)
        return;
    if (s.thumb_radius > 0)
        ui_paint_rounded_rect(paint, g.thumb, s.thumb_radius, s.thumb_color);
    else
        ui_paint_rect(paint, g.thumb, s.thumb_color);
}

static uint64_t scrollview_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_scrollview *sv = sv_of(widget);
    if (!is_overlay(sv) || !sv->impl.shown || sv->impl.bar_state != BAR_IDLE)
        return 0;
    if (!sv->impl.hide_at)
        sv->impl.hide_at = now_ms + BAR_HIDE_AFTER_MS;
    if (now_ms < sv->impl.hide_at)
        return sv->impl.hide_at;
    sv->impl.shown = false;
    ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
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
        if (child->measured_size.x != child_w || (child->impl.invalidation & UI_WIDGET_INVALIDATE_MEASURE)) {
            ui_widget_measure(child, (struct ui_constraints){
                .min = { .x = child_w, .y = 0 },
                .max = { .x = child_w, .y = INT_MAX },
            });
        }
    }
    sv->impl.scroll_y = geom_clamp_i(sv->impl.scroll_y, 0, sv_max_offset(sv));
    if (!child)
        return;
    int tw = bar_reserve(sv);
    int child_w = geom_clamp_i(geom_sat_i((int64_t)bounds.width - tw), 0, bounds.width);
    int child_h = sv->impl.has_metrics ? bounds.height : child->measured_size.y;
    ui_widget_place(child, (struct rect2i){
        .pos = { .x = bounds.pos.x,
                 .y = geom_sat_i((int64_t)bounds.pos.y - sv->impl.scroll_y) },
        .size = { .x = child_w, .y = child_h },
    });
}

static void scrollview_parts(const struct ui_widget *widget, struct ui_debug_parts *out)
{
    const struct ui_scrollview *sv = ui_container_of_const(widget, struct ui_scrollview, widget);
    ui_debug_part(out, "content", -1, widget->bounds);
    struct bar g = {0};
    if (!bar_visible(sv) || !bar_geometry(sv, sv->impl.bar_state, &g))
        return;
    ui_debug_part(out, "scroll-track", -1, ui_widget_rect_to_surface(widget, g.track));
    ui_debug_part(out, "scroll-thumb", -1, ui_widget_rect_to_surface(widget, g.thumb));
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
    .measure = scrollview_measure,
    .layout = scrollview_layout,
    .event = scrollview_event,
    .hit_front = scrollview_hit_front,
    .paint_front = scrollview_paint_front,
    .tick = scrollview_tick,
    .reveal = scrollview_reveal,
    .cursor = scrollview_cursor,
    .keeps_press_focus = scrollview_keeps_press_focus,
    .debug_parts = scrollview_parts,
    .clip_children = true,
    .child_limit = 1,
};

static void scrollview_ensure_init(struct ui_scrollview *sv)
{
    if (!sv || sv->widget.ops)
        return;
    sv_parts_init();
    ui_widget_init(&sv->widget, &SCROLLVIEW_OPS, NULL);
    ui_widget_set_style_identity(&sv->widget, "scrollview", NULL, "scrollview");
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
    return sv ? sv->impl.scroll_y : 0;
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
    int before = sv->impl.scroll_y;
    set_scroll(sv, geom_sat_i((int64_t)sv->impl.scroll_y + dy));
    if (sv->impl.scroll_y != before && is_overlay(sv))
        ui_scrollview_flash(sv);
}

void ui_scrollview_set_scroll_step(struct ui_scrollview *sv, int px)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    sv->impl.scroll_step = px;
}

void ui_scrollview_set_scrollbar(struct ui_scrollview *sv, bool enabled)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    if (sv->impl.bar_off != !enabled) {
        sv->impl.bar_off = !enabled;
        if (!enabled) {
            ui_widget_release_pointer(&sv->widget);
            set_bar_state(sv, BAR_IDLE);
            sv->impl.shown = false;
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
    struct ui_scroll_metrics *old = &sv->impl.metrics;
    bool first = !sv->impl.has_metrics;
    if (!first && old->history_lines == metrics.history_lines &&
        old->visible_lines == metrics.visible_lines &&
        old->view_offset == metrics.view_offset)
        return;
    sv->impl.has_metrics = true;
    sv->impl.metrics = metrics;
    if (first) {
        sv->impl.scroll_y = 0;
        sv->impl.scroll_remainder = 0.0f;
        ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_LAYOUT);
    } else if (bar_visible(sv)) {
        ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_PAINT);
    }
}

void ui_scrollview_clear_metrics(struct ui_scrollview *sv)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    if (!sv->impl.has_metrics)
        return;
    ui_widget_release_pointer(&sv->widget);
    set_bar_state(sv, BAR_IDLE);
    sv->impl.has_metrics = false;
    sv->impl.metrics = (struct ui_scroll_metrics){0};
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
    sv->impl.fixed_bar = !overlay;
    sv->impl.shown = false;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_scrollview_flash(struct ui_scrollview *sv)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    sv->impl.shown = true;
    sv->impl.hide_at = 0;
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_PAINT);
}

void ui_scrollview_hide(struct ui_scrollview *sv)
{
    if (!sv)
        return;
    scrollview_ensure_init(sv);
    if (!sv->impl.shown && sv->impl.bar_state == BAR_IDLE)
        return;
    sv->impl.shown = false;
    sv->impl.bar_state = BAR_IDLE;
    sv->impl.hide_at = 0;
    ui_widget_release_pointer(&sv->widget);
    ui_widget_invalidate(&sv->widget, UI_WIDGET_INVALIDATE_PAINT);
}

bool ui_scrollview_is_shown(const struct ui_scrollview *sv)
{
    return sv && bar_visible(sv);
}
