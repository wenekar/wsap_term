#include "ui/widgets/tab_preview.h"

#include "ui/debug.h"
#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/surface.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define SHOW_DELAY_MS 500
#define REFRESH_MS    500
#define BELOW_GAP_PX  6

struct ui_tab_preview_skin {
    struct insets2i pad;
    struct vector2i shadow_offset;
    uint32_t background;
    uint32_t border_color;
    uint32_t shadow_color;
    int shadow_blur;
    int border;
    int radius;
    int gap;
    int width;
};

static struct {
    ui_part_id card[2];
    ui_part_id title;
} parts;

static void tab_preview_parts_init(void)
{
    if (parts.card[0])
        return;
    parts.card[0] = ui_part_intern("tab-preview");
    parts.card[1] = ui_part_intern("tab-preview dragging");
    parts.title = ui_part_intern("tab-preview-title");
}

static struct ui_tab_preview *preview_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_tab_preview,
                           widget);
}

static struct ui_tab_preview_skin skin_of(const struct ui_widget *widget,
                                          bool dragging)
{
    const struct css_scale_context *scale = &widget->style_scale;
    struct css_style card = ui_widget_part(widget, parts.card[dragging ? 1 : 0], 0);
    return (struct ui_tab_preview_skin){
        .border = card.has_border_color
            ? ui_ctx_inset(scale, card.border_width, 0) : 0,
        .radius = ui_ctx_size(scale, card.border_radius, 0),
        .pad = ui_ctx_insets(scale, card.padding, 0),
        .gap = ui_ctx_inset(scale, card.gap, 0),
        .width = ui_ctx_size(scale, card.width, 400),
        .background = card.has_bg_color ? card.bg_color : 0,
        .border_color = card.has_border_color ? card.border_color : 0,
        .shadow_color = card.has_shadow ? card.shadow_color : 0,
        .shadow_blur = card.has_shadow
            ? ui_css_resolve_int(card.shadow_blur, 0, 0, scale, true) : 0,
        .shadow_offset = card.has_shadow
            ? (struct vector2i){
                  .x = ui_css_resolve_int(card.shadow_x, 0, 0, scale, false),
                  .y = ui_css_resolve_int(card.shadow_y, 0, 0, scale, false),
              } : (struct vector2i){},
    };
}

static bool parent_fills_root(const struct ui_surface *surface,
                              const struct ui_widget *parent)
{
    const struct ui_widget *root = ui_surface_root(surface);
    return parent && parent->parent == root &&
           parent->impl.placement.kind == UI_PLACE_FILL &&
           root && (!root->ops || (!root->ops->layout && !root->ops->measure));
}

static bool container_bounds(const struct ui_tab_preview *p, struct rect2i *out)
{
    const struct ui_widget *parent = p->widget.parent;
    struct ui_surface *surface = ui_widget_surface(&p->widget);
    if (surface) {
        const struct ui_widget *root = ui_surface_root(surface);
        /* prepare runs before measure_and_layout, so a root-filling container
         * still holds its old bounds after ui_surface_resize. */
        if (!parent || parent == root || parent_fills_root(surface, parent)) {
            *out = (struct rect2i){ .size = ui_surface_viewport(surface) };
            return true;
        }
    }
    if (parent) {
        *out = parent->bounds;
        return true;
    }
    return false;
}

struct horiz_fit {
    struct rect2i box;
    bool has_box;
    int width;
    int max_w;
};

static struct horiz_fit fit_horiz(const struct ui_tab_preview *p,
                                  const struct ui_tab_preview_skin *s)
{
    int chrome_w = 2 * s->border + s->pad.l + s->pad.r;
    int want = s->width - chrome_w;
    if (want < 1)
        want = 1;
    struct rect2i box = {0};
    if (!container_bounds(p, &box))
        return (struct horiz_fit){ .width = want, .max_w = want };
    int max_w = box.width - chrome_w;
    int width = want < max_w ? want : max_w;
    return (struct horiz_fit){
        .box = box,
        .has_box = true,
        .width = width > 1 ? width : 1,
        .max_w = max_w,
    };
}

struct budget {
    struct rect2i box;
    int width;
    int max_height;
    bool fits_title;
};

static int hover_avail_height(const struct ui_tab_preview *p, struct rect2i box)
{
    int gap = ui_ctx_px(&p->widget.style_scale, BELOW_GAP_PX);
    int64_t box_top = box.y;
    int64_t box_bot = (int64_t)box.y + box.height;
    int64_t below_top = (int64_t)p->impl.anchor.y + p->impl.anchor.height + gap;
    int64_t room_below = (below_top >= box_top && box_bot > below_top)
        ? box_bot - below_top : 0;
    int64_t end_above = (int64_t)p->impl.anchor.y - gap;
    int64_t room_above = (end_above <= box_bot && end_above > box_top)
        ? end_above - box_top : 0;
    int64_t best = room_below > room_above ? room_below : room_above;
    return best > INT_MAX ? INT_MAX : (int)best;
}

static struct budget compute_budget(const struct ui_tab_preview *p,
                                    const struct ui_tab_preview_skin *s)
{
    struct horiz_fit h = fit_horiz(p, s);
    if (!h.has_box) {
        return (struct budget){
            .width = h.width,
            .max_height = INT_MAX,
            .fits_title = true,
        };
    }
    if (rect2i_is_empty(h.box) || h.max_w <= 0)
        return (struct budget){ .box = h.box };
    int cell_h = ui_widget_text_style(&p->widget.style_scale).cell_h;
    int title_h = 2 * s->border + s->pad.t + cell_h + s->pad.b;
    int avail_h = p->impl.dragging
        ? h.box.height : hover_avail_height(p, h.box);
    if (avail_h < title_h)
        return (struct budget){ .box = h.box };
    int max_image_h = avail_h - title_h - s->gap;
    if (max_image_h < 0)
        max_image_h = 0;
    return (struct budget){
        .box = h.box,
        .width = h.width,
        .max_height = max_image_h,
        .fits_title = true,
    };
}

struct card {
    struct rect2i title;
    struct rect2i image;
    struct vector2i size;
};

static struct card card_layout(const struct ui_tab_preview *p,
                               const struct ui_tab_preview_skin *s)
{
    int cell_h = ui_widget_text_style(&p->widget.style_scale).cell_h;
    int x = s->border + s->pad.l;
    int y = s->border + s->pad.t;
    int want_w = fit_horiz(p, s).width;
    struct vector2i img = p->impl.image.size;
    int content_w = want_w > img.x ? want_w : img.x;
    struct card l = {
        .title = { .pos = { .x = x, .y = y },
                   .size = { .x = content_w, .y = cell_h } },
    };
    y += cell_h + (img.y > 0 ? s->gap : 0);
    l.image = (struct rect2i){ .pos = { .x = x, .y = y }, .size = img };
    y += img.y;
    l.size = (struct vector2i){
        .x = x + content_w + s->pad.r + s->border,
        .y = y + s->pad.b + s->border,
    };
    return l;
}

struct paint_rects {
    struct card card;
    struct rect2i fill;
    struct rect2i title;
    struct rect2i image;
};

static struct paint_rects paint_rects_of(const struct ui_tab_preview *p,
                                         const struct ui_tab_preview_skin *s,
                                         struct rect2i box)
{
    struct rect2i fill = s->border > 0 ? rect2i_inset(box, s->border) : box;
    struct card card = card_layout(p, s);
    if (rect2i_is_empty(fill))
        return (struct paint_rects){ .card = card };
    struct rect2i content = rect2i_apply_insets(fill, s->pad);
    return (struct paint_rects){
        .card = card,
        .fill = fill,
        .title = rect2i_intersect(card.title, content),
        .image = rect2i_intersect(card.image, fill),
    };
}

/* What a capture asks the card to redo. The preview polls its source while it
 * shows, so a capture that finds the same content must not relayout or repaint
 * the card. */
enum capture_change {
    CAPTURE_CHANGE_NONE = 0,
    CAPTURE_CHANGE_PAINT,
    CAPTURE_CHANGE_MEASURE,
};

enum capture_result {
    CAPTURE_OK = 0,
    CAPTURE_NO_ROOM,
    CAPTURE_FAILED,
};

static void hide_for_size_state(struct ui_tab_preview *p, int index,
                                struct rect2i box)
{
    p->impl.shown = false;
    p->impl.hidden_for_size = true;
    p->impl.index = index;
    p->impl.image = (struct ui_image){0};
    p->impl.pending = -1;
    p->impl.show_at = 0;
    p->impl.refresh_at = 0;
    p->impl.budget_w = 0;
    p->impl.budget_h = 0;
    p->impl.budget_box = box;
}

static void hide_for_size(struct ui_tab_preview *p, int index,
                          struct rect2i box)
{
    bool was = p->impl.shown || p->impl.pending >= 0;
    hide_for_size_state(p, index, box);
    if (was)
        ui_widget_invalidate(&p->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

static void reset_state(struct ui_tab_preview *p)
{
    p->impl.shown = false;
    p->impl.hidden_for_size = false;
    p->impl.dragging = false;
    p->impl.image = (struct ui_image){0};
    p->impl.index = -1;
    p->impl.pending = -1;
    p->impl.show_at = 0;
    p->impl.refresh_at = 0;
    p->impl.budget_w = 0;
    p->impl.budget_h = 0;
    p->impl.budget_box = (struct rect2i){0};
    p->impl.follow_pos = (struct vector2i){0};
}

static enum capture_result capture_budget(struct ui_tab_preview *p, int index,
                                          const struct budget *b,
                                          enum capture_change *change)
{
    const struct ui_tab_preview_source *src = &p->impl.source;
    const char *title = NULL;
    const struct ui_image *image = NULL;
    if (!src->capture(src->userdata, index, b->width, b->max_height,
                      p->impl.dragging, &title, &image))
        return CAPTURE_FAILED;
    /* The source owns the pixels and it can write them again in place. Keep a
     * copy of the thumbnail here, so the comparison below reads the values of
     * the last capture. */
    struct ui_image shot = {0};
    if (b->max_height > 0 && image && image->pixels &&
        image->size.x > 0 && image->size.y > 0 &&
        image->size.y <= b->max_height)
        shot = *image;
    const char *text = title ? title : "";
    bool title_changed = !p->impl.title || strcmp(p->impl.title, text) != 0;
    if (title_changed) {
        char *copy = strdup(text);
        if (!copy)
            return CAPTURE_FAILED;
        free(p->impl.title);
        p->impl.title = copy;
    }
    bool was_shown = p->impl.shown;
    bool content_changed = shot.gen != p->impl.image.gen ||
                           shot.pixels != p->impl.image.pixels;
    bool layout_changed = !was_shown ||
                          !vector2i_eq(shot.size, p->impl.image.size);
    p->impl.image = shot;
    p->impl.index = index;
    p->impl.shown = true;
    p->impl.hidden_for_size = false;
    p->impl.budget_w = b->width;
    p->impl.budget_h = b->max_height;
    p->impl.budget_box = b->box;
    *change = layout_changed ? CAPTURE_CHANGE_MEASURE
            : (content_changed || title_changed) ? CAPTURE_CHANGE_PAINT
            : CAPTURE_CHANGE_NONE;
    return CAPTURE_OK;
}

static enum capture_result capture(struct ui_tab_preview *p, int index,
                                   enum capture_change *change)
{
    if (!p->impl.source.capture)
        return CAPTURE_FAILED;
    struct ui_tab_preview_skin s = skin_of(&p->widget, p->impl.dragging);
    struct budget b = compute_budget(p, &s);
    if (!b.fits_title) {
        hide_for_size(p, index, b.box);
        return CAPTURE_NO_ROOM;
    }
    return capture_budget(p, index, &b, change);
}

static void apply_change(struct ui_tab_preview *p, enum capture_change change)
{
    if (change == CAPTURE_CHANGE_NONE)
        return;
    ui_widget_invalidate(&p->widget, change == CAPTURE_CHANGE_MEASURE
        ? UI_WIDGET_INVALIDATE_MEASURE : UI_WIDGET_INVALIDATE_PAINT);
}

static struct ui_placement cur_placement(const struct ui_tab_preview *p)
{
    if (p->impl.dragging) {
        struct ui_tab_preview_skin s = skin_of(&p->widget, true);
        struct vector2i size = card_layout(p, &s).size;
        return (struct ui_placement){
            .kind = UI_PLACE_AT,
            .anchor = { .pos = {
                .x = p->impl.follow_pos.x - size.x / 2,
                .y = p->impl.follow_pos.y - size.y / 2,
            } },
        };
    }
    return (struct ui_placement){
        .kind = UI_PLACE_BELOW,
        .anchor = p->impl.anchor,
        .gap = ui_ctx_px(&p->widget.style_scale, BELOW_GAP_PX),
    };
}

static void place_cur(struct ui_tab_preview *p)
{
    ui_widget_set_placement(&p->widget, cur_placement(p));
}

static void sync_budget(struct ui_tab_preview *p)
{
    if ((!p->impl.shown && !p->impl.hidden_for_size) || !p->impl.source.capture)
        return;
    struct ui_tab_preview_skin s = skin_of(&p->widget, p->impl.dragging);
    struct budget b = compute_budget(p, &s);
    if (!b.fits_title) {
        hide_for_size(p, p->impl.index, b.box);
        return;
    }
    p->impl.budget_box = b.box;
    if (p->impl.hidden_for_size || b.width != p->impl.budget_w ||
        b.max_height != p->impl.budget_h) {
        enum capture_change change;
        enum capture_result res = capture_budget(p, p->impl.index, &b, &change);
        if (res == CAPTURE_FAILED) {
            ui_tab_preview_hide(p);
            return;
        }
        apply_change(p, change);
    }
    place_cur(p);
}

void ui_tab_preview_hide(struct ui_tab_preview *p)
{
    if (!p)
        return;
    bool was = p->impl.shown || p->impl.pending >= 0;
    reset_state(p);
    if (was)
        ui_widget_invalidate(&p->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

static bool defer_budget_to_layout(const struct ui_tab_preview *p)
{
    if (!p->impl.managed_anchor || p->impl.dragging)
        return false;
    const struct ui_widget *parent = p->widget.parent;
    struct ui_surface *surface = ui_widget_surface(&p->widget);
    if (!parent || !surface)
        return false;
    struct vector2i vp = ui_surface_viewport(surface);
    if (vp.x <= 0 || vp.y <= 0)
        return false;
    unsigned dirty = UI_WIDGET_INVALIDATE_MEASURE | UI_WIDGET_INVALIDATE_LAYOUT;
    if (parent->impl.invalidation & dirty)
        return true;
    if (!parent_fills_root(surface, parent))
        return false;
    const struct ui_widget *root = ui_surface_root(surface);
    return root && (root->impl.invalidation & dirty) &&
           !rect2i_eq(parent->bounds, (struct rect2i){ .size = vp });
}

static void preview_prepare(struct ui_widget *widget)
{
    struct ui_tab_preview *p = preview_of(widget);
    struct ui_surface *surface = ui_widget_surface(widget);
    if (surface &&
        (!widget->impl.style_valid ||
         widget->impl.style_generation !=
             ui_style_context_generation(&surface->impl.style)))
        return;
    if (defer_budget_to_layout(p))
        return;
    sync_budget(p);
}

static void preview_style_changed(struct ui_widget *widget)
{
    struct ui_tab_preview *p = preview_of(widget);
    if (!p->impl.shown && !p->impl.hidden_for_size)
        return;
    if (defer_budget_to_layout(p))
        return;
    sync_budget(p);
    if (p->impl.shown)
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_MEASURE);
}

static void preview_layout(struct ui_widget *widget, struct rect2i bounds)
{
    (void)bounds;
    struct ui_tab_preview *p = preview_of(widget);
    if (!p->impl.shown && !p->impl.hidden_for_size)
        return;
    struct rect2i box = {0};
    if (container_bounds(p, &box) && !rect2i_eq(box, p->impl.budget_box))
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_MEASURE);
}

static uint64_t preview_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_tab_preview *p = preview_of(widget);
    if (!p->impl.shown && !p->impl.hidden_for_size && p->impl.pending >= 0) {
        if (!p->impl.show_at)
            p->impl.show_at = now_ms + SHOW_DELAY_MS;
        if (now_ms < p->impl.show_at)
            return p->impl.show_at;
        int index = p->impl.pending;
        p->impl.pending = -1;
        enum capture_change change;
        enum capture_result res = capture(p, index, &change);
        if (res != CAPTURE_OK)
            return 0;
        apply_change(p, change);
        place_cur(p);
        p->impl.refresh_at = 0;
    }
    if (!p->impl.shown)
        return 0;
    if (!p->impl.refresh_at) {
        p->impl.refresh_at = now_ms + REFRESH_MS;
    } else if (now_ms >= p->impl.refresh_at) {
        enum capture_change change;
        enum capture_result res = capture(p, p->impl.index, &change);
        if (res == CAPTURE_FAILED) {
            ui_tab_preview_hide(p);
            return 0;
        }
        if (res == CAPTURE_NO_ROOM)
            return 0;
        apply_change(p, change);
        place_cur(p);
        p->impl.refresh_at = now_ms + REFRESH_MS;
    }
    return p->impl.refresh_at;
}

static struct vector2i preview_measure(const struct ui_widget *widget,
                                       struct ui_constraints constraints)
{
    const struct ui_tab_preview *p = preview_of(widget);
    struct vector2i size = {0};
    if (p->impl.shown) {
        struct ui_tab_preview_skin s = skin_of(widget, p->impl.dragging);
        size = card_layout(p, &s).size;
    }
    return ui_constraints_clamp(constraints, size);
}

static void preview_paint(const struct ui_widget *widget,
                          struct ui_paint_context *paint)
{
    const struct ui_tab_preview *p = preview_of(widget);
    if (!p->impl.shown)
        return;
    struct ui_tab_preview_skin s = skin_of(widget, p->impl.dragging);
    struct rect2i box = ui_paint_bounds(paint);
    if (rect2i_is_empty(box))
        return;
    if (s.shadow_color >> 24)
        ui_paint_shadow(paint, rect2i_translate(box, s.shadow_offset),
                        s.radius, s.shadow_blur, s.shadow_color);
    if (s.border > 0 && s.border_color)
        ui_paint_rounded_rect(paint, box, s.radius, s.border_color);
    struct paint_rects r = paint_rects_of(p, &s, box);
    if (rect2i_is_empty(r.fill))
        return;
    int radius = s.radius > s.border ? s.radius - s.border : 0;
    if (s.background)
        ui_paint_rounded_rect(paint, r.fill, radius, s.background);
    if (!ui_paint_push_clip(paint, r.fill))
        return;

    struct ui_text_style text = ui_widget_text_style(&widget->style_scale);
    text.flags = UI_TEXT_BOLD;
    struct css_style title_style = ui_widget_part(widget, parts.title, 0);
    uint32_t title_color = title_style.has_text_color
        ? title_style.text_color : 0xFFFFFFFFu;
    const char *title = p->impl.title;
    if (title && *title && !rect2i_is_empty(r.title) && text.advance > 0) {
        size_t len = ui_text_run_fit(title, strlen(title),
                                     r.title.width / text.advance);
        ui_paint_text(paint, r.title, text, title_color, title, len);
    }
    if (p->impl.image.pixels && !rect2i_is_empty(r.image))
        ui_paint_image(paint, r.card.image.pos, &p->impl.image);
    ui_paint_pop_clip(paint);
}

static void preview_debug_parts(const struct ui_widget *widget,
                                struct ui_debug_parts *out)
{
    const struct ui_tab_preview *p = preview_of(widget);
    if (!p->impl.shown)
        return;
    struct ui_tab_preview_skin s = skin_of(widget, p->impl.dragging);
    struct rect2i box = { .size = widget->bounds.size };
    struct paint_rects r = paint_rects_of(p, &s, box);
    if (rect2i_is_empty(r.fill))
        return;
    if (!rect2i_is_empty(r.title)) {
        const char *title = p->impl.title;
        ui_debug_part_text(out, "title", -1,
                           ui_widget_rect_to_surface(widget, r.title),
                           title, title ? strlen(title) : 0);
    }
    if (p->impl.image.pixels && !rect2i_is_empty(r.image))
        ui_debug_part(out, "image", -1,
                      ui_widget_rect_to_surface(widget, r.image));
}

static void preview_destroy(struct ui_widget *widget)
{
    struct ui_tab_preview *p = preview_of(widget);
    free(p->impl.title);
    p->impl.title = NULL;
}

static const struct ui_widget_ops TAB_PREVIEW_OPS = {
    .name = "tab-preview",
    .prepare = preview_prepare,
    .measure = preview_measure,
    .layout = preview_layout,
    .tick = preview_tick,
    .paint = preview_paint,
    .style_changed = preview_style_changed,
    .debug_parts = preview_debug_parts,
    .destroy = preview_destroy,
};

void ui_tab_preview_init(struct ui_tab_preview *p)
{
    if (!p)
        return;
    tab_preview_parts_init();
    *p = (struct ui_tab_preview){ .impl.index = -1, .impl.pending = -1 };
    ui_widget_init(&p->widget, &TAB_PREVIEW_OPS, NULL);
    ui_widget_set_style_identity(&p->widget, NULL, NULL, "tab-preview");
    ui_widget_set_pointer_transparent(&p->widget, true);
    ui_widget_set_placement(&p->widget, (struct ui_placement){
        .kind = UI_PLACE_AT,
    });
}

void ui_tab_preview_set_source(struct ui_tab_preview *p,
                               const struct ui_tab_preview_source *source)
{
    if (!p)
        return;
    ui_tab_preview_hide(p);
    p->impl.source = source ? *source : (struct ui_tab_preview_source){0};
}

void ui_tab_preview_hover(struct ui_tab_preview *p, int index,
                          struct rect2i anchor)
{
    if (!p || p->impl.dragging)
        return;
    if (p->impl.suppressed) {
        if (index < 0)
            p->impl.suppressed = false;
        return;
    }
    if (index < 0) {
        ui_tab_preview_hide(p);
        return;
    }
    p->impl.anchor = anchor;
    if (p->impl.shown || p->impl.hidden_for_size) {
        if (index != p->impl.index) {
            enum capture_change change;
            enum capture_result res = capture(p, index, &change);
            if (res == CAPTURE_FAILED) {
                ui_tab_preview_hide(p);
                return;
            }
            if (res == CAPTURE_OK) {
                apply_change(p, change);
                place_cur(p);
            }
            return;
        }
        sync_budget(p);
        return;
    }
    if (p->impl.pending != index) {
        p->impl.pending = index;
        p->impl.show_at = 0;
        ui_widget_invalidate(&p->widget, UI_WIDGET_INVALIDATE_PAINT);
    }
}

void ui_tab_preview_suppress(struct ui_tab_preview *p)
{
    if (!p)
        return;
    ui_tab_preview_hide(p);
    p->impl.suppressed = true;
}

void ui_tab_preview_follow(struct ui_tab_preview *p, int index,
                           struct vector2i at)
{
    if (!p)
        return;
    if (index < 0) {
        ui_tab_preview_hide(p);
        return;
    }
    p->impl.follow_pos = at;
    if (!p->impl.dragging ||
        (!p->impl.shown && !p->impl.hidden_for_size) ||
        p->impl.index != index) {
        p->impl.dragging = true;
        p->impl.pending = -1;
        enum capture_change change;
        enum capture_result res = capture(p, index, &change);
        if (res == CAPTURE_FAILED) {
            ui_tab_preview_hide(p);
            return;
        }
        if (res == CAPTURE_OK)
            apply_change(p, change);
    }
    if (p->impl.shown)
        place_cur(p);
}

bool ui_tab_preview_is_visible(const struct ui_tab_preview *p)
{
    return p && p->impl.shown;
}

static bool clear_in_layout(struct ui_tab_preview *p, bool was_shown)
{
    reset_state(p);
    p->impl.anchor = (struct rect2i){0};
    p->widget.impl.placement = cur_placement(p);
    return was_shown;
}

void ui_tab_preview_set_managed_anchor(struct ui_tab_preview *p, bool managed)
{
    if (p)
        p->impl.managed_anchor = managed;
}

bool ui_tab_preview_sync_anchor(struct ui_tab_preview *p, int index,
                                struct rect2i anchor)
{
    if (!p || p->impl.dragging || p->impl.suppressed)
        return false;
    if (p->impl.pending >= 0 && !p->impl.shown && !p->impl.hidden_for_size) {
        if (index == p->impl.pending)
            p->impl.anchor = anchor;
        else
            reset_state(p);
        return false;
    }
    if (!p->impl.shown && !p->impl.hidden_for_size)
        return false;
    bool was_shown = p->impl.shown;
    if (index < 0 || index != p->impl.index || !p->impl.source.capture)
        return clear_in_layout(p, was_shown);
    p->impl.anchor = anchor;
    struct ui_tab_preview_skin s = skin_of(&p->widget, false);
    struct budget b = compute_budget(p, &s);
    if (!b.fits_title) {
        hide_for_size_state(p, index, b.box);
        return was_shown;
    }
    p->impl.budget_box = b.box;
    if (p->impl.hidden_for_size || b.width != p->impl.budget_w ||
        b.max_height != p->impl.budget_h) {
        enum capture_change change;
        enum capture_result res = capture_budget(p, index, &b, &change);
        if (res == CAPTURE_FAILED)
            return clear_in_layout(p, was_shown);
        if (change != CAPTURE_CHANGE_NONE)
            p->widget.impl.invalidation |= UI_WIDGET_INVALIDATE_PAINT;
    }
    p->widget.impl.placement = cur_placement(p);
    struct vector2i want = p->impl.shown
        ? card_layout(p, &s).size : (struct vector2i){0};
    return was_shown != p->impl.shown ||
           !vector2i_eq(want, p->widget.measured_size);
}

int ui_tab_preview_image_width(const struct ui_tab_preview *p, bool dragging)
{
    if (!p)
        return 0;
    struct ui_tab_preview_skin s = skin_of(&p->widget, dragging);
    return fit_horiz(p, &s).width;
}
