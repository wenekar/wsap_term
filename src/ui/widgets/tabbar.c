#include "ui/widgets/tabbar.h"

#include "ui/widget_impl.h"
#include "ui/debug.h"
#include "ui/paint.h"
#include "ui/surface.h"
#include "paint/text_run.h"
#include "ui/ui_util.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

#define UI_TABBAR_BG         0xFF181825
#define UI_TABBAR_TAB_BG     0xFF1E1E2E
#define UI_TABBAR_ACTIVE_BG  0xFF313244
#define UI_TABBAR_HOVER_BG   0xFF262637
#define UI_TABBAR_TEXT       0xFFBAC2DE
#define UI_TABBAR_ACTIVE_TXT 0xFFFFFFFF
#define UI_TABBAR_FOCUS      0xFF89B4FA
#define UI_TABBAR_CLOSE      0xFF9399B2
#define UI_TABBAR_PREVIEW_BG 0xEE1E1E2Eu

struct tb_theme {
    uint32_t bar_bg;
    uint32_t border_color;
    uint32_t tab_idle_bg;
    uint32_t tab_active_bg;
    uint32_t tab_hover_bg;
    uint32_t drop_preview_bg;
    uint32_t drop_preview_shadow;
    uint32_t text_color;
    uint32_t active_text_color;
    uint32_t focus_color;
    uint32_t close_color;
    uint32_t close_hover_color;
    uint32_t close_s_bg;
    uint32_t close_h_bg;
    uint32_t new_tab_fg;
    uint32_t new_tab_hover_fg;
    uint32_t new_tab_hover_bg;
    uint32_t sep_color;
    uint32_t preview_bg;
    uint32_t preview_shadow;
    struct vector2i preview_shadow_offset;
    int preview_pad;
    int preview_gap;
    int preview_radius;
    int preview_shadow_blur;
    uint32_t ripple_color;
    uint32_t ripple_duration_ms;
    bool has_border_color;
    bool close_s_has_bg;
    bool close_h_has_bg;
    bool new_tab_has_hover_bg;
    bool new_tab_has_hover_fg;
    float close_s_opacity;
    float close_h_opacity;
    float new_tab_hover_opacity;
    float bar_opacity;
    float drop_preview_opacity;
    struct vector2i drop_preview_shadow_offset;
    int drop_preview_shadow_blur;
    int height;
    int tab_height;
    int tab_radius;
    int focus_width;
    int pad_l, pad_r, pad_t, pad_b;
    int gap;
    int tab_pad;
    int close_side, close_side_h, close_pad;
    int new_tab_w, new_tab_h;
};

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

enum drag_finish {
    FINISH_NONE = 0,
    FINISH_REORDER,
    FINISH_DETACH,
};

struct tb_hit {
    int index;
    bool on_close;
    bool on_new;
};

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

static bool tabbar_emit_scene(struct ui_tabbar *tb,
                              const struct tb_theme *t,
                              const struct ui_tab_label *tabs, int n_tabs,
                              struct vector2i size, struct ui_scene *scene,
                              uint64_t now_ms);
static int bar_height(const struct ui_tabbar *tb, int css_height, int n_tabs);

static inline const struct css_scale_context *tb_ctx(const struct ui_tabbar *tb)
{
    return &tb->widget.style_scale;
}

static int bar_css_height(const struct css_scale_context *ctx,
                          const struct css_style *bar)
{
    float def_h = (float)ui_ctx_px(ctx, UI_TABBAR_HEIGHT_DP);
    int h = ui_ctx_size(ctx, bar->height, def_h);
    return h < 1 ? (int)roundf(def_h) : h;
}

static int tb_css_height(const struct ui_tabbar *tb)
{
    tb_intern_parts();
    struct css_style bar = ui_widget_part(&tb->widget, tb_parts.bar, CSS_PSEUDO_NONE);
    return bar_css_height(tb_ctx(tb), &bar);
}

static void tb_theme_read(const struct ui_tabbar *tb, struct tb_theme *t)
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

struct tb_layout {
    int height;
    struct insets2i pad;
    int gap;
    int tab_height;
    int tab_width;
    int new_tab_width;
    int new_tab_height;
    float n_eff;
    float share;
    int n_tabs;
};

static float tb_open_progress(const struct ui_tabbar *tb, int index)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return 1.0f;
    const struct ui_tab_state *t = &tb->impl.tabs[index];
    if (t->closing)
        return t->open.value > 0.0f ? t->open.value : 0.0f;
    if (t->open.value <= 0.0f && t->open.to <= 0.0f)
        return 1.0f;
    float v = t->open.value;
    if (v < 0.0f) v = 0.0f;
    else if (v > 1.0f) v = 1.0f;
    return v;
}

static float tb_open_fade(float open)
{
    if (open <= 0.0f)
        return 0.0f;
    if (open >= 0.6f)
        return 1.0f;
    return open / 0.6f;
}

static inline float tb_tab_offset(const struct ui_tabbar *tb, int index)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return 0.0f;
    const struct ui_tab_state *t = &tb->impl.tabs[index];
    return t->slide.value;
}

static struct tb_layout tb_layout_for(const struct ui_tabbar *tb,
                                      const struct tb_theme *t, int n_tabs,
                                      struct vector2i size)
{
    if (!tb || n_tabs < 1 || size.x <= 0)
        return (struct tb_layout){0};

    struct tb_layout l = {
        .height = bar_height(tb, t->height, n_tabs),
        .pad = { .l = t->pad_l, .r = t->pad_r, .t = t->pad_t, .b = t->pad_b },
        .gap = t->gap,
        .tab_height = t->tab_height,
        .new_tab_width = t->new_tab_w,
        .new_tab_height = t->new_tab_h,
        .n_tabs = n_tabs,
    };
    if (l.pad.l + l.pad.r > size.x) {
        l.pad.l = size.x / 2;
        l.pad.r = size.x - l.pad.l;
    }
    if (l.pad.t + l.pad.b > t->height) {
        l.pad.t = t->height / 2;
        l.pad.b = t->height - l.pad.t;
    }
    int inner_h = t->height - l.pad.t - l.pad.b;
    if (inner_h < 1) inner_h = t->height;
    if (l.tab_height > inner_h || l.tab_height < 1)
        l.tab_height = inner_h;

    int avail_full = size.x - l.pad.l - l.pad.r - l.new_tab_width;

    float n_eff = tb->impl.external_drag ? 1.0f : 0.0f;
    for (int i = 0; i < n_tabs; i++)
        n_eff += tb_open_progress(tb, i);
    l.n_eff = n_eff;

    float share = 0.0f;
    if (n_eff > 0.0001f)
        share = ((float)avail_full - (float)l.gap * n_eff) / n_eff;
    if (share < 1.0f)
        share = 1.0f;
    l.share = share;

    int slots = n_tabs + (tb->impl.external_drag ? 1 : 0);
    int available = avail_full - l.gap * slots;
    l.tab_width = slots > 0 ? available / slots : 0;
    if (l.tab_width < 1)
        l.tab_width = 1;
    return l;
}

static float tb_tab_width_f(const struct ui_tabbar *tb,
                            const struct tb_layout *l, int index)
{
    return l->share * tb_open_progress(tb, index);
}

static inline int tb_tab_width_at(const struct ui_tabbar *tb,
                                  const struct tb_layout *l, int index)
{
    int w = (int)roundf(tb_tab_width_f(tb, l, index));
    return w > 0 ? w : 0;
}

static int tb_tab_x(const struct ui_tabbar *tb, const struct tb_layout *l, int index)
{

    float x = (float)l->pad.l;
    for (int i = 0; i < index; i++) {
        float open = tb_open_progress(tb, i);
        x += tb_tab_width_f(tb, l, i);
        x += (float)l->gap * open;
    }
    return (int)roundf(x);
}

static struct rect2i tb_close_box(struct rect2i tab_r, int close_w, int close_h, int close_pad)
{
    int tab_right = rect2i_end(tab_r).x;
    return (struct rect2i){
        .pos = {
            .x = tab_right - close_pad - close_w,
            .y = tab_r.y + (tab_r.height - close_h) / 2,
        },
        .size = { .x = close_w, .y = close_h },
    };
}

static bool tb_new_tab_rect(const struct ui_tabbar *tb,
                            const struct tb_layout *l, int n_tabs,
                            int width, struct rect2i *out)
{
    if (l->new_tab_width <= 0)
        return false;
    int x = tb_tab_x(tb, l, n_tabs);
    if (tb->impl.external_drag)
        x += (int)roundf(l->share) + l->gap;
    int max_x = width - l->pad.r - l->new_tab_width;
    if (x > max_x)
        x = max_x;
    if (x < l->pad.l)
        return false;
    int new_y = l->pad.t + (l->height - l->pad.t - l->pad.b
                            - l->new_tab_height) / 2;
    *out = (struct rect2i){
        .pos = { .x = x, .y = new_y },
        .size = { .x = l->new_tab_width, .y = l->new_tab_height },
    };
    return true;
}

static bool tb_drop_rect(const struct ui_tabbar *tb,
                         const struct tb_layout *l, struct vector2i size,
                         struct rect2i *out)
{
    if (!tb->impl.external_drag || tb->impl.external_slot < 0 ||
        tb->impl.external_slot > l->n_tabs)
        return false;
    int x = tb_tab_x(tb, l, tb->impl.external_slot);
    int max_right = size.x - l->pad.r - l->new_tab_width - l->gap;
    int width = (int)roundf(l->share);
    if (width > max_right - x)
        width = max_right - x;
    if (width <= 0)
        return false;
    int y = l->pad.t + (l->height - l->pad.t - l->pad.b - l->tab_height) / 2;
    *out = (struct rect2i){
        .pos = { .x = x, .y = y },
        .size = { .x = width, .y = l->tab_height },
    };
    return true;
}

static bool tb_tab_span(const struct ui_tabbar *tb,
                        const struct tb_layout *l, int n_tabs, int width,
                        int index, int *x, int *w)
{
    (void)n_tabs;
    int tx = tb_tab_x(tb, l, index);
    tx += (int)roundf(tb_tab_offset(tb, index));
    if (tx >= width)
        return false;
    int tw = tb_tab_width_at(tb, l, index);
    if (tx + tw > width - l->pad.r)
        tw = width - l->pad.r - tx;
    if (tw <= 0)
        return false;
    *x = tx;
    *w = tw;
    return true;
}

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

static bool tb_ensure_tabs(struct ui_tabbar *tb, int n)
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

static int bar_height(const struct ui_tabbar *tb, int css_height, int n_tabs)
{
    if (!tb || n_tabs < 1)
        return 0;

    bool armed = tb->impl.reveal.start_ms != 0;
    if (n_tabs <= 1 && !armed && !tb->impl.external_drag)
        return 0;
    float v = armed ? tb->impl.reveal.value : 1.0f;
    if (v <= 0.0f)
        return 0;
    if (v > 1.0f)
        v = 1.0f;
    return (int)roundf((float)css_height * v);
}

static int steady_height(const struct ui_tabbar *tb, const struct tb_theme *t,
                         int n_tabs)
{
    if (!tb || (n_tabs <= 1 && !tb->impl.external_drag))
        return 0;
    return t->height;
}

static int drop_height(const struct ui_tabbar *tb, const struct tb_theme *t,
                       int n_tabs)
{
    if (!tb || n_tabs < 1)
        return 0;
    return t->height;
}

static bool tab_rect_at(const struct ui_tabbar *tb, const struct tb_theme *t,
                        int n_tabs, struct vector2i size,
                        int index, struct rect2i *out)
{
    if (!tb || !out || n_tabs < 1 || index < 0 || index >= n_tabs || size.x <= 0)
        return false;
    struct tb_layout l = tb_layout_for(tb, t, n_tabs, size);
    if (l.height <= 0)
        return false;
    int x, w;
    if (!tb_tab_span(tb, &l, n_tabs, size.x, index, &x, &w))
        return false;
    int tab_y = l.pad.t + (l.height - l.pad.t - l.pad.b - l.tab_height) / 2;
    *out = (struct rect2i){
        .pos = { .x = x, .y = tab_y },
        .size = { .x = w, .y = l.tab_height },
    };
    return true;
}

static bool close_rect_at(const struct ui_tabbar *tb, const struct tb_theme *t,
                          int n_tabs, struct vector2i size,
                          int index, struct rect2i *out)
{
    if (!tb || !out || n_tabs < 1 || index < 0 || index >= n_tabs || size.x <= 0)
        return false;
    struct rect2i tab_r;
    if (!tab_rect_at(tb, t, n_tabs, size, index, &tab_r))
        return false;
    int close_side = t->close_side;
    int close_side_h = t->close_side_h;
    int close_pad = t->close_pad;
    if (tab_r.width < close_side + close_pad * 2)
        return false;
    *out = tb_close_box(tab_r, close_side, close_side_h, close_pad);
    return true;
}

static bool new_rect_at(const struct ui_tabbar *tb, const struct tb_theme *t,
                        int n_tabs, struct vector2i size, struct rect2i *out)
{
    if (!out || n_tabs < 1 || size.x <= 0)
        return false;
    struct tb_layout l = tb_layout_for(tb, t, n_tabs, size);
    if (l.height <= 0)
        return false;
    return tb_new_tab_rect(tb, &l, n_tabs, size.x, out);
}

static struct tb_hit hit_at(const struct ui_tabbar *tb, const struct tb_theme *t,
                            int n_tabs, struct vector2i size, struct vector2i p)
{
    struct tb_hit hit = { .index = -1 };
    if (!tb || n_tabs <= 1 || size.x <= 0 || p.x < 0 || p.x >= size.x || p.y < 0)
        return hit;
    struct tb_layout l = tb_layout_for(tb, t, n_tabs, size);
    if (l.height <= 0 || p.y >= l.height)
        return hit;

    int tab_y = l.pad.t + (l.height - l.pad.t - l.pad.b - l.tab_height) / 2;

    float base_x = (float)l.pad.l;
    int close_hit = -1;
    int tab_hit = -1;

    for (int i = 0; i < n_tabs; i++) {
        float open = tb_open_progress(tb, i);
        int tw = tb_tab_width_at(tb, &l, i);
        int cur_x = (int)roundf(base_x);
        base_x += tb_tab_width_f(tb, &l, i) + (float)l.gap * open;

        int tx = cur_x + (int)roundf(tb_tab_offset(tb, i));
        if (tx >= size.x)
            continue;
        int w = tw;
        if (tx + w > size.x - l.pad.r)
            w = size.x - l.pad.r - tx;
        if (w <= 0)
            continue;

        struct rect2i r = {
            .pos = { .x = tx, .y = tab_y },
            .size = { .x = w, .y = l.tab_height },
        };

        if (w >= t->close_side + t->close_pad * 2) {
            struct rect2i close = tb_close_box(r, t->close_side, t->close_side_h, t->close_pad);
            if (rect2i_contains(close, p) && close_hit < 0)
                close_hit = i;
        }

        if (rect2i_contains(r, p) && tab_hit < 0)
            tab_hit = i;
    }

    if (close_hit >= 0) {
        hit.index = close_hit;
        hit.on_close = true;
        return hit;
    }
    if (tab_hit >= 0) {
        hit.index = tab_hit;
        hit.on_close = false;
        return hit;
    }

    struct rect2i nr;
    if (tb_new_tab_rect(tb, &l, n_tabs, size.x, &nr) &&
        rect2i_contains(nr, p)) {
        hit.on_new = true;
        return hit;
    }

    return hit;
}

static void ripple(struct ui_tabbar *tb, const struct tb_theme *t, int n_tabs,
                   struct vector2i size, int index, struct vector2i click_pos,
                   uint64_t now_ms)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return;
    struct rect2i r;
    if (!tab_rect_at(tb, t, n_tabs, size, index, &r))
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

        int effective = tb->impl.tab_cap -
            (ui_tabbar_closing_tab(tb) >= 0 ? 1 : 0);
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

    if (tb->impl.tab_cap == 2) {
        if (!tb->impl.reveal.start_ms)
            tb->impl.reveal.value = 1.0f;
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

    int h = bar_height(tb, t->height, n_tabs);
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
    int h = drop_height(tb, t, n_tabs);
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
    if (!tab_rect_at(tb, t, tb->impl.n_tabs, tb->impl.size, index, &tab_rect))
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

static bool tabbar_emit_scene(struct ui_tabbar *tb, const struct tb_theme *t,
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
    tab_rect_at(tb, t, tb->impl.n_tabs, tb->impl.size, tb->impl.drag_tab, &tab);
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
    struct tb_hit hit = hit_at(tb, t, n, size, local);
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
    if (tab_rect_at(tb, t, n, size, hit.index, &tab_r)) {
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
    int h = bar_height(tb, tb_css_height(tb), tb->impl.n_tabs);
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
    int h = bar_height(tb, t->height, tb->impl.n_tabs);
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
        hover(tb, hit_at(tb, t, tb->impl.n_tabs, tb->impl.size, local),
              event->time_ms);
        return over_bar(tb, widget, pe->pos);
    case UI_POINTER_PRESS:
        if (!on_bar)
            return false;
        {
            struct tb_hit hit = hit_at(tb, t, tb->impl.n_tabs, tb->impl.size,
                                       local);
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
    struct tb_hit hit = hit_at(tb, &theme, tb->impl.n_tabs, tb->impl.size,
                               ui_widget_to_local(widget, pos));
    return hit.index >= 0 || hit.on_new;
}

static enum ui_cursor tb_cursor(const struct ui_widget *widget,
                                struct vector2i pos)
{
    const struct ui_tabbar *tb = tb_of(widget);
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    struct tb_hit hit = hit_at(tb, &theme, tb->impl.n_tabs, tb->impl.size,
                               ui_widget_to_local(widget, pos));
    if (hit.on_close || hit.on_new)
        return UI_CURSOR_POINTER;
    return hit.index >= 0 ? UI_CURSOR_DEFAULT : UI_CURSOR_AUTO;
}

static uint64_t tb_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_tabbar *tb = tb_of(widget);
    int css_height = tb_css_height(tb);
    int before = bar_height(tb, css_height, tb->impl.n_tabs);
    bool animating = bar_tick(tb, tb->impl.size, now_ms);
    if (bar_height(tb, css_height, tb->impl.n_tabs) != before)
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
        .y = bar_height(tb, tb_css_height(tb), tb->impl.n_tabs),
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
        tabbar_emit_scene(tb, &theme, tb->impl.labels, tb->impl.n_tabs,
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
    int h = bar_height(tb, theme.height, n);
    if (h <= 0)
        return;
    struct vector2i o = widget->bounds.pos;
    ui_debug_part(out, "bar", -1, rect2i_translate((struct rect2i){
        .size = { .x = size.x, .y = h },
    }, o));
    struct rect2i r;
    for (int i = 0; i < n; i++) {
        if (tab_rect_at(tb, &theme, n, size, i, &r))
            ui_debug_part(out, "tab", i, rect2i_translate(r, o));
        if (close_rect_at(tb, &theme, n, size, i, &r))
            ui_debug_part(out, "tab-close", i, rect2i_translate(r, o));
    }
    struct tb_layout l = tb_layout_for(tb, &theme, n, size);
    if (tb_drop_rect(tb, &l, size, &r))
        ui_debug_part(out, "drop-preview", -1, rect2i_translate(r, o));
    if (new_rect_at(tb, &theme, n, size, &r))
        ui_debug_part(out, "tab-new", -1, rect2i_translate(r, o));
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

static bool same_labels(const struct ui_tabbar *tb,
                        const struct ui_tab_label *tabs, int n)
{
    if (tb->impl.n_tabs != n)
        return false;
    for (int i = 0; i < n; i++) {
        const char *a = tb->impl.labels[i].text;
        const char *b = tabs && tabs[i].text ? tabs[i].text : "";
        if (tb->impl.labels[i].active != (tabs && tabs[i].active) || strcmp(a, b))
            return false;
    }
    return true;
}

static bool reserve_labels(struct ui_tabbar *tb, int n)
{
    size_t have = (size_t)tb->impl.labels_cap;
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
        const char *text = tabs && tabs[i].text ? tabs[i].text : "";
        struct ui_tab_label *l = &tb->impl.labels[i];
        if (!l->text || strcmp(l->text, text)) {
            char *copy = strdup(text);
            if (!copy)
                break;
            free((char *)l->text);
            l->text = copy;
        }
        l->active = tabs && tabs[i].active;
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
    return bar_height(tb, tb_css_height(tb), tb->impl.n_tabs);
}

int ui_tabbar_steady_height(const struct ui_tabbar *tb)
{
    if (!tb)
        return 0;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return steady_height(tb, &theme, tb->impl.n_tabs);
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
    return to_surface(tb, tab_rect_at(tb, &theme, tb->impl.n_tabs,
                                      tb->impl.size, index, out), out);
}

bool ui_tabbar_close_rect(const struct ui_tabbar *tb, int index,
                          struct rect2i *out)
{
    if (!tb)
        return false;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return to_surface(tb, close_rect_at(tb, &theme, tb->impl.n_tabs,
                                        tb->impl.size, index, out), out);
}

bool ui_tabbar_new_tab_rect(const struct ui_tabbar *tb, struct rect2i *out)
{
    if (!tb)
        return false;
    struct tb_theme theme;
    tb_theme_read(tb, &theme);
    return to_surface(tb, new_rect_at(tb, &theme, tb->impl.n_tabs,
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
