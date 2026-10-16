#include "ui/widgets/tab_preview.h"

#include "ui/debug.h"
#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"

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

static int inner_width(const struct ui_tab_preview_skin *s)
{
    int w = s->width - 2 * s->border - s->pad.l - s->pad.r;
    return w > 1 ? w : 1;
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
    struct vector2i img = p->impl.image ? p->impl.image->size : (struct vector2i){0};
    int content_w = inner_width(s) > img.x ? inner_width(s) : img.x;
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

static bool capture(struct ui_tab_preview *p, int index)
{
    const struct ui_tab_preview_source *src = &p->impl.source;
    const char *title = NULL;
    const struct ui_image *image = NULL;
    struct ui_tab_preview_skin s = skin_of(&p->widget, p->impl.dragging);
    if (!src->capture ||
        !src->capture(src->userdata, index, inner_width(&s),
                      p->impl.dragging, &title, &image))
        return false;
    char *copy = strdup(title ? title : "");
    if (!copy)
        return false;
    free(p->impl.title);
    p->impl.title = copy;
    p->impl.image = image;
    p->impl.index = index;
    p->impl.shown = true;
    ui_widget_invalidate(&p->widget, UI_WIDGET_INVALIDATE_MEASURE);
    return true;
}

static void place_below(struct ui_tab_preview *p, struct rect2i anchor)
{
    ui_widget_set_placement(&p->widget, (struct ui_placement){
        .kind = UI_PLACE_BELOW, .anchor = anchor,
        .gap = ui_ctx_px(&p->widget.style_scale, BELOW_GAP_PX),
    });
}

void ui_tab_preview_hide(struct ui_tab_preview *p)
{
    if (!p)
        return;
    bool was = p->impl.shown || p->impl.pending >= 0;
    p->impl.shown = false;
    p->impl.dragging = false;
    p->impl.image = NULL;
    p->impl.pending = -1;
    p->impl.show_at = 0;
    p->impl.refresh_at = 0;
    if (was)
        ui_widget_invalidate(&p->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

static uint64_t preview_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_tab_preview *p = preview_of(widget);
    if (!p->impl.shown && p->impl.pending >= 0) {
        if (!p->impl.show_at)
            p->impl.show_at = now_ms + SHOW_DELAY_MS;
        if (now_ms < p->impl.show_at)
            return p->impl.show_at;
        int index = p->impl.pending;
        p->impl.pending = -1;
        if (!capture(p, index))
            return 0;
        place_below(p, p->impl.anchor);
        p->impl.refresh_at = 0;
    }
    if (!p->impl.shown)
        return 0;
    if (!p->impl.refresh_at) {
        p->impl.refresh_at = now_ms + REFRESH_MS;
    } else if (now_ms >= p->impl.refresh_at) {
        if (!capture(p, p->impl.index)) {
            ui_tab_preview_hide(p);
            return 0;
        }
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
    if (s.shadow_color >> 24)
        ui_paint_shadow(paint, rect2i_translate(box, s.shadow_offset),
                        s.radius, s.shadow_blur, s.shadow_color);
    if (s.border > 0 && s.border_color)
        ui_paint_rounded_rect(paint, box, s.radius, s.border_color);
    struct rect2i fill = s.border > 0 ? rect2i_inset(box, s.border) : box;
    int radius = s.radius > s.border ? s.radius - s.border : 0;
    if (s.background && !rect2i_is_empty(fill))
        ui_paint_rounded_rect(paint, fill, radius, s.background);

    struct card l = card_layout(p, &s);
    struct ui_text_style text = ui_widget_text_style(&widget->style_scale);
    text.flags = UI_TEXT_BOLD;
    struct css_style title_style = ui_widget_part(widget, parts.title, 0);
    uint32_t title_color = title_style.has_text_color
        ? title_style.text_color : 0xFFFFFFFFu;
    const char *title = p->impl.title;
    if (title && *title) {
        size_t len = ui_text_run_fit(title, strlen(title),
                                     l.title.width / text.advance);
        ui_paint_text(paint, l.title, text, title_color, title, len);
    }
    if (p->impl.image)
        ui_paint_image(paint, l.image.pos, p->impl.image);
}

static void preview_debug_parts(const struct ui_widget *widget,
                                struct ui_debug_parts *out)
{
    const struct ui_tab_preview *p = preview_of(widget);
    if (!p->impl.shown)
        return;
    struct ui_tab_preview_skin s = skin_of(widget, p->impl.dragging);
    struct card l = card_layout(p, &s);
    const char *title = p->impl.title;
    ui_debug_part_text(out, "title", -1, ui_widget_rect_to_surface(widget, l.title),
                       title, title ? strlen(title) : 0);
    if (p->impl.image)
        ui_debug_part(out, "image", -1, ui_widget_rect_to_surface(widget, l.image));
}

static void preview_destroy(struct ui_widget *widget)
{
    struct ui_tab_preview *p = preview_of(widget);
    free(p->impl.title);
    p->impl.title = NULL;
}

static const struct ui_widget_ops TAB_PREVIEW_OPS = {
    .name = "tab-preview",
    .measure = preview_measure,
    .tick = preview_tick,
    .paint = preview_paint,
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
    if (p->impl.shown) {
        if (index != p->impl.index && !capture(p, index)) {
            ui_tab_preview_hide(p);
            return;
        }
        place_below(p, anchor);
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
    if (!p->impl.dragging || !p->impl.shown || p->impl.index != index) {
        p->impl.dragging = true;
        p->impl.pending = -1;
        if (!capture(p, index)) {
            ui_tab_preview_hide(p);
            return;
        }
    }
    struct ui_tab_preview_skin s = skin_of(&p->widget, p->impl.dragging);
    struct vector2i size = card_layout(p, &s).size;
    ui_widget_set_placement(&p->widget, (struct ui_placement){
        .kind = UI_PLACE_AT,
        .anchor = { .pos = { .x = at.x - size.x / 2, .y = at.y - size.y / 2 } },
    });
}

bool ui_tab_preview_is_visible(const struct ui_tab_preview *p)
{
    return p && p->impl.shown;
}

int ui_tab_preview_image_width(const struct ui_tab_preview *p, bool dragging)
{
    if (!p)
        return 0;
    struct ui_tab_preview_skin s = skin_of(&p->widget, dragging);
    return inner_width(&s);
}
