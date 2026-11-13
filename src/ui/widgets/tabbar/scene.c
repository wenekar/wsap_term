#include "ui/widgets/tabbar/internal.h"

#include "paint/text_run.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"

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

static float tb_open_fade(float open)
{
    if (open <= 0.0f)
        return 0.0f;
    if (open >= 0.6f)
        return 1.0f;
    return open / 0.6f;
}

static bool scene_stroke(struct ui_scene *scene, struct rect2i bounds,
                         int width, uint32_t color)
{
    if (width <= 0 || rect2i_is_empty(bounds))
        return true;
    if (width * 2 > bounds.width)
        width = bounds.width / 2;
    if (width * 2 > bounds.height)
        width = bounds.height / 2;
    if (width <= 0)
        return true;

    struct rect2i inner = rect2i_inset(bounds, width);
    if (!ui_scene_rect(scene, (struct rect2i){
            .pos = bounds.pos,
            .size = { .x = bounds.width, .y = width },
        }, color) ||
        !ui_scene_rect(scene, (struct rect2i){
            .pos = { .x = bounds.x, .y = bounds.y + bounds.height - width },
            .size = { .x = bounds.width, .y = width },
        }, color))
        return false;
    if (inner.height <= 0)
        return true;
    return ui_scene_rect(scene, (struct rect2i){
            .pos = { .x = bounds.x, .y = bounds.y + width },
            .size = { .x = width, .y = inner.height },
        }, color) &&
        ui_scene_rect(scene, (struct rect2i){
            .pos = { .x = bounds.x + bounds.width - width,
                     .y = bounds.y + width },
            .size = { .x = width, .y = inner.height },
        }, color);
}

bool ui_tabbar_build_drag_scene(struct ui_tabbar *tb, int index,
                                const struct ui_image *preview_image,
                                struct ui_scene *scene,
                                struct vector2i *image_size,
                                struct vector2i *hotspot)
{
    if (!tb || !scene || !image_size || !hotspot ||
        index < 0 || index >= tb->impl.n_tabs)
        return false;
    const struct ui_tab_label *tabs = tb->impl.labels;
    struct tb_theme snapshot;
    tb_theme_read(tb, &snapshot);
    const struct tb_theme *t = &snapshot;
    struct rect2i tab_rect;
    if (!tb_tab_rect_at(tb, t, tb->impl.n_tabs, tb->impl.size, index, &tab_rect))
        return false;
    ui_scene_init(scene);

    bool has_preview = preview_image && preview_image->pixels &&
                       preview_image->size.x > 0 && preview_image->size.y > 0;

    int pad = t->preview_pad;
    int gap = t->preview_gap;
    int card_w = has_preview ? preview_image->size.x + pad * 2 : 0;
    int card_h = has_preview ? preview_image->size.y + pad * 2 : 0;

    int total_w = tab_rect.width;
    if (has_preview && card_w > total_w)
        total_w = card_w;

    int total_h = tab_rect.height;
    if (has_preview)
        total_h += gap + card_h;

    int radius = t->tab_radius;
    struct vector2i shadow_at = t->preview_shadow_offset;
    int blur = t->preview_shadow_blur;
    struct rect2i r = { .pos = { .x = 0, .y = 0 }, .size = tab_rect.size };
    if (!ui_scene_shadow(scene, (struct rect2i){
            .pos = { .x = shadow_at.x, .y = shadow_at.y }, .size = r.size },
            radius + 2, blur, t->preview_shadow) ||
        !ui_scene_rounded_rect(scene, r, radius, t->tab_active_bg))
        goto fail;

    int advance = ui_cell_dim(tb->widget.style_scale.cell_w, ui_ctx_px(tb_ctx(tb), 8));
    int text_h = ui_cell_dim(tb->widget.style_scale.cell_h, ui_ctx_px(tb_ctx(tb), 16));
    struct rect2i close = tb_close_box(r, t->close_side,
                                       t->close_side_h,
                                       t->close_pad);
    int text_x = t->tab_pad;
    int avail = close.x - ui_ctx_px(tb_ctx(tb), 4) - text_x;
    int cap = advance > 0 && avail > 0 ? avail / advance : 0;
    const char *label = tabs[index].text && tabs[index].text[0]
        ? tabs[index].text : "shell";
    size_t len = ui_text_run_fit(label, strlen(label), cap);
    if (len > 0 && !ui_scene_text_run(scene, (struct rect2i){
            .pos = { .x = text_x, .y = (r.height - text_h) / 2 },
            .size = { .x = avail, .y = text_h },
        }, (struct ui_text_style){
            .advance = advance, .cell_h = text_h, .flags = UI_TEXT_BOLD,
        }, t->active_text_color, label, len))
        goto fail;

    struct ui_text_style mark_style = {
        .advance = advance,
        .cell_h = text_h,
        .flags = UI_TEXT_PLAIN,
    };
    struct rect2i mark = {
        .pos = { .x = close.x + (close.width - advance) / 2,
                 .y = close.y + (close.height - text_h) / 2 },
        .size = { .x = advance, .y = text_h },
    };
    if (!ui_scene_text_run(scene, mark, mark_style, t->close_color,
                           TB_CLOSE_MARK, sizeof(TB_CLOSE_MARK) - 1))
        goto fail;

    if (has_preview) {
        struct rect2i card = {
            .pos = { .x = 0, .y = tab_rect.height + gap },
            .size = { .x = card_w, .y = card_h },
        };
        if (!ui_scene_shadow(scene, (struct rect2i){
                .pos = { .x = card.x + shadow_at.x, .y = card.y + shadow_at.y },
                .size = card.size }, t->preview_radius, blur,
                t->preview_shadow) ||
            !ui_scene_rounded_rect(scene, card, t->preview_radius,
                                   t->preview_bg))
            goto fail;
        struct vector2i img_pos = {
            .x = card.x + pad,
            .y = card.y + pad,
        };
        if (!ui_scene_image(scene, img_pos, preview_image))
            goto fail;
    }

    *image_size = (struct vector2i){ .x = total_w, .y = total_h };
    hotspot->x = tb->impl.drag_grab_x;
    hotspot->y = tb->impl.drag_origin.y - tab_rect.y;
    if (hotspot->x < 0) hotspot->x = 0;
    if (hotspot->x >= total_w) hotspot->x = total_w - 1;
    if (hotspot->y < 0) hotspot->y = 0;
    if (hotspot->y >= total_h) hotspot->y = total_h - 1;
    return true;

fail:
    ui_scene_destroy(scene);
    return false;
}

bool tb_scene_emit(struct ui_tabbar *tb, const struct tb_theme *t,
                   const struct ui_tab_label *tabs, int n_tabs,
                   struct vector2i size, struct ui_scene *scene,
                   uint64_t now_ms)
{
    if (!scene || !tabs || n_tabs < 1 || size.x <= 0)
        return false;
    if (tb && !tb_ensure_tabs(tb, n_tabs))
        return false;
    struct tb_layout l = tb_layout_for(tb, t, n_tabs, size);
    if (l.height <= 0)
        return false;

    uint32_t bar_bg = ui_color_opacity(t->bar_bg, t->bar_opacity);
    if (!ui_scene_rect(scene, (struct rect2i){ .size = { .x = size.x, .y = l.height } }, bar_bg))
        return false;

    bool bar_clip = tb && tb->impl.reveal.start_ms != 0 &&
        tb->impl.reveal.value < 1.0f;
    if (bar_clip &&
        !ui_scene_push_clip(scene, (struct rect2i){
            .size = { .x = size.x, .y = l.height } }))
        return false;

    int advance = ui_cell_dim(tb ? tb->widget.style_scale.cell_w : 0, ui_ctx_px(tb_ctx(tb), 8));
    int text_h = ui_cell_dim(tb ? tb->widget.style_scale.cell_h : 0, ui_ctx_px(tb_ctx(tb), 16));

    bool dragging = tb && (tb->impl.drag_state == UI_TAB_DRAG_REORDER ||
                           tb->impl.drag_state == UI_TAB_DRAG_DETACH);
    int drag_idx = dragging ? tb->impl.drag_tab : -1;

    float base_x = (float)l.pad.l;
    int tab_y = l.pad.t + (l.height - l.pad.t - l.pad.b - l.tab_height) / 2;
    bool clipped_right = false;

    struct rect2i drop_r;
    if (tb && tb_drop_rect(tb, &l, size, &drop_r)) {
        if (!ui_scene_begin_part(scene, TB_PART_DROP))
            return false;
        struct rect2i shadow = rect2i_translate(
            drop_r, t->drop_preview_shadow_offset);
        if ((t->drop_preview_shadow >> 24) &&
            !ui_scene_shadow(scene, shadow, t->tab_radius,
                             t->drop_preview_shadow_blur,
                             t->drop_preview_shadow))
            return false;
        uint32_t bg = ui_color_opacity(t->drop_preview_bg,
                                       t->drop_preview_opacity);
        if (t->tab_radius > 0) {
            if (!ui_scene_rounded_rect(scene, drop_r, t->tab_radius, bg))
                return false;
        } else if (!ui_scene_rect(scene, drop_r, bg)) {
            return false;
        }
        if (!ui_scene_end_scope(scene))
            return false;
    }

    #define TB_TAB_HOT(idx)                                                 \
        (tabs[idx].active || (idx) == drag_idx ||                           \
         (tb && ((tb->impl.hover_tab == (idx) || tb->impl.hover_close == (idx) ||     \
                 tb->impl.pressed_tab == (idx)))))

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
            uint32_t sep_c = ui_color_opacity(t->sep_color, sep_fade);
            if (!ui_scene_rect(scene, (struct rect2i){
                    .pos = { .x = sep_x, .y = r.y + (r.height - sep_h) / 2 },
                    .size = { .x = 1, .y = sep_h } },
                    sep_c))
                return false;
        }

        uint32_t bg_c;
        if (tabs[i].active)
            bg_c = t->tab_active_bg;
        else if (tb && tb->impl.hover_tab == i)
            bg_c = t->tab_hover_bg;
        else
            bg_c = t->tab_idle_bg;

        if (t->tab_radius > 0) {
            if (!ui_scene_rounded_rect(scene, r, t->tab_radius, bg_c))
                return false;
        } else {
            if (!ui_scene_rect(scene, r, bg_c))
                return false;
        }

        if (tb && i < tb->impl.tab_cap)
            ui_scene_ripple(scene, r, &tb->impl.tabs[i].ripple, now_ms);

        bool content_clip = open < 1.0f;
        if (content_clip && !ui_scene_push_clip(scene, r))
            return false;

        struct rect2i close;
        bool has_close = false;
        if (content_clip || r.width >= t->close_side + t->close_pad * 2) {
            close = tb_close_box(r, t->close_side, t->close_side_h, t->close_pad);
            has_close = true;
        }
        int title_right = has_close ? close.x - ui_ctx_px(tb_ctx(tb), 4)
                                    : rect2i_end(r).x - t->tab_pad;

        const char *label = tabs[i].text && tabs[i].text[0] ? tabs[i].text : "shell";
        int text_x = r.x + t->tab_pad;
        int avail = title_right - text_x;
        int cap = advance > 0 && avail > 0 ? avail / advance : 0;
        size_t len = ui_text_run_fit(label, strlen(label), cap);
        if (len > 0) {
            uint32_t txt_c = tabs[i].active ? t->active_text_color : t->text_color;
            txt_c = ui_color_opacity(txt_c, fade);
            struct ui_text_style t_style = {
                .advance = advance,
                .cell_h = text_h,
                .flags = tabs[i].active ? UI_TEXT_BOLD : UI_TEXT_PLAIN,
            };
            int pad_t = (r.height - text_h) / 2;
            if (pad_t < 0) pad_t = 0;
            struct rect2i tr = {
                .pos = { .x = text_x, .y = r.y + pad_t },
                .size = { .x = (int)len * advance, .y = text_h },
            };
            if (!ui_scene_text_run(scene, tr, t_style, txt_c, label, len))
                return false;
        }

        if (has_close) {
            float progress = (tb && i < tb->impl.tab_cap)
                ? ui_hover_anim_value(&tb->impl.tabs[i].close_hover) : 0.0f;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;

            int crad = close.height < close.width ? close.height / 2 : close.width / 2;
            struct vector2i cc = {
                .x = close.x + close.width / 2,
                .y = close.y + close.height / 2,
            };
            if (progress > 0.0f && t->close_h_has_bg) {
                uint32_t c = ui_color_opacity(t->close_h_bg, t->close_h_opacity * progress * fade);
                if (!ui_scene_circle(scene, cc, crad, c, (uint8_t)(c >> 24)))
                    return false;
            } else if (t->close_s_has_bg) {
                uint32_t c = ui_color_opacity(t->close_s_bg, t->close_s_opacity * (1.0f - progress) * fade);
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
                    int bx = close.x + (close.width - b_w) / 2;
                    if (bx < r.x) bx = r.x;
                    if (bx + b_w > rect2i_end(r).x) bx = rect2i_end(r).x - b_w;
                    struct ui_text_style b_style = {
                        .advance = advance,
                        .cell_h = text_h,
                        .flags = UI_TEXT_PLAIN,
                    };
                    struct rect2i br = {
                        .pos = { .x = bx, .y = close.y + (close.height - text_h) / 2 },
                        .size = { .x = b_w, .y = text_h },
                    };
                    uint32_t badge_c = ui_color_opacity(t->close_color, badge_alpha);
                    if (!ui_scene_text_run(scene, br, b_style, badge_c, badge, blen))
                        return false;
                }
            }

            float close_alpha = progress * fade;
            if (close_alpha > 0.001f) {
                uint32_t mark_c = ui_color_opacity(t->close_hover_color, close_alpha);
                struct ui_text_style m_style = {
                    .advance = advance,
                    .cell_h = text_h,
                    .flags = UI_TEXT_PLAIN,
                };
                struct rect2i mr = {
                    .pos = { .x = close.x + (close.width - advance) / 2,
                             .y = close.y + (close.height - text_h) / 2 },
                    .size = { .x = advance, .y = text_h },
                };
                if (!ui_scene_text_run(scene, mr, m_style, mark_c, TB_CLOSE_MARK,
                                       sizeof(TB_CLOSE_MARK) - 1))
                    return false;
            }
        }

        if (content_clip && !ui_scene_pop_clip(scene))
            return false;
        if (tb && ui_widget_has_focus(&tb->widget) &&
            i == tb->impl.keyboard_index &&
            !scene_stroke(scene, r, t->focus_width,
                          ui_color_opacity(t->focus_color, fade)))
            return false;
        if (!ui_scene_end_scope(scene))
            return false;
    }

    #undef TB_TAB_HOT

    if (clipped_right) {
        int fade_w = ui_ctx_px(tb_ctx(tb), UI_TABBAR_FADE_DP);
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
                        t->bar_bg, stops[s]))
                    return false;
                fx += sw;
            }
        }
    }

    struct rect2i nr;
    bool has_new = false;
    if (l.new_tab_width > 0) {
        int nx = (int)roundf(base_x);
        if (tb && tb->impl.external_drag)
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
        float progress = tb ? ui_hover_anim_value(&tb->impl.new_hover) : 0.0f;
        if (progress > 0.001f) {
            float alpha = t->new_tab_has_hover_bg
                ? progress * t->new_tab_hover_opacity : progress;
            uint32_t hbg = ui_color_opacity(t->new_tab_hover_bg, alpha);
            int crad = nr.height < nr.width ? nr.height / 2 : nr.width / 2;
            struct vector2i cc = { .x = nr.x + nr.width / 2, .y = nr.y + nr.height / 2 };
            ui_scene_circle(scene, cc, crad, hbg, (uint8_t)(hbg >> 24));
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
        uint32_t n_color = progress > 0.001f ? t->new_tab_hover_fg : t->new_tab_fg;
        ui_scene_text_run(scene, n_text_r, n_style, n_color, TB_NEW_MARK, sizeof(TB_NEW_MARK) - 1);
        if (tb && ui_widget_has_focus(&tb->widget) &&
            tb->impl.keyboard_index == n_tabs &&
            !scene_stroke(scene, nr, t->focus_width, t->focus_color))
            return false;
        if (!ui_scene_end_scope(scene))
            return false;
    }

    if (dragging && drag_idx >= 0 && drag_idx < n_tabs) {
        int orig_x, orig_w;
        int tab_y = l.pad.t + (l.height - l.pad.t - l.pad.b - l.tab_height) / 2;
        if (tb_tab_span(tb, &l, n_tabs, size.x, drag_idx, &orig_x, &orig_w)) {
            if (!ui_scene_begin_part(scene, TB_PART_DRAG))
                return false;
            int drag_x = tb->impl.drag_pos.x - tb->impl.drag_grab_x;
            int drag_y = (tb->impl.drag_state == UI_TAB_DRAG_DETACH)
                         ? tb->impl.drag_pos.y - l.tab_height / 2
                         : tab_y;
            if (tb->impl.drag_state == UI_TAB_DRAG_REORDER) {
                if (drag_x < l.pad.l) drag_x = l.pad.l;
                if (drag_x + orig_w > size.x - l.pad.r) drag_x = size.x - l.pad.r - orig_w;
            }
            struct rect2i r = { .pos = { .x = drag_x, .y = drag_y }, .size = { .x = orig_w, .y = l.tab_height } };

            int radius = t->tab_radius;
            struct vector2i shadow_at = t->preview_shadow_offset;
            struct rect2i sh = { .pos = { .x = drag_x + shadow_at.x,
                                          .y = drag_y + shadow_at.y },
                                 .size = { .x = orig_w, .y = l.tab_height } };
            if (radius > 0)
                ui_scene_rounded_rect(scene, sh, radius + 2,
                                      t->preview_shadow);
            else
                ui_scene_rect(scene, sh, t->preview_shadow);

            uint32_t bg = ui_color_opacity(t->tab_active_bg,
                                           tb->impl.drag_state == UI_TAB_DRAG_DETACH ? 0.85f : 0.95f);
            if (radius > 0)
                ui_scene_rounded_rect(scene, r, radius, bg);
            else
                ui_scene_rect(scene, r, bg);

            uint32_t bcol = ui_color_opacity(t->drop_preview_bg, 0.8f);
            if (radius > 0)
                ui_scene_rounded_rect(scene, r, radius, bcol);

            const char *label = tabs[drag_idx].text && tabs[drag_idx].text[0]
                                ? tabs[drag_idx].text : "shell";
            struct rect2i close = tb_close_box(r, t->close_side, t->close_side_h, t->close_pad);
            int text_x = r.x + t->tab_pad;
            int avail = close.x - ui_ctx_px(tb_ctx(tb), 4) - text_x;
            int cap = advance > 0 && avail > 0 ? avail / advance : 0;
            size_t len = ui_text_run_fit(label, strlen(label), cap);
            if (len > 0) {
                struct ui_text_style style = {
                    .advance = advance,
                    .cell_h = text_h,
                    .flags = UI_TEXT_BOLD,
                };
                struct rect2i tr = {
                    .pos = { .x = text_x, .y = r.y + (r.height - text_h) / 2 },
                    .size = { .x = avail, .y = text_h },
                };
                ui_scene_text_run(scene, tr, style, t->active_text_color, label, len);
            }

            struct ui_text_style m_style = {
                .advance = advance,
                .cell_h = text_h,
                .flags = UI_TEXT_PLAIN,
            };
            struct rect2i mr = {
                .pos = { .x = close.x + (close.width - advance) / 2, .y = close.y + (close.height - text_h) / 2 },
                .size = { .x = advance, .y = text_h },
            };
            ui_scene_text_run(scene, mr, m_style, t->close_hover_color,
                              TB_CLOSE_MARK, sizeof(TB_CLOSE_MARK) - 1);
            if (!ui_scene_end_scope(scene))
                return false;
        }
    }

    if (!ui_scene_rect(scene, (struct rect2i){
            .pos = { .x = 0, .y = l.height - 1 },
            .size = { .x = size.x, .y = 1 } },
            t->border_color))
        return false;

    if (bar_clip && !ui_scene_pop_clip(scene))
        return false;

    return true;
}
