#include "ui/widgets/tabbar/internal.h"

#include "ui/ui_util.h"
#include "ui/widget_impl.h"

#include <math.h>

#define UI_TABBAR_BG         0xFF181825
#define UI_TABBAR_TAB_BG     0xFF1E1E2E
#define UI_TABBAR_ACTIVE_BG  0xFF313244
#define UI_TABBAR_HOVER_BG   0xFF262637
#define UI_TABBAR_TEXT       0xFFBAC2DE
#define UI_TABBAR_ACTIVE_TXT 0xFFFFFFFF
#define UI_TABBAR_FOCUS      0xFF89B4FA
#define UI_TABBAR_CLOSE      0xFF9399B2
#define UI_TABBAR_PREVIEW_BG 0xEE1E1E2Eu

static struct {
    ui_part_id tab, tab_active, tab_drop, tab_close, tab_new, tab_sep,
        tab_preview, bar;
} tb_parts;

static void tb_intern_parts(void)
{
    if (tb_parts.bar)
        return;
    tb_parts.tab = ui_part_intern("tab");
    tb_parts.tab_active = ui_part_intern("tab active");
    tb_parts.tab_drop = ui_part_intern("tab drop-preview");
    tb_parts.tab_close = ui_part_intern("tab-close");
    tb_parts.tab_new = ui_part_intern("tab-new");
    tb_parts.tab_sep = ui_part_intern("tab-separator");
    tb_parts.tab_preview = ui_part_intern("tab-preview");
    tb_parts.bar = ui_part_intern("tab-bar");
}

static int bar_css_height(const struct css_scale_context *ctx,
                          const struct css_style *bar)
{
    float def_h = (float)ui_ctx_px(ctx, UI_TABBAR_HEIGHT_DP);
    int h = ui_ctx_size(ctx, bar->height, def_h);
    return h < 1 ? (int)roundf(def_h) : h;
}

int tb_css_height(const struct ui_tabbar *tb)
{
    tb_intern_parts();
    struct css_style bar = ui_widget_part(&tb->widget, tb_parts.bar, CSS_PSEUDO_NONE);
    return bar_css_height(tb_ctx(tb), &bar);
}

void tb_theme_read(const struct ui_tabbar *tb, struct tb_theme *t)
{
    *t = (struct tb_theme){0};
    if (!tb)
        return;
    tb_intern_parts();
    const struct css_scale_context *ctx = tb_ctx(tb);

    struct css_style bar = ui_widget_part(&tb->widget, tb_parts.bar, CSS_PSEUDO_NONE);
    struct css_style idle = ui_widget_part(&tb->widget, tb_parts.tab, CSS_PSEUDO_NONE);
    struct css_style active = ui_widget_part(&tb->widget, tb_parts.tab_active, CSS_PSEUDO_NONE);
    struct css_style hover = ui_widget_part(&tb->widget, tb_parts.tab, CSS_PSEUDO_HOVER);
    struct css_style focus = ui_widget_part(&tb->widget, tb_parts.tab, CSS_PSEUDO_FOCUS);
    struct css_style drop = ui_widget_part(&tb->widget, tb_parts.tab_drop, CSS_PSEUDO_NONE);
    struct css_style close_s = ui_widget_part(&tb->widget, tb_parts.tab_close, CSS_PSEUDO_NONE);
    struct css_style close_h = ui_widget_part(&tb->widget, tb_parts.tab_close, CSS_PSEUDO_HOVER);
    struct css_style newtab = ui_widget_part(&tb->widget, tb_parts.tab_new, CSS_PSEUDO_NONE);
    struct css_style newtab_h = ui_widget_part(&tb->widget, tb_parts.tab_new, CSS_PSEUDO_HOVER);
    struct css_style sep = ui_widget_part(&tb->widget, tb_parts.tab_sep, CSS_PSEUDO_NONE);
    struct css_style card = ui_widget_part(&tb->widget, tb_parts.tab_preview, CSS_PSEUDO_NONE);

    uint32_t def_bg = tb->impl.bg_color ? tb->impl.bg_color : UI_TABBAR_BG;
    t->bar_bg = bar.has_bg_color ? bar.bg_color : def_bg;
    t->bar_opacity = bar.opacity;
    t->has_border_color = bar.has_border_color;
    t->border_color = bar.has_border_color ? bar.border_color : def_bg;

    t->tab_idle_bg = idle.has_bg_color ? idle.bg_color : UI_TABBAR_TAB_BG;
    t->tab_active_bg = active.has_bg_color ? active.bg_color : UI_TABBAR_ACTIVE_BG;
    t->tab_hover_bg = hover.has_bg_color ? hover.bg_color : UI_TABBAR_HOVER_BG;
    t->drop_preview_bg = drop.has_bg_color ? drop.bg_color : t->tab_active_bg;
    t->drop_preview_opacity = drop.has_opacity ? drop.opacity : 0.45f;
    t->drop_preview_shadow = drop.has_shadow ? drop.shadow_color : 0x66000000u;
    t->drop_preview_shadow_offset = drop.has_shadow
        ? (struct vector2i){
            .x = ui_css_resolve_int(drop.shadow_x, 0, 0, ctx, false),
            .y = ui_css_resolve_int(drop.shadow_y, 0, 0, ctx, false),
        } : (struct vector2i){ .y = ui_ctx_px(ctx, 2) };
    t->drop_preview_shadow_blur = drop.has_shadow
        ? ui_css_resolve_int(drop.shadow_blur, 0, 0, ctx, true)
        : ui_ctx_px(ctx, 5);

    t->text_color = idle.has_text_color ? idle.text_color : UI_TABBAR_TEXT;
    t->active_text_color = active.has_text_color ? active.text_color : UI_TABBAR_ACTIVE_TXT;
    t->focus_color = focus.has_border_color ? focus.border_color : UI_TABBAR_FOCUS;
    t->focus_width = focus.has_border_color
        ? ui_css_resolve_int(focus.border_width, 0, 0, ctx, true)
        : ui_ctx_px(ctx, 2);
    if (t->focus_width <= 0)
        t->focus_width = ui_ctx_px(ctx, 2);

    t->close_color = close_s.has_text_color ? close_s.text_color : UI_TABBAR_CLOSE;
    t->close_hover_color = close_h.has_text_color ? close_h.text_color : UI_TABBAR_ACTIVE_TXT;

    t->close_s_has_bg = close_s.has_bg_color;
    t->close_s_bg = close_s.bg_color;
    t->close_s_opacity = close_s.opacity;

    t->close_h_has_bg = close_h.has_bg_color;
    t->close_h_bg = close_h.bg_color;
    t->close_h_opacity = close_h.opacity;

    t->new_tab_has_hover_bg = newtab_h.has_bg_color;
    t->new_tab_hover_bg = newtab_h.has_bg_color ? newtab_h.bg_color : t->tab_hover_bg;
    t->new_tab_hover_opacity = newtab_h.opacity;
    t->sep_color = sep.has_text_color
        ? sep.text_color
        : ui_color_opacity(t->text_color, 0.30f);

    t->preview_bg = card.has_bg_color ? card.bg_color : UI_TABBAR_PREVIEW_BG;
    t->preview_radius = ui_ctx_size(ctx, card.border_radius, 0);
    t->preview_pad = ui_ctx_inset(ctx, card.padding.v[0], 0);
    t->preview_gap = ui_ctx_inset(ctx, card.gap, 0);
    t->preview_shadow = card.has_shadow ? card.shadow_color : 0x60000000u;
    t->preview_shadow_blur = card.has_shadow
        ? ui_css_resolve_int(card.shadow_blur, 0, 0, ctx, true)
        : ui_ctx_px(ctx, 5);
    t->preview_shadow_offset = card.has_shadow
        ? (struct vector2i){
            .x = ui_css_resolve_int(card.shadow_x, 0, 0, ctx, false),
            .y = ui_css_resolve_int(card.shadow_y, 0, 0, ctx, false),
        } : (struct vector2i){ .x = 1, .y = 3 };
    if (t->preview_radius <= 0)
        t->preview_radius = ui_ctx_px(ctx, 8);
    if (t->preview_pad <= 0)
        t->preview_pad = ui_ctx_px(ctx, 8);
    if (t->preview_gap <= 0)
        t->preview_gap = ui_ctx_px(ctx, 6);

    t->ripple_color = idle.has_ripple_color ? idle.ripple_color : 0x30FFFFFF;
    t->ripple_duration_ms = idle.has_transition_duration
        ? (uint32_t)idle.transition_duration : 300;

    t->new_tab_fg = newtab.has_text_color ? newtab.text_color : t->text_color;
    t->new_tab_hover_fg = newtab_h.has_text_color ? newtab_h.text_color : t->active_text_color;
    t->new_tab_has_hover_fg = newtab_h.has_text_color;

    t->height = bar_css_height(ctx, &bar);

    struct insets2i pad = ui_ctx_insets(ctx, bar.padding, (float)ui_ctx_px(ctx, 4));
    t->pad_l = pad.l;
    t->pad_r = pad.r;
    t->pad_t = pad.t;
    t->pad_b = pad.b;
    t->gap = ui_ctx_inset(ctx, bar.gap, (float)ui_ctx_px(ctx, 4));

    int inner_h = t->height - t->pad_t - t->pad_b;
    if (inner_h < 1)
        inner_h = t->height;

    t->tab_height = ui_ctx_size(ctx, idle.height, (float)inner_h);
    if (t->tab_height > inner_h || t->tab_height < 1)
        t->tab_height = inner_h;

    t->tab_radius = ui_ctx_size(ctx, idle.border_radius, 4.0f);
    t->tab_pad = ui_ctx_size(ctx, idle.padding.left, (float)ui_ctx_px(ctx, UI_TABBAR_PAD_DP));

    int advance = ui_cell_dim(ctx ? ctx->cell_w : 0, ui_ctx_px(ctx, 8));
    int text_h = ui_cell_dim(ctx ? ctx->cell_h : 0, ui_ctx_px(ctx, 16));
    int close_side = ui_ctx_size(ctx, close_s.width, (float)text_h);
    int close_side_h = ui_ctx_size(ctx, close_s.height, (float)text_h);
    if (close_side < 1) close_side = text_h;
    if (close_side_h < 1) close_side_h = text_h;
    int close_pad = ui_ctx_size(ctx, close_s.padding.right, (float)ui_ctx_px(ctx, 4));
    t->close_side = close_side;
    t->close_side_h = close_side_h;
    t->close_pad = close_pad;

    int new_pad_l = ui_ctx_size(ctx, newtab.padding.left, (float)ui_ctx_px(ctx, UI_TABBAR_PAD_DP));
    int new_pad_r = ui_ctx_size(ctx, newtab.padding.right, (float)ui_ctx_px(ctx, UI_TABBAR_PAD_DP));
    t->new_tab_w = advance + new_pad_l + new_pad_r;
    t->new_tab_h = ui_ctx_size(ctx, newtab.height, (float)t->tab_height);
    if (t->new_tab_h > inner_h || t->new_tab_h < 1)
        t->new_tab_h = t->tab_height;
}
