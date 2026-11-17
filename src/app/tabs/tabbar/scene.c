#include "app/tabs/tabbar/internal.h"

#include "paint/text_run.h"
#include "ui/text_break.h"
#include "ui/anim.h"
#include "ui/css.h"
#include "ui/widget.h"
#include "ui/widget_ops.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TB_BADGE_TABS 8
#ifdef __APPLE__
#define TB_BADGE_PREFIX "\xe2\x8c\x98"
#else
#define TB_BADGE_PREFIX "^"
#endif
#define TB_CLOSE_MARK "\xc3\x97"
#define TB_NEW_MARK   "+"

#define TB_PART_TAB(i) ((uint64_t)(unsigned)(i))
#define TB_PART_NEW    (1ULL << 32)
#define TB_PART_DRAG   (2ULL << 32)
#define TB_PART_DROP   (3ULL << 32)

/* Paints a tab title at `at`. Cuts it to `cap` cells. A cut title ends with
 * a marker. The marker takes one of the `cap` cells. When no cell fits, the
 * title paints nothing. */
static bool tb_title(struct ui_scene *scene, struct vector2i at, int advance,
                     int text_h, uint32_t color, bool bold, const char *label,
                     int cap)
{
    if (cap <= 0 || advance <= 0)
        return true;
    size_t total = strlen(label);
    size_t len = ui_text_fit(label, total, cap);
    bool cut = len < total;
    if (cut)
        len = cap > 1 ? ui_text_fit(label, total, cap - 1) : 0;
    struct ui_text_style style = {
        .advance = advance, .cell_h = text_h,
        .flags = bold ? UI_TEXT_BOLD : UI_TEXT_PLAIN,
    };
    if (len && !ui_scene_text_run(scene, (struct rect2i){
            .pos = at, .size = { .x = (int)len * advance, .y = text_h },
        }, style, color, label, len))
        return false;
    if (!cut)
        return true;
    return ui_scene_text_run(scene, (struct rect2i){
        .pos = { .x = at.x + (int)len * advance, .y = at.y },
        .size = { .x = advance, .y = text_h },
    }, style, color, "\xE2\x80\xA6", 3);
}

static float tb_open_fade(float open)
{
    if (open <= 0.0f)
        return 0.0f;
    if (open >= 0.6f)
        return 1.0f;
    return open / 0.6f;
}

/* Returns the text color of the bar. A part with no color falls back to the
 * text color of the bar widget. */
static uint32_t tb_text_color(const struct ui_tabbar *tb,
                              const struct css_style *part)
{
    if (part->has_text_color)
        return part->text_color;
    return tb->widget.style.has_text_color ? tb->widget.style.text_color : 0;
}

bool ui_tabbar_build_drag_scene(struct ui_tabbar *tb, int index,
                                const struct ui_image *preview_image,
                                struct ui_scene *scene,
                                struct vector2i *image_size,
                                struct vector2i *hotspot)
{
    if (!tb || !scene || !image_size || !hotspot ||
        index < 0 || index >= tb->state.n_tabs)
        return false;
    const struct ui_tab_label *tabs = tb->state.labels;
    const struct tb_parts *p = tb_part();
    const struct css_scale_context *ctx = tb_ctx(tb);
    struct ui_text_style text = ui_widget_text_style(ctx);
    struct css_style active = tb_style(tb, p->tab_active, CSS_PSEUDO_NONE);
    struct css_style close = tb_style(tb, p->tab_close, CSS_PSEUDO_NONE);
    struct css_style card = tb_style(tb, p->tab_preview, CSS_PSEUDO_NONE);
    struct tb_layout l = tb_layout_for(tb, tb->state.n_tabs, tb->state.size);
    struct rect2i tab_rect;
    if (!tb_tab_rect_at(tb, tb->state.n_tabs, tb->state.size, index,
                        &tab_rect))
        return false;
    ui_scene_init(scene);

    bool has_preview = preview_image && preview_image->pixels &&
                       preview_image->size.x > 0 && preview_image->size.y > 0;

    struct insets2i card_pad = ui_ctx_insets(ctx, card.padding, 0);
    int pad = card_pad.l;
    int gap = ui_ctx_inset(ctx, card.gap, 0);
    int card_w = has_preview ? preview_image->size.x + pad * 2 : 0;
    int card_h = has_preview ? preview_image->size.y + pad * 2 : 0;

    int total_w = tab_rect.width;
    if (has_preview && card_w > total_w)
        total_w = card_w;

    int total_h = tab_rect.height;
    if (has_preview)
        total_h += gap + card_h;

    int radius = l.radius;
    struct vector2i shadow_at = card.has_shadow
        ? (struct vector2i){
              .x = ui_css_resolve_int(card.shadow_x, 0, 0, ctx, false),
              .y = ui_css_resolve_int(card.shadow_y, 0, 0, ctx, false),
          } : (struct vector2i){0};
    int blur = card.has_shadow
        ? ui_css_resolve_int(card.shadow_blur, 0, 0, ctx, true) : 0;
    uint32_t shadow_color = card.has_shadow ? card.shadow_color : 0;
    struct rect2i r = { .pos = { .x = 0, .y = 0 }, .size = tab_rect.size };
    if (!ui_scene_shadow(scene, (struct rect2i){
            .pos = { .x = shadow_at.x, .y = shadow_at.y }, .size = r.size },
            radius + 2, blur, shadow_color) ||
        !ui_scene_rounded_rect(scene, r, radius,
                               active.has_bg_color ? active.bg_color : 0))
        goto fail;

    int advance = text.advance;
    int text_h = text.cell_h;
    struct rect2i close_box = tb_close_box(r, l.close_w, l.close_h,
                                           l.close_pad);
    int text_x = l.tab_pad;
    int avail = close_box.x - l.close_gap - text_x;
    int cap = advance > 0 && avail > 0 ? avail / advance : 0;
    const char *label = tabs[index].text && tabs[index].text[0]
        ? tabs[index].text : "shell";
    if (!tb_title(scene, (struct vector2i){
                      .x = text_x, .y = (r.height - text_h) / 2 },
                  advance, text_h, tb_text_color(tb, &active), true, label, cap))
        goto fail;

    struct ui_text_style mark_style = {
        .advance = advance,
        .cell_h = text_h,
        .flags = UI_TEXT_PLAIN,
    };
    struct rect2i mark = {
        .pos = { .x = close_box.x + (close_box.width - advance) / 2,
                 .y = close_box.y + (close_box.height - text_h) / 2 },
        .size = { .x = advance, .y = text_h },
    };
    if (!ui_scene_text_run(scene, mark, mark_style, tb_text_color(tb, &close),
                           TB_CLOSE_MARK, sizeof(TB_CLOSE_MARK) - 1))
        goto fail;

    if (has_preview) {
        struct rect2i card_box = {
            .pos = { .x = 0, .y = tab_rect.height + gap },
            .size = { .x = card_w, .y = card_h },
        };
        if (!ui_scene_shadow(scene, (struct rect2i){
                .pos = { .x = card_box.x + shadow_at.x,
                         .y = card_box.y + shadow_at.y },
                .size = card_box.size }, l.preview_radius, blur,
                shadow_color) ||
            !ui_scene_rounded_rect(scene, card_box, l.preview_radius,
                                   card.has_bg_color ? card.bg_color : 0))
            goto fail;
        struct vector2i img_pos = {
            .x = card_box.x + pad,
            .y = card_box.y + pad,
        };
        if (!ui_scene_image(scene, img_pos, preview_image))
            goto fail;
    }

    *image_size = (struct vector2i){ .x = total_w, .y = total_h };
    hotspot->x = tb->state.drag_grab_x;
    hotspot->y = tb->state.drag_origin.y - tab_rect.y;
    if (hotspot->x < 0) hotspot->x = 0;
    if (hotspot->x >= total_w) hotspot->x = total_w - 1;
    if (hotspot->y < 0) hotspot->y = 0;
    if (hotspot->y >= total_h) hotspot->y = total_h - 1;
    return true;

fail:
    ui_scene_destroy(scene);
    return false;
}

bool tb_scene_emit(struct ui_tabbar *tb, const struct ui_tab_label *tabs,
                   int n_tabs, struct vector2i size, struct ui_scene *scene,
                   uint64_t now_ms)
{
    if (!scene || !tabs || n_tabs < 1 || size.x <= 0)
        return false;
    if (tb && !tb_ensure_tabs(tb, n_tabs))
        return false;
    struct tb_layout l = tb_layout_for(tb, n_tabs, size);
    if (l.height <= 0)
        return false;
    const struct tb_parts *p = tb_part();
    const struct css_scale_context *ctx = tb_ctx(tb);
    struct ui_text_style text = ui_widget_text_style(ctx);
    struct css_style bar = tb_style(tb, p->bar, CSS_PSEUDO_NONE);
    struct css_style idle = tb_style(tb, p->tab, CSS_PSEUDO_NONE);
    struct css_style hover = tb_style(tb, p->tab, CSS_PSEUDO_HOVER);
    struct css_style active = tb_style(tb, p->tab_active, CSS_PSEUDO_NONE);
    struct css_style focus = tb_style(tb, p->tab, CSS_PSEUDO_FOCUS);
    struct css_style drop = tb_style(tb, p->tab_drop, CSS_PSEUDO_NONE);
    struct css_style close = tb_style(tb, p->tab_close, CSS_PSEUDO_NONE);
    struct css_style close_h = tb_style(tb, p->tab_close, CSS_PSEUDO_HOVER);
    struct css_style fresh = tb_style(tb, p->tab_new, CSS_PSEUDO_NONE);
    struct css_style fresh_h = tb_style(tb, p->tab_new, CSS_PSEUDO_HOVER);
    struct css_style sep = tb_style(tb, p->tab_sep, CSS_PSEUDO_NONE);
    struct css_style card = tb_style(tb, p->tab_preview, CSS_PSEUDO_NONE);

    uint32_t bar_color = bar.has_bg_color ? bar.bg_color
        : (tb->state.bg_color ? tb->state.bg_color : 0);
    uint32_t bar_bg = ui_color_opacity(bar_color,
                                       css_style_effective_opacity(&bar));
    if (!ui_scene_rect(scene, (struct rect2i){
            .size = { .x = size.x, .y = l.height } }, bar_bg))
        return false;

    uint32_t text_color = tb_text_color(tb, &idle);
    uint32_t active_text = tb_text_color(tb, &active);
    uint32_t tab_active_bg = active.has_bg_color ? active.bg_color : 0;

    bool bar_clip = tb->state.reveal.start_ms != 0 &&
        tb->state.reveal.value < 1.0f;
    if (bar_clip &&
        !ui_scene_push_clip(scene, (struct rect2i){
            .size = { .x = size.x, .y = l.height } }))
        return false;

    int advance = text.advance;
    int text_h = text.cell_h;

    bool dragging = tb->state.drag_state == UI_TAB_DRAG_REORDER ||
                    tb->state.drag_state == UI_TAB_DRAG_DETACH;
    int drag_idx = dragging ? tb->state.drag_tab : -1;

    float base_x = (float)l.pad.l;
    int tab_y = l.pad.t + (l.height - l.pad.t - l.pad.b - l.tab_height) / 2;
    bool clipped_right = false;

    struct rect2i drop_r;
    if (tb_drop_rect(tb, &l, size, &drop_r)) {
        if (!ui_scene_begin_part(scene, TB_PART_DROP))
            return false;
        struct vector2i drop_shadow_at = drop.has_shadow
            ? (struct vector2i){
                  .x = ui_css_resolve_int(drop.shadow_x, 0, 0, ctx, false),
                  .y = ui_css_resolve_int(drop.shadow_y, 0, 0, ctx, false),
              } : (struct vector2i){0};
        int drop_blur = drop.has_shadow
            ? ui_css_resolve_int(drop.shadow_blur, 0, 0, ctx, true) : 0;
        struct rect2i shadow = rect2i_translate(drop_r, drop_shadow_at);
        if (drop.has_shadow && (drop.shadow_color >> 24) &&
            !ui_scene_shadow(scene, shadow, l.radius, drop_blur,
                             drop.shadow_color))
            return false;
        uint32_t drop_bg = ui_color_opacity(
            drop.has_bg_color ? drop.bg_color : tab_active_bg,
            css_style_effective_opacity(&drop));
        if (!ui_scene_rounded_rect(scene, drop_r, l.radius, drop_bg))
            return false;
        if (!ui_scene_end_scope(scene))
            return false;
    }

    uint32_t tab_hover_bg = hover.has_bg_color ? hover.bg_color : 0;
    uint32_t sep_color = sep.has_text_color
        ? sep.text_color
        : ui_color_opacity(text_color, 0.30f);

    #define TB_TAB_HOT(idx)                                                 \
        (tabs[idx].active || (idx) == drag_idx ||                           \
         (tb->state.hover_tab == (idx) || tb->state.hover_close == (idx) || \
          tb->state.pressed_tab == (idx)))

    for (int i = 0; i < n_tabs; i++) {
        float open = tb_open_progress(tb, i);
        int tw = tb_tab_width_at(tb, &l, i);
        int cur_x = (int)roundf(base_x);
        base_x += tb_tab_width_f(tb, &l, i) + (float)l.gap * open;

        if (i == drag_idx)
            continue;

        int tx = cur_x + (int)roundf(tb_tab_offset(tb, i));
        if (tx >= size.x) {
            clipped_right = true;
            continue;
        }
        int w = tw;
        if (tx + w > size.x - l.pad.r) {
            w = size.x - l.pad.r - tx;
            clipped_right = true;
        }
        if (w <= 0)
            continue;
        if (!ui_scene_begin_part(scene, TB_PART_TAB(i)))
            return false;

        struct rect2i r = {
            .pos = { .x = tx, .y = tab_y },
            .size = { .x = w, .y = l.tab_height },
        };

        float fade = tb_open_fade(open);

        if (i > 0 && !TB_TAB_HOT(i) && !TB_TAB_HOT(i - 1)) {
            float f0 = tb_open_fade(tb_open_progress(tb, i - 1));
            float sep_fade = f0 < fade ? f0 : fade;
            int sep_x = l.gap > 0 ? r.x - (l.gap + 1) / 2 : r.x - 1;
            if (sep_x < 0) sep_x = 0;
            int sep_h = r.height / 2;
            uint32_t sep_c = ui_color_opacity(sep_color, sep_fade);
            if (!ui_scene_rect(scene, (struct rect2i){
                    .pos = { .x = sep_x, .y = r.y + (r.height - sep_h) / 2 },
                    .size = { .x = 1, .y = sep_h } },
                    sep_c))
                return false;
        }

        uint32_t bg_c = tab_active_bg;
        if (!tabs[i].active) {
            if (tb->state.hover_tab == i)
                bg_c = tab_hover_bg;
            else
                bg_c = idle.has_bg_color ? idle.bg_color : 0;
        }

        if (!ui_scene_rounded_rect(scene, r, l.radius, bg_c))
            return false;

        if (i < tb->state.tab_cap &&
            !ui_scene_ripple(scene, r, &tb->state.tabs[i].ripple, now_ms))
            return false;

        bool content_clip = open < 1.0f;
        if (content_clip && !ui_scene_push_clip(scene, r))
            return false;

        struct rect2i close_box;
        bool has_close = false;
        if (content_clip || r.width >= l.close_w + l.close_pad * 2) {
            close_box = tb_close_box(r, l.close_w, l.close_h, l.close_pad);
            has_close = true;
        }
        int title_right = has_close ? close_box.x - l.close_gap
                                    : rect2i_end(r).x - l.tab_pad;

        const char *label = tabs[i].text && tabs[i].text[0] ? tabs[i].text : "shell";
        int text_x = r.x + l.tab_pad;
        int avail = title_right - text_x;
        int cap = advance > 0 && avail > 0 ? avail / advance : 0;
        uint32_t txt_c = tabs[i].active ? active_text : text_color;
        txt_c = ui_color_opacity(txt_c, fade);
        int pad_t = (r.height - text_h) / 2;
        if (pad_t < 0) pad_t = 0;
        if (!tb_title(scene, (struct vector2i){ .x = text_x, .y = r.y + pad_t },
                      advance, text_h, txt_c, tabs[i].active, label, cap))
            return false;

        if (has_close) {
            float progress = (i < tb->state.tab_cap)
                ? ui_hover_anim_value(&tb->state.tabs[i].close_hover) : 0.0f;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;

            int crad = close_box.height < close_box.width
                ? close_box.height / 2 : close_box.width / 2;
            struct vector2i cc = {
                .x = close_box.x + close_box.width / 2,
                .y = close_box.y + close_box.height / 2,
            };
            float close_h_opacity = css_style_effective_opacity(&close_h);
            if (progress > 0.0f && close_h.has_bg_color) {
                uint32_t c = ui_color_opacity(close_h.bg_color,
                                              close_h_opacity * progress * fade);
                if (!ui_scene_circle(scene, cc, crad, c, (uint8_t)(c >> 24)))
                    return false;
            } else if (close.has_bg_color) {
                float close_opacity = css_style_effective_opacity(&close);
                uint32_t c = ui_color_opacity(close.bg_color,
                                              close_opacity * (1.0f - progress) * fade);
                if (!ui_scene_circle(scene, cc, crad, c, (uint8_t)(c >> 24)))
                    return false;
            }

            float badge_alpha = (1.0f - progress) * fade;
            if (badge_alpha > 0.001f && i < TB_BADGE_TABS) {
                char badge[8];
                int bn = snprintf(badge, sizeof(badge), TB_BADGE_PREFIX "%d", i + 1);
                size_t blen = (bn > 0 && (size_t)bn < sizeof(badge)) ? (size_t)bn : 0;
                int b_w = ui_text_run_cells(badge, blen) * advance;
                if (b_w <= r.width) {
                    int bx = close_box.x + (close_box.width - b_w) / 2;
                    if (bx < r.x) bx = r.x;
                    if (bx + b_w > rect2i_end(r).x) bx = rect2i_end(r).x - b_w;
                    struct ui_text_style b_style = {
                        .advance = advance,
                        .cell_h = text_h,
                        .flags = UI_TEXT_PLAIN,
                    };
                    struct rect2i br = {
                        .pos = { .x = bx, .y = close_box.y + (close_box.height - text_h) / 2 },
                        .size = { .x = b_w, .y = text_h },
                    };
                    uint32_t badge_c = ui_color_opacity(tb_text_color(tb, &close),
                                                        badge_alpha);
                    if (!ui_scene_text_run(scene, br, b_style, badge_c, badge, blen))
                        return false;
                }
            }

            float close_alpha = progress * fade;
            if (close_alpha > 0.001f) {
                uint32_t mark_c = ui_color_opacity(tb_text_color(tb, &close_h),
                                                   close_alpha);
                struct ui_text_style m_style = {
                    .advance = advance,
                    .cell_h = text_h,
                    .flags = UI_TEXT_PLAIN,
                };
                struct rect2i mr = {
                    .pos = { .x = close_box.x + (close_box.width - advance) / 2,
                             .y = close_box.y + (close_box.height - text_h) / 2 },
                    .size = { .x = advance, .y = text_h },
                };
                if (!ui_scene_text_run(scene, mr, m_style, mark_c, TB_CLOSE_MARK,
                                       sizeof(TB_CLOSE_MARK) - 1))
                    return false;
            }
        }

        if (content_clip && !ui_scene_pop_clip(scene))
            return false;
        if (ui_widget_has_focus(&tb->widget) &&
            i == tb->state.keyboard_index &&
            !ui_scene_rect_stroke(scene, r, l.focus_w,
                                  ui_color_opacity(
                                      focus.has_border_color
                                          ? focus.border_color : 0, fade)))
            return false;
        if (!ui_scene_end_scope(scene))
            return false;
    }

    #undef TB_TAB_HOT

    if (clipped_right) {
        int fade_w = l.fade_w;
        int edge_x = size.x - l.pad.r;
        if (fade_w > edge_x) fade_w = edge_x;
        if (fade_w > 0) {
            static const uint8_t stops[4] = { 0x18, 0x58, 0xA0, 0xE8 };
            int step = fade_w / 4;
            int rem = fade_w - step * 4;
            int fx = edge_x - fade_w;
            for (int s = 0; s < 4; s++) {
                int sw = step + (s == 3 ? rem : 0);
                if (sw <= 0)
                    continue;
                if (!ui_scene_blend_rect(scene, (struct rect2i){
                        .pos = { .x = fx, .y = 0 },
                        .size = { .x = sw, .y = l.height } },
                        bar_color, stops[s]))
                    return false;
                fx += sw;
            }
        }
    }

    struct rect2i nr;
    bool has_new = false;
    if (l.new_tab_width > 0) {
        int nx = (int)roundf(base_x);
        if (tb->state.external_drag)
            nx += (int)roundf(l.share) + l.gap;
        int max_x = size.x - l.pad.r - l.new_tab_width;
        if (nx > max_x)
            nx = max_x;
        if (nx >= l.pad.l) {
            int new_y = l.pad.t + (l.height - l.pad.t - l.pad.b - l.new_tab_height) / 2;
            nr = (struct rect2i){
                .pos = { .x = nx, .y = new_y },
                .size = { .x = l.new_tab_width, .y = l.new_tab_height },
            };
            has_new = true;
        }
    }
    if (has_new) {
        if (!ui_scene_begin_part(scene, TB_PART_NEW))
            return false;
        float progress = ui_hover_anim_value(&tb->state.new_hover);
        if (progress > 0.001f) {
            uint32_t hover_bg = fresh_h.has_bg_color
                ? fresh_h.bg_color : tab_hover_bg;
            float alpha = css_style_effective_opacity(&fresh_h) * progress;
            uint32_t hbg = ui_color_opacity(hover_bg, alpha);
            int crad = nr.height < nr.width ? nr.height / 2 : nr.width / 2;
            struct vector2i cc = { .x = nr.x + nr.width / 2, .y = nr.y + nr.height / 2 };
            if (!ui_scene_circle(scene, cc, crad, hbg, (uint8_t)(hbg >> 24)))
                return false;
        }
        struct ui_text_style n_style = {
            .advance = advance,
            .cell_h = text_h,
            .flags = UI_TEXT_PLAIN,
        };
        struct rect2i n_text_r = {
            .pos = { .x = nr.x + (nr.width - advance) / 2,
                     .y = nr.y + (nr.height - text_h) / 2 },
            .size = { .x = advance, .y = text_h },
        };
        uint32_t n_color = progress > 0.001f
            ? (fresh_h.has_text_color ? fresh_h.text_color : active_text)
            : (fresh.has_text_color ? fresh.text_color : text_color);
        if (!ui_scene_text_run(scene, n_text_r, n_style, n_color, TB_NEW_MARK,
                               sizeof(TB_NEW_MARK) - 1))
            return false;
        if (ui_widget_has_focus(&tb->widget) &&
            tb->state.keyboard_index == n_tabs &&
            !ui_scene_rect_stroke(scene, nr, l.focus_w,
                                  focus.has_border_color
                                      ? focus.border_color : 0))
            return false;
        if (!ui_scene_end_scope(scene))
            return false;
    }

    struct rect2i drag_r;
    if (dragging && tb_drag_rect(tb, &l, n_tabs, size, &drag_r)) {
        if (!ui_scene_begin_part(scene, TB_PART_DRAG))
            return false;
        struct rect2i r = drag_r;
        int radius = l.radius;
        struct vector2i shadow_at = card.has_shadow
            ? (struct vector2i){
                  .x = ui_css_resolve_int(card.shadow_x, 0, 0, ctx, false),
                  .y = ui_css_resolve_int(card.shadow_y, 0, 0, ctx, false),
              } : (struct vector2i){0};
        struct rect2i sh = rect2i_translate(r, shadow_at);
        if (!ui_scene_rounded_rect(scene, sh, radius > 0 ? radius + 2 : 0,
                                   card.has_shadow ? card.shadow_color : 0))
            return false;

        uint32_t bg = ui_color_opacity(
            tab_active_bg,
            tb->state.drag_state == UI_TAB_DRAG_DETACH ? 0.85f : 0.95f);
        uint32_t bcol = ui_color_opacity(drop.has_bg_color ? drop.bg_color
                                                          : tab_active_bg, 0.8f);
        if (!ui_scene_rounded_rect(scene, r, radius, bg) ||
            !ui_scene_rect_stroke(scene, r, l.stroke_w, bcol))
            return false;

        const char *label = tabs[drag_idx].text && tabs[drag_idx].text[0]
                            ? tabs[drag_idx].text : "shell";
        struct rect2i close_box = tb_close_box(r, l.close_w, l.close_h,
                                               l.close_pad);
        int text_x = r.x + l.tab_pad;
        int avail = close_box.x - l.close_gap - text_x;
        int cap = advance > 0 && avail > 0 ? avail / advance : 0;
        if (!tb_title(scene,
                      (struct vector2i){ .x = text_x,
                                         .y = r.y + (r.height - text_h) / 2 },
                      advance, text_h, tb_text_color(tb, &active), true, label,
                      cap))
            return false;

        struct ui_text_style m_style = {
            .advance = advance,
            .cell_h = text_h,
            .flags = UI_TEXT_PLAIN,
        };
        struct rect2i mr = {
            .pos = { .x = close_box.x + (close_box.width - advance) / 2,
                     .y = close_box.y + (close_box.height - text_h) / 2 },
            .size = { .x = advance, .y = text_h },
        };
        if (!ui_scene_text_run(scene, mr, m_style, tb_text_color(tb, &close_h),
                               TB_CLOSE_MARK, sizeof(TB_CLOSE_MARK) - 1) ||
            !ui_scene_end_scope(scene))
            return false;
    }

    if (!ui_scene_rect(scene, (struct rect2i){
            .pos = { .x = 0, .y = l.height - 1 },
            .size = { .x = size.x, .y = 1 } },
            bar.has_border_color ? bar.border_color : bar_bg))
        return false;

    if (bar_clip && !ui_scene_pop_clip(scene))
        return false;

    return true;
}
