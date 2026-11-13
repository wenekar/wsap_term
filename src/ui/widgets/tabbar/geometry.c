#include "ui/widgets/tabbar/internal.h"

#include <math.h>

float tb_open_progress(const struct ui_tabbar *tb, int index)
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

float tb_tab_offset(const struct ui_tabbar *tb, int index)
{
    if (!tb || index < 0 || index >= tb->impl.tab_cap)
        return 0.0f;
    const struct ui_tab_state *t = &tb->impl.tabs[index];
    return t->slide.value;
}

struct tb_layout tb_layout_for(const struct ui_tabbar *tb,
                               const struct tb_theme *t, int n_tabs,
                               struct vector2i size)
{
    if (!tb || n_tabs < 1 || size.x <= 0)
        return (struct tb_layout){0};

    struct tb_layout l = {
        .height = tb_bar_height(tb, t->height, n_tabs),
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

float tb_tab_width_f(const struct ui_tabbar *tb,
                     const struct tb_layout *l, int index)
{
    return l->share * tb_open_progress(tb, index);
}

int tb_tab_width_at(const struct ui_tabbar *tb,
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

struct rect2i tb_close_box(struct rect2i tab_r, int close_w, int close_h, int close_pad)
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

bool tb_drop_rect(const struct ui_tabbar *tb,
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

bool tb_tab_span(const struct ui_tabbar *tb,
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

/* The number of entries that stay after all close animations end. */
int tb_live_tabs(const struct ui_tabbar *tb, int n_tabs)
{
    int live = n_tabs;
    for (int i = 0; tb && i < n_tabs && i < tb->impl.tab_cap; i++)
        if (tb->impl.tabs[i].closing)
            live--;
    return live;
}

int tb_bar_height(const struct ui_tabbar *tb, int css_height, int n_tabs)
{
    if (!tb || n_tabs < 1)
        return 0;

    bool armed = tb->impl.reveal.start_ms != 0;
    if (tb_live_tabs(tb, n_tabs) <= 1 && !armed && !tb->impl.external_drag)
        return 0;
    float v = armed ? tb->impl.reveal.value : 1.0f;
    if (v <= 0.0f)
        return 0;
    if (v > 1.0f)
        v = 1.0f;
    return (int)roundf((float)css_height * v);
}

int tb_steady_height(const struct ui_tabbar *tb, const struct tb_theme *t,
                     int n_tabs)
{
    if (!tb || tb_live_tabs(tb, n_tabs) <= 1)
        return 0;
    return t->height;
}

int tb_drop_height(const struct ui_tabbar *tb, const struct tb_theme *t,
                   int n_tabs)
{
    if (!tb || n_tabs < 1)
        return 0;
    return t->height;
}

bool tb_tab_rect_at(const struct ui_tabbar *tb, const struct tb_theme *t,
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

bool tb_close_rect_at(const struct ui_tabbar *tb, const struct tb_theme *t,
                      int n_tabs, struct vector2i size,
                      int index, struct rect2i *out)
{
    if (!tb || !out || n_tabs < 1 || index < 0 || index >= n_tabs || size.x <= 0)
        return false;
    struct rect2i tab_r;
    if (!tb_tab_rect_at(tb, t, n_tabs, size, index, &tab_r))
        return false;
    int close_side = t->close_side;
    int close_side_h = t->close_side_h;
    int close_pad = t->close_pad;
    if (tab_r.width < close_side + close_pad * 2)
        return false;
    *out = tb_close_box(tab_r, close_side, close_side_h, close_pad);
    return true;
}

bool tb_new_tab_rect_at(const struct ui_tabbar *tb, const struct tb_theme *t,
                        int n_tabs, struct vector2i size, struct rect2i *out)
{
    if (!out || n_tabs < 1 || size.x <= 0)
        return false;
    struct tb_layout l = tb_layout_for(tb, t, n_tabs, size);
    if (l.height <= 0)
        return false;
    return tb_new_tab_rect(tb, &l, n_tabs, size.x, out);
}

struct tb_hit tb_hit_at(const struct ui_tabbar *tb, const struct tb_theme *t,
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
