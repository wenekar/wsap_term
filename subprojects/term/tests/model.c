#include "model.h"
#include "wsap_vtcore/unicode/unicode.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KNOWN_HISTORY_COUNTS_ROWS 0
#define KNOWN_ALT_RESIZE_CUTS_NORMAL 0

#define MAX_TABS 4096
#define STR_KEEP 1024

struct cell {
    uint32_t cp[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    uint8_t n, width;
    uint16_t attrs;
    wsap_vtcore_color fg, bg;
    bool pad;
};

struct row {
    int len;
    bool wrapped;
    struct cell c[];
};

struct pen {
    wsap_vtcore_color fg, bg, ul;
    uint16_t attrs;
    bool inverse;
};

struct saved {
    int x, y;
    bool pending, origin;
    struct pen pen;
    char g[2];
    int gl;
};

struct keyboard {
    unsigned flags, saved[32], depth;
};

enum { GROUND, ESC, CSI, STR, STR_ESC, CHARSET, HASH, DOCS };
enum { OSC, DCS, IGNORE };

struct model {
    char name[64];
    int cols, rows, limit;
    struct row **scr;
    struct row **hist;
    int hist_len, hist_cap, hist_lines;

    bool alternate;
    struct row **normal;
    int normal_cols, normal_rows;
    struct saved normal_cursor, normal_saved;
    bool normal_visible, normal_blink;
    int normal_style;

    int x, y;
    bool pending;
    struct pen pen;
    char g[2];
    int gl;
    struct saved saved;
    bool autowrap, origin, insert, lnm, reverse, app_cursor, app_keypad;
    bool backarrow, bracketed, focus, sync, visible, blink;
    int style, mouse, mouse_enc, decsace;
    int top, bottom;
    bool tabs[MAX_TABS];
    struct keyboard keyboard[2];
    uint32_t last[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int last_n;
    int prompt;
    unsigned flags;

    int state, str_kind;
    char slot;
    int param[16], nparam, csi_phase;
    uint16_t sub;
    char prefix, inter;
    char str[STR_KEEP];
    size_t str_len;

    int need;
    uint32_t acc;
    uint8_t lo, hi;

    char *reply;
    size_t reply_len, reply_cap;
    struct model_cell **out;
    int out_cap, out_cols;
};

static struct cell blank(wsap_vtcore_color fg, wsap_vtcore_color bg)
{
    return (struct cell){ .cp = { ' ' }, .n = 1, .width = 1, .fg = fg, .bg = bg };
}

static struct cell default_blank(void)
{
    return blank(WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
}

static struct row *row_new(int cols)
{
    struct row *r = malloc(sizeof(*r) + (size_t)cols * sizeof(struct cell));
    if (!r) abort();
    r->len = 0;
    r->wrapped = false;
    for (int x = 0; x < cols; x++) r->c[x] = default_blank();
    return r;
}

static void row_cut(struct row *r, int n, int cols)
{
    if (n >= r->len) return;
    for (int x = n; x < r->len && x < cols; x++) r->c[x] = default_blank();
    r->len = n;
}

static bool pristine(const struct model *m)
{
    return m->pen.fg == WSAP_VTCORE_COLOR_FG && m->pen.bg == WSAP_VTCORE_COLOR_BG;
}

static bool is_ri(uint32_t cp)
{
    return cp - 0x1F1E6u <= 0x19u && wsap_vtcore_grapheme_regional_indicator(cp);
}

static void __attribute__((format(printf, 2, 3))) reply(struct model *m, const char *fmt, ...)
{
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (m->reply_len + (size_t)n + 1 > m->reply_cap) {
        m->reply_cap = (m->reply_len + (size_t)n + 1) * 2;
        m->reply = realloc(m->reply, m->reply_cap);
        if (!m->reply) abort();
    }
    memcpy(m->reply + m->reply_len, tmp, (size_t)n);
    m->reply_len += (size_t)n;
}

static int history_limit(int requested)
{
    return requested == 0 ? WSAP_VTCORE_SCROLLBACK_DEFAULT : requested < 0 ? 0 : requested;
}

static void hist_reflow_tail(struct model *m);

static void hist_push(struct model *m, struct row *r)
{
    if (!m->limit) {
        free(r);
        return;
    }
    if (KNOWN_HISTORY_COUNTS_ROWS) {
        if (m->hist_len == m->limit) {
            free(m->hist[0]);
            memmove(m->hist, m->hist + 1, (size_t)(m->hist_len - 1) * sizeof(*m->hist));
            m->hist_len--;
        }
    } else {
        bool new_line = (m->hist_len == 0 || !m->hist[m->hist_len - 1]->wrapped);
        if (new_line) {
            if (m->hist_lines == m->limit) {
                int evict_rows = 1;
                while (evict_rows < m->hist_len && m->hist[evict_rows - 1]->wrapped)
                    evict_rows++;
                for (int i = 0; i < evict_rows; i++)
                    free(m->hist[i]);
                memmove(m->hist, m->hist + evict_rows,
                        (size_t)(m->hist_len - evict_rows) * sizeof(*m->hist));
                m->hist_len -= evict_rows;
                m->hist_lines--;
            }
            m->hist_lines++;
        }
    }
    if (m->hist_len == m->hist_cap) {
        m->hist_cap = m->hist_cap ? m->hist_cap * 2 : 64;
        m->hist = realloc(m->hist, (size_t)m->hist_cap * sizeof(*m->hist));
        if (!m->hist) abort();
    }
    m->hist[m->hist_len++] = r;
    if (!KNOWN_HISTORY_COUNTS_ROWS)
        hist_reflow_tail(m);
}

static void wsap_vtcore_hist_clear(struct model *m)
{
    for (int i = 0; i < m->hist_len; i++) free(m->hist[i]);
    m->hist_len = 0;
    m->hist_lines = 0;
}

static void erase_row(struct model *m, int y);

static void scroll_up_full(struct model *m)
{
    struct row *top = m->scr[0];
    memmove(m->scr, m->scr + 1, (size_t)(m->rows - 1) * sizeof(*m->scr));
    m->scr[m->rows - 1] = row_new(m->cols);
    erase_row(m, m->rows - 1);
    if (m->alternate) {
        free(top);
        return;
    }
    if (m->prompt > 0) m->prompt--;
    hist_push(m, top);
}

static void scroll_region(struct model *m, int top, int bottom, int n, bool down, bool history)
{
    int height = bottom - top + 1;
    if (n > height) n = height;
    if (!down && history && top == 0 && bottom == m->rows - 1) {
        for (int i = 0; i < n; i++) scroll_up_full(m);
        return;
    }
    if (!down && history && top == 0 && !m->alternate) {
        for (int i = 0; i < n; i++) {
            scroll_up_full(m);
            scroll_region(m, bottom, m->rows - 1, 1, true, false);
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        if (down) {
            free(m->scr[bottom]);
            memmove(m->scr + top + 1, m->scr + top, (size_t)(height - 1) * sizeof(*m->scr));
            m->scr[top] = row_new(m->cols);
            erase_row(m, top);
        } else {
            free(m->scr[top]);
            memmove(m->scr + top, m->scr + top + 1, (size_t)(height - 1) * sizeof(*m->scr));
            m->scr[bottom] = row_new(m->cols);
            erase_row(m, bottom);
        }
    }
    if (!m->alternate && m->prompt >= top && m->prompt <= bottom) {
        m->prompt += down ? n : -n;
        if (m->prompt < top || m->prompt > bottom) m->prompt = -1;
    }
}

static void line_feed(struct model *m)
{
    if (m->y == m->bottom) {
        scroll_region(m, m->top, m->bottom, 1, false, true);
    } else if (m->y < m->rows - 1) {
        m->y++;
    }
}

static void reverse_index(struct model *m)
{
    if (m->y == m->top)
        scroll_region(m, m->top, m->bottom, 1, true, false);
    else if (m->y > 0)
        m->y--;
}

static void erase_cells(struct model *m, int y, int from, int to)
{
    struct row *r = m->scr[y];
    if (to > m->cols) to = m->cols;
    bool plain = pristine(m);
    if (plain && to > r->len) to = r->len;
    if (from >= to) return;
    if (from > 0 && r->c[from].width == 0) from--;
    if (to < r->len && r->c[to].width == 0) to++;
    if (to >= m->cols) r->wrapped = false;
    if (!plain) {
        for (int x = from; x < to; x++) r->c[x] = blank(m->pen.fg, m->pen.bg);
        if (to > r->len) r->len = to;
        return;
    }
    if (to < r->len) {
        for (int x = from; x < to; x++) r->c[x] = default_blank();
        return;
    }
    row_cut(r, from, m->cols);
    if (from == 0) r->wrapped = false;
}

static void erase_row(struct model *m, int y)
{
    erase_cells(m, y, 0, m->cols);
}

static int last_text_row(const struct model *m)
{
    for (int y = m->rows - 1; y >= 0; y--)
        for (int x = 0; x < m->cols; x++) {
            const struct cell *c = &m->scr[y]->c[x];
            if (c->n > 1 || (c->n == 1 && c->cp[0] > ' ')) return y;
        }
    return -1;
}

static void erase_display(struct model *m, int mode)
{
    if (mode == 3) {
        wsap_vtcore_hist_clear(m);
        return;
    }
    if (mode == 2) {
        int last = m->alternate ? -1 : last_text_row(m);
        for (int i = 0; i <= last; i++) scroll_up_full(m);
        for (int y = 0; y < m->rows; y++) erase_row(m, y);
    } else if (mode == 0) {
        erase_cells(m, m->y, m->x, m->cols);
        for (int y = m->y + 1; y < m->rows; y++) erase_row(m, y);
    } else if (mode == 1) {
        for (int y = 0; y < m->y; y++) erase_row(m, y);
        erase_cells(m, m->y, 0, m->x + 1);
    }
}

static void erase_line(struct model *m, int mode)
{
    if (mode == 0) erase_cells(m, m->y, m->x, m->cols);
    else if (mode == 1) erase_cells(m, m->y, 0, m->x + 1);
    else if (mode == 2) erase_cells(m, m->y, 0, m->cols);
}

static void split_edges(struct model *m, struct row *r, int from, int to)
{
    if (from >= to) return;
    if (from > 0 && from < r->len && r->c[from].width == 0)
        r->c[from - 1] = blank(r->c[from - 1].fg, r->c[from - 1].bg);
    if (to < r->len && r->c[to].width == 0)
        r->c[to] = blank(r->c[to].fg, r->c[to].bg);
    (void)m;
}

static void repair_row(struct model *m, struct row *r, int from, int to)
{
    if (from < 0) from = 0;
    if (to > m->cols) to = m->cols;
    for (int x = from; x < to; x++) {
        struct cell *c = &r->c[x];
        if (c->width == 0 && (x == 0 || r->c[x - 1].width != 2))
            *c = blank(c->fg, c->bg);
        if (c->width == 2 && m->cols > 1 && (x + 1 == m->cols || r->c[x + 1].width != 0))
            *c = blank(c->fg, c->bg);
    }
}

static void fill_gap(struct row *r, int x)
{
    if (x > r->len) r->len = x;
}

static uint16_t presentation(const uint32_t *cps, int n)
{
    bool pictographic = wsap_vtcore_unicode_extended_pictographic(cps[0]);
    bool emoji = false;
    for (int i = 1; i < n; i++) {
        if (cps[i] == 0xFE0F) return WSAP_VTCORE_ATTR_EMOJI;
        if (cps[i] == 0xFE0E) return WSAP_VTCORE_ATTR_TEXT;
        if (cps[i] == 0x20E3 || wsap_vtcore_grapheme_regional_indicator(cps[i]) ||
            (pictographic && (cps[i] == 0x200D || wsap_vtcore_grapheme_emoji_modifier(cps[i]) ||
                              (cps[i] >= 0xE0020 && cps[i] <= 0xE007F))))
            emoji = true;
    }
    return emoji ? WSAP_VTCORE_ATTR_EMOJI : 0;
}

static void wrap_to_next_row(struct model *m)
{
    m->scr[m->y]->wrapped = true;
    m->x = 0;
    line_feed(m);
    m->pending = false;
}

static void put_glyph(struct model *m, const uint32_t *cps, int n, uint16_t attrs)
{
    if (n > 1)
        attrs = (uint16_t)((attrs & ~(WSAP_VTCORE_ATTR_EMOJI | WSAP_VTCORE_ATTR_TEXT)) | presentation(cps, n));
    int width = wsap_vtcore_unicode_width(cps[0]) == 2 ? 2 : 1;
    if (attrs & WSAP_VTCORE_ATTR_EMOJI) width = 2;
    if (attrs & WSAP_VTCORE_ATTR_TEXT) width = 1;
    if (m->pending) wrap_to_next_row(m);
    int right = m->cols - 1;
    struct row *r = m->scr[m->y];
    if (width == 2 && m->cols > 1 && m->x == right && m->autowrap) {
        fill_gap(r, m->x);
        split_edges(m, r, m->x, right + 1);
        r->c[m->x] = default_blank();
        r->c[m->x].pad = true;
        r->len = m->cols;
        wrap_to_next_row(m);
        r = m->scr[m->y];
    }
    int x = m->x;
    int span = width > right + 1 - x ? 1 : width;
    fill_gap(r, x);
    if (m->insert) {
        int move = right + 1 - x - span;
        if (move > 0) memmove(r->c + x + span, r->c + x, (size_t)move * sizeof(struct cell));
        r->len = r->len + span > m->cols ? m->cols : r->len + span;
        repair_row(m, r, right, right + 1);
    }
    split_edges(m, r, x, x + span);
    struct cell c = {
        .n = (uint8_t)n, .width = (uint8_t)span, .attrs = attrs,
        .fg = m->pen.inverse ? m->pen.bg : m->pen.fg,
        .bg = m->pen.inverse ? m->pen.fg : m->pen.bg,
    };
    memcpy(c.cp, cps, (size_t)n * sizeof(*cps));
    r->c[x] = c;
    if (span == 2) {
        r->c[x + 1] = c;
        r->c[x + 1].n = 0;
        r->c[x + 1].width = 0;
        memset(r->c[x + 1].cp, 0, sizeof(c.cp));
    }
    if (x + span > r->len) r->len = x + span;
    if (x + span <= right) {
        m->x = x + span;
    } else {
        m->x = right;
        m->pending = m->autowrap;
    }
}

static bool joins(uint32_t cp, int width)
{
    if (cp < 0x300) return false;
    return width == 0 || wsap_vtcore_grapheme_emoji_modifier(cp) ||
           (width == 1 && wsap_vtcore_unicode_spacing_mark(cp));
}

static bool cluster_joins(const uint32_t *cps, int n, uint32_t cp, int width)
{
    if (joins(cp, width)) return true;
    if (cps[n - 1] == 0x200D) return wsap_vtcore_unicode_extended_pictographic(cp);
    return n == 1 && is_ri(cp) && is_ri(cps[0]);
}

static bool extend_cluster(struct model *m, uint32_t cp, int width)
{
    bool after_join = m->last_n > 1 || (m->last_n == 1 && m->last[0] == 0x200D);
    if (!(width == 0 || after_join || is_ri(cp) || joins(cp, width)))
        return false;
    struct row *r = m->scr[m->y];
    int x = m->pending ? m->cols - 1 : m->x - 1;
    struct cell *prev = NULL;
    if (x >= 0 && x < r->len) {
        if (x > 0 && r->c[x].width == 0) x--;
        prev = &r->c[x];
    }
    if (!prev || !(prev->n > 1 || (prev->n == 1 && prev->cp[0] > ' ')))
        return joins(cp, width);
    if (!cluster_joins(prev->cp, prev->n, cp, width))
        return false;
    if (prev->n >= WSAP_VTCORE_GRAPHEME_MAX_LEN)
        return true;
    struct cell old = *prev;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    memcpy(cps, old.cp, (size_t)old.n * sizeof(*cps));
    cps[old.n] = cp;
    int n = old.n + 1;
    int span = old.width == 2 && x + 1 < m->cols ? 2 : 1;
    for (int i = 0; i < span; i++) r->c[x + i] = blank(old.fg, old.bg);
    if (r->len == x + span) row_cut(r, x, m->cols);
    m->x = x;
    m->pending = false;
    memcpy(m->last, cps, (size_t)n * sizeof(*cps));
    m->last_n = n;
    put_glyph(m, cps, n, (uint16_t)(old.attrs & ~(WSAP_VTCORE_ATTR_EMOJI | WSAP_VTCORE_ATTR_TEXT)));
    r = m->scr[m->y];
    x = m->pending ? m->cols - 1 : m->x - 1;
    if (x > 0 && r->c[x].width == 0) x--;
    r->c[x].fg = old.fg;
    r->c[x].bg = old.bg;
    if (r->c[x].width == 2 && x + 1 < m->cols) {
        r->c[x + 1].fg = old.fg;
        r->c[x + 1].bg = old.bg;
    }
    return true;
}

static void esc_dispatch(struct model *m, unsigned char ch);

static void emit(struct model *m, uint32_t cp)
{
    static const uint32_t dec_graphics[] = {
        0x00a0,
        0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
        0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
        0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
        0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
    };
    if (cp >= 0x80 && cp <= 0x9f) {
        static const char c1[32] = {
            [4] = 'D', [5] = 'E', [8] = 'H', [13] = 'M',
            [16] = 'P', [27] = '[', [28] = '\\', [29] = ']',
        };
        if (c1[cp - 0x80]) {
            m->state = GROUND;
            esc_dispatch(m, (unsigned char)c1[cp - 0x80]);
        }
        return;
    }
    if (m->g[m->gl] == '0' && cp >= '_' && cp <= '~')
        cp = dec_graphics[cp - '_'];
    m->last[0] = cp;
    m->last_n = 1;
    put_glyph(m, &cp, 1, m->pen.attrs);
}

static void print_cp(struct model *m, uint32_t cp)
{
    if (cp < 0x80) {
        emit(m, cp);
        return;
    }
    int width = wsap_vtcore_unicode_width(cp);
    if (!extend_cluster(m, cp, width))
        emit(m, cp);
}

static int top_limit(const struct model *m) { return m->origin ? m->top : 0; }
static int bottom_limit(const struct model *m) { return m->origin ? m->bottom : m->rows - 1; }

static void move_to(struct model *m, int x, int y)
{
    m->pending = false;
    m->x = x < 0 ? 0 : x >= m->cols ? m->cols - 1 : x;
    m->y = y < 0 ? 0 : y >= m->rows ? m->rows - 1 : y;
}

static void move_v(struct model *m, int n, bool col0)
{
    int lo = 0, hi = m->rows - 1;
    if (m->origin || (m->y >= m->top && m->y <= m->bottom)) {
        lo = m->top;
        hi = m->bottom;
    } else if (m->y < m->top) {
        hi = m->bottom;
    } else {
        lo = m->top;
    }
    long y = (long)m->y + n;
    if (n < 0 && m->y >= lo && y < lo) y = lo;
    if (n > 0 && m->y <= hi && y > hi) y = hi;
    move_to(m, col0 ? 0 : m->x, (int)(y < 0 ? 0 : y > m->rows - 1 ? m->rows - 1 : y));
}

static void move_h(struct model *m, int n)
{
    long x = (long)m->x + n;
    move_to(m, (int)(x < 0 ? 0 : x > m->cols - 1 ? m->cols - 1 : x), m->y);
}

static struct saved save_cursor(const struct model *m)
{
    return (struct saved){
        .x = m->x, .y = m->y, .pending = m->pending, .origin = m->origin,
        .pen = m->pen, .g = { m->g[0], m->g[1] }, .gl = m->gl,
    };
}

static void restore_cursor(struct model *m, const struct saved *s)
{
    m->pen = s->pen;
    m->origin = s->origin;
    m->g[0] = s->g[0];
    m->g[1] = s->g[1];
    m->gl = s->gl;
    move_to(m, s->x, s->y);
    m->pending = s->pending;
}

static struct saved initial_saved(void)
{
    return (struct saved){
        .pen = { .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG },
        .g = { 'B', 'B' },
    };
}

static void tabs_default(struct model *m)
{
    memset(m->tabs, 0, sizeof(m->tabs));
    for (int c = 8; c < MAX_TABS; c += 8) m->tabs[c] = true;
}

static void delete_chars(struct model *m, int n)
{
    struct row *r = m->scr[m->y];
    int x = m->x, cols = m->cols;
    if (n > cols - x) n = cols - x;
    int len = r->len;
    split_edges(m, r, x, x + n);
    if (len > x + n) {
        memmove(r->c + x, r->c + x + n, (size_t)(len - x - n) * sizeof(struct cell));
        for (int i = len - n; i < len; i++) r->c[i] = default_blank();
        r->len = len - n;
    } else if (len > x) {
        row_cut(r, x, cols);
    }
    if (!pristine(m)) {
        int from = cols - n < x ? x : cols - n;
        fill_gap(r, from);
        for (int i = from; i < cols; i++) r->c[i] = blank(m->pen.fg, m->pen.bg);
        r->len = cols;
    }
    r->wrapped = false;
    repair_row(m, r, x - 1, len);
}

static void insert_chars(struct model *m, int n)
{
    struct row *r = m->scr[m->y];
    int x = m->x, cols = m->cols;
    if (n > cols - x) n = cols - x;
    fill_gap(r, x);
    int len = r->len;
    if (x > 0 && r->c[x].width == 0) {
        r->c[x - 1] = blank(m->pen.fg, m->pen.bg);
        r->c[x] = blank(m->pen.fg, m->pen.bg);
    }
    if (len > x) {
        int move = len - x;
        if (move > cols - x - n) move = cols - x - n;
        if (move > 0) memmove(r->c + x + n, r->c + x, (size_t)move * sizeof(struct cell));
    }
    for (int i = x; i < x + n; i++) r->c[i] = blank(m->pen.fg, m->pen.bg);
    int new_len = len + n > cols ? cols : len + n;
    if (pristine(m) && len <= x) new_len = len;
    else if (new_len < x + n) new_len = x + n;
    for (int i = new_len; i < cols; i++) r->c[i] = default_blank();
    r->len = new_len;
    r->wrapped = false;
    int end = x + n + 1 > len + 1 ? x + n + 1 : len + 1;
    repair_row(m, r, x - 1, end);
}

static void pen_reset(struct pen *p)
{
    *p = (struct pen){ .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG };
}

static void set_underline(uint16_t *attrs, int style)
{
    static const uint16_t flags[] = {
        0, WSAP_VTCORE_ATTR_UNDERLINE, WSAP_VTCORE_ATTR_DOUBLE_UNDERLINE,
        WSAP_VTCORE_ATTR_CURLY_UNDERLINE, WSAP_VTCORE_ATTR_DOTTED_UNDERLINE, WSAP_VTCORE_ATTR_DASHED_UNDERLINE,
    };
    if ((unsigned)style > 5) style = 1;
    *attrs = (uint16_t)((*attrs & ~WSAP_VTCORE_ATTR_UNDERLINE_MASK) | flags[style]);
}

static int extended_color(const struct model *m, int i, wsap_vtcore_color *color, bool *ok)
{
    *ok = false;
    if (i + 1 >= m->nparam) return 0;
    if (i + 2 < m->nparam && m->param[i + 1] == 5) {
        int index = m->param[i + 2];
        if (index >= 0 && index <= 255) {
            *color = wsap_vtcore_color_index(index);
            *ok = true;
        }
        return 2;
    }
    if (m->param[i + 1] != 2) return 0;
    int colon = 0;
    while (i + 2 + colon < m->nparam && i + 2 + colon < 16 &&
           (m->sub & (1u << (i + 2 + colon))))
        colon++;
    int skip = colon >= 4 ? 1 : 0;
    if (i + 4 + skip >= m->nparam) return 0;
    *color = wsap_vtcore_color_rgb((uint8_t)m->param[i + 2 + skip], (uint8_t)m->param[i + 3 + skip],
                            (uint8_t)m->param[i + 4 + skip]);
    *ok = true;
    return 4 + skip;
}

static void sgr(struct model *m)
{
    struct pen *p = &m->pen;
    if (m->nparam == 0) {
        pen_reset(p);
        return;
    }
    for (int i = 0; i < m->nparam; i++) {
        int v = m->param[i];
        wsap_vtcore_color color;
        bool ok;
        switch (v) {
        case 0: pen_reset(p); break;
        case 1: p->attrs |= WSAP_VTCORE_ATTR_BOLD; break;
        case 2: p->attrs |= WSAP_VTCORE_ATTR_FAINT; break;
        case 3: p->attrs |= WSAP_VTCORE_ATTR_ITALIC; break;
        case 4: {
            int style = 1;
            if (i + 1 < m->nparam && (m->sub & (1u << (i + 1))))
                style = m->param[++i];
            set_underline(&p->attrs, style);
            break;
        }
        case 7: p->inverse = true; break;
        case 9: p->attrs |= WSAP_VTCORE_ATTR_STRIKE; break;
        case 21: set_underline(&p->attrs, 2); break;
        case 22: p->attrs &= (uint16_t)~(WSAP_VTCORE_ATTR_BOLD | WSAP_VTCORE_ATTR_FAINT); break;
        case 23: p->attrs &= (uint16_t)~WSAP_VTCORE_ATTR_ITALIC; break;
        case 24: set_underline(&p->attrs, 0); break;
        case 27: p->inverse = false; break;
        case 29: p->attrs &= (uint16_t)~WSAP_VTCORE_ATTR_STRIKE; break;
        case 38: i += extended_color(m, i, &color, &ok); if (ok) p->fg = color; break;
        case 39: p->fg = WSAP_VTCORE_COLOR_FG; break;
        case 48: i += extended_color(m, i, &color, &ok); if (ok) p->bg = color; break;
        case 49: p->bg = WSAP_VTCORE_COLOR_BG; break;
        case 53: p->attrs |= WSAP_VTCORE_ATTR_OVERLINE; break;
        case 55: p->attrs &= (uint16_t)~WSAP_VTCORE_ATTR_OVERLINE; break;
        case 58: i += extended_color(m, i, &color, &ok); if (ok) p->ul = color; break;
        case 59: p->ul = 0; break;
        default:
            if (v >= 30 && v <= 37) p->fg = wsap_vtcore_color_index(v - 30);
            else if (v >= 40 && v <= 47) p->bg = wsap_vtcore_color_index(v - 40);
            else if (v >= 90 && v <= 97) p->fg = wsap_vtcore_color_index(v - 90 + 8);
            else if (v >= 100 && v <= 107) p->bg = wsap_vtcore_color_index(v - 100 + 8);
            break;
        }
    }
}

static void sgr_report(const struct model *m, char *out, size_t cap)
{
    const struct pen *p = &m->pen;
    size_t n = 0;
    out[0] = 0;
#define ADD(...) do { int k = snprintf(out + n, cap - n, __VA_ARGS__); if (k > 0) n += (size_t)k; } while (0)
    int ul = wsap_vtcore_attr_underline(p->attrs);
    if (!(p->attrs & (WSAP_VTCORE_ATTR_BOLD | WSAP_VTCORE_ATTR_FAINT | WSAP_VTCORE_ATTR_ITALIC | WSAP_VTCORE_ATTR_STRIKE |
                      WSAP_VTCORE_ATTR_OVERLINE | WSAP_VTCORE_ATTR_UNDERLINE_MASK)) &&
        !p->inverse && p->fg == WSAP_VTCORE_COLOR_FG && p->bg == WSAP_VTCORE_COLOR_BG && !p->ul)
        return;
    ADD("0");
    if (p->attrs & WSAP_VTCORE_ATTR_BOLD) ADD(";1");
    if (p->attrs & WSAP_VTCORE_ATTR_FAINT) ADD(";2");
    if (p->attrs & WSAP_VTCORE_ATTR_ITALIC) ADD(";3");
    if (ul == 1) ADD(";4");
    else if (ul) ADD(";4:%d", ul);
    if (p->inverse) ADD(";7");
    if (p->attrs & WSAP_VTCORE_ATTR_STRIKE) ADD(";9");
    if (p->attrs & WSAP_VTCORE_ATTR_OVERLINE) ADD(";53");
    const wsap_vtcore_color colors[3] = { p->fg, p->bg, p->ul };
    const bool set[3] = { p->fg != WSAP_VTCORE_COLOR_FG, p->bg != WSAP_VTCORE_COLOR_BG, p->ul != 0 };
    for (int k = 0; k < 3; k++) {
        if (!set[k]) continue;
        int base = 30 + 10 * k;
        wsap_vtcore_color c = colors[k];
        if (wsap_vtcore_color_is_slot(c) && (c & 0x1FFu) < 256) {
            int i = (int)(c & 0xFFu);
            if (i < 16 && k < 2) ADD(";%d", i < 8 ? base + i : base + 60 + i - 8);
            else ADD(";%d:5:%d", base + 8, i);
        } else {
            ADD(";%d:2:%d:%d:%d", base + 8, (int)((c >> 16) & 0xff),
                (int)((c >> 8) & 0xff), (int)(c & 0xff));
        }
    }
#undef ADD
}

static void cara_cell(struct model *m, struct cell *c)
{
    if (m->nparam <= 4) {
        c->fg = WSAP_VTCORE_COLOR_FG;
        c->bg = WSAP_VTCORE_COLOR_BG;
        c->attrs &= WSAP_VTCORE_ATTR_EMOJI | WSAP_VTCORE_ATTR_TEXT | WSAP_VTCORE_ATTR_LINK;
        return;
    }
    for (int i = 4; i < m->nparam; i++) {
        int v = m->param[i];
        wsap_vtcore_color color;
        bool ok;
        switch (v) {
        case 0:
            c->fg = WSAP_VTCORE_COLOR_FG;
            c->bg = WSAP_VTCORE_COLOR_BG;
            c->attrs &= WSAP_VTCORE_ATTR_EMOJI | WSAP_VTCORE_ATTR_TEXT | WSAP_VTCORE_ATTR_LINK;
            break;
        case 1: c->attrs |= WSAP_VTCORE_ATTR_BOLD; break;
        case 2: c->attrs |= WSAP_VTCORE_ATTR_FAINT; break;
        case 3: c->attrs |= WSAP_VTCORE_ATTR_ITALIC; break;
        case 4: {
            int style = 1;
            if (i + 1 < m->nparam && (m->sub & (1u << (i + 1))))
                style = m->param[++i];
            set_underline(&c->attrs, style);
            break;
        }
        case 7: { wsap_vtcore_color t = c->fg; c->fg = c->bg; c->bg = t; break; }
        case 9: c->attrs |= WSAP_VTCORE_ATTR_STRIKE; break;
        case 21: set_underline(&c->attrs, 2); break;
        case 22: c->attrs &= (uint16_t)~(WSAP_VTCORE_ATTR_BOLD | WSAP_VTCORE_ATTR_FAINT); break;
        case 23: c->attrs &= (uint16_t)~WSAP_VTCORE_ATTR_ITALIC; break;
        case 24: set_underline(&c->attrs, 0); break;
        case 29: c->attrs &= (uint16_t)~WSAP_VTCORE_ATTR_STRIKE; break;
        case 38: i += extended_color(m, i, &color, &ok); if (ok) c->fg = color; break;
        case 39: c->fg = WSAP_VTCORE_COLOR_FG; break;
        case 48: i += extended_color(m, i, &color, &ok); if (ok) c->bg = color; break;
        case 49: c->bg = WSAP_VTCORE_COLOR_BG; break;
        case 53: c->attrs |= WSAP_VTCORE_ATTR_OVERLINE; break;
        case 55: c->attrs &= (uint16_t)~WSAP_VTCORE_ATTR_OVERLINE; break;
        case 58: i += extended_color(m, i, &color, &ok); break;
        default:
            if (v >= 30 && v <= 37) c->fg = wsap_vtcore_color_index(v - 30);
            else if (v >= 40 && v <= 47) c->bg = wsap_vtcore_color_index(v - 40);
            else if (v >= 90 && v <= 97) c->fg = wsap_vtcore_color_index(v - 90 + 8);
            else if (v >= 100 && v <= 107) c->bg = wsap_vtcore_color_index(v - 100 + 8);
            break;
        }
    }
}

static void cara_row(struct model *m, int y, int x0, int x1)
{
    struct row *r = m->scr[y];
    if (x0 > 0 && r->c[x0].width == 0) x0--;
    if (x1 + 1 < m->cols && r->c[x1].width == 2) x1++;
    fill_gap(r, x1 + 1);
    for (int x = x0; x <= x1; x++) cara_cell(m, &r->c[x]);
}

static void deccara(struct model *m)
{
    int top = m->param[0] > 0 ? m->param[0] - 1 : 0;
    int left = m->nparam > 1 && m->param[1] > 0 ? m->param[1] - 1 : 0;
    int bottom = m->nparam > 2 && m->param[2] > 0 ? m->param[2] - 1 : m->rows - 1;
    int right = m->nparam > 3 && m->param[3] > 0 ? m->param[3] - 1 : m->cols - 1;
    if (top > bottom || left > right || top >= m->rows || left >= m->cols) return;
    if (bottom >= m->rows) bottom = m->rows - 1;
    if (right >= m->cols) right = m->cols - 1;
    if (m->decsace == 2) {
        for (int y = top; y <= bottom; y++) cara_row(m, y, left, right);
        return;
    }
    cara_row(m, top, left, top == bottom ? right : m->cols - 1);
    for (int y = top + 1; y < bottom; y++) cara_row(m, y, 0, m->cols - 1);
    if (bottom > top) cara_row(m, bottom, 0, right);
}

static void free_rows(struct row **rows, int n)
{
    if (!rows) return;
    for (int i = 0; i < n; i++) free(rows[i]);
    free(rows);
}

static struct row **rows_new(int n, int cols)
{
    struct row **rows = calloc((size_t)n, sizeof(*rows));
    if (!rows) abort();
    for (int i = 0; i < n; i++) rows[i] = row_new(cols);
    return rows;
}

static struct row *row_resized(const struct row *r, int old_cols, int cols)
{
    struct row *n = row_new(cols);
    int keep = old_cols < cols ? old_cols : cols;
    memcpy(n->c, r->c, (size_t)keep * sizeof(struct cell));
    n->len = r->len < cols ? r->len : cols;
    n->wrapped = r->wrapped;
    return n;
}

struct reflow {
    int cols, rows;
    struct row **out;
    int count, cap;
    struct row *cur;
    int x;
    bool cursor_set, saved_set, prompt_set;
    int cursor_x, cursor_y, saved_x, saved_y, prompt;
    bool pending;
};

static void reflow_end_row(struct reflow *r, bool wrapped)
{
    if (!r->cur) r->cur = row_new(r->cols);
    r->cur->len = r->x > r->cols ? r->cols : r->x;
    r->cur->wrapped = wrapped;
    if (r->count == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->out = realloc(r->out, (size_t)r->cap * sizeof(*r->out));
        if (!r->out) abort();
    }
    r->out[r->count++] = r->cur;
    r->cur = NULL;
    r->x = 0;
}

static struct cell *reflow_dst(struct reflow *r)
{
    if (!r->cur) r->cur = row_new(r->cols);
    return r->cur->c;
}

static void reflow_append(struct reflow *r, const struct cell *cells, int len,
                          int cursor, int saved)
{
    for (int i = 0; i < len;) {
        const struct cell *c = &cells[i];
        if (c->pad || c->width == 0) {
            int at = c->pad ? r->x : r->x - 1;
            if (cursor == i) {
                r->cursor_x = at < 0 ? 0 : at;
                r->cursor_y = r->count;
                r->cursor_set = true;
                r->pending = false;
            }
            if (saved == i) {
                r->saved_x = at < 0 ? 0 : at;
                r->saved_y = r->count;
                r->saved_set = true;
            }
            i++;
            continue;
        }
        int n = c->width == 2 && r->cols > 1 ? 2 : 1;
        if (r->x + n > r->cols) {
            if (r->x < r->cols) {
                reflow_dst(r)[r->x] = default_blank();
                reflow_dst(r)[r->x].pad = true;
                r->x++;
            }
            reflow_end_row(r, true);
        }
        if (cursor == i) {
            r->cursor_x = r->x;
            r->cursor_y = r->count;
            r->cursor_set = true;
            r->pending = false;
        }
        if (saved == i) {
            r->saved_x = r->x;
            r->saved_y = r->count;
            r->saved_set = true;
        }
        struct cell *dst = reflow_dst(r);
        dst[r->x] = *c;
        if (n == 2) {
            dst[r->x + 1] = *c;
            dst[r->x + 1].n = 0;
            dst[r->x + 1].width = 0;
            memset(dst[r->x + 1].cp, 0, sizeof(c->cp));
        }
        r->x += n;
        i++;
    }
    if (cursor == len) {
        r->cursor_y = r->count;
        r->cursor_x = r->x == r->cols ? r->cols - 1 : r->x;
        r->pending = r->x == r->cols;
        r->cursor_set = true;
    }
    if (saved == len) {
        r->saved_y = r->count;
        r->saved_x = r->x == r->cols ? r->cols - 1 : r->x;
        r->saved_set = true;
    }
}

static void hist_reflow_tail(struct model *m)
{
    if (m->hist_len <= 0)
        return;
    int start = m->hist_len - 1;
    while (start > 0 && m->hist[start - 1]->wrapped)
        start--;

    bool line_open = m->hist[m->hist_len - 1]->wrapped;
    struct reflow rf = { .cols = m->cols, .rows = m->rows };
    for (int i = start; i < m->hist_len; i++) {
        const struct row *src = m->hist[i];
        int len = src->wrapped && src->len < m->cols ? m->cols : src->len;
        reflow_append(&rf, src->c, len, -1, -1);
    }
    if (rf.x || rf.count == 0)
        reflow_end_row(&rf, false);
    free(rf.cur);

    if (line_open) {
        struct row *last = rf.out[rf.count - 1];
        last->wrapped = true;
        if (last->len < m->cols) {
            for (int x = last->len; x < m->cols; x++) {
                last->c[x] = default_blank();
                last->c[x].pad = true;
            }
            last->len = m->cols;
        }
    }

    for (int i = start; i < m->hist_len; i++)
        free(m->hist[i]);
    m->hist_len = start;
    while (m->hist_len + rf.count > m->hist_cap) {
        m->hist_cap = m->hist_cap ? m->hist_cap * 2 : 64;
        m->hist = realloc(m->hist, (size_t)m->hist_cap * sizeof(*m->hist));
        if (!m->hist) abort();
    }
    for (int i = 0; i < rf.count; i++)
        m->hist[m->hist_len++] = rf.out[i];
    free(rf.out);
}

static void reflow(struct model *m, int cols, int rows)
{
    int old_cols = m->cols, old_rows = m->rows;
    int active = old_rows;
    while (active > m->y + 1 && active > m->saved.y + 1 &&
           !m->scr[active - 1]->len && !m->scr[active - 1]->wrapped)
        active--;
    struct reflow r = { .cols = cols, .rows = rows };
    int total_in = m->hist_len + active;
    struct cell *line = malloc(((size_t)old_cols + (size_t)m->saved.x + 2) * sizeof(*line));
    if (!line) abort();
    for (int i = 0; i < total_in; i++) {
        int y = i - m->hist_len;
        const struct row *src = i < m->hist_len ? m->hist[i] : m->scr[y];
        int len = src->len;
        bool wrapped = src->wrapped;
        if (m->prompt >= 0 && y >= m->prompt) {
            if (r.x) reflow_end_row(&r, false);
            if (y == m->prompt) {
                r.prompt = r.count;
                r.prompt_set = true;
            }
            if (y == m->y) {
                r.cursor_y = r.count;
                r.cursor_x = m->x < cols ? m->x : cols - 1;
                r.pending = false;
                r.cursor_set = true;
            }
            if (y == m->saved.y) {
                r.saved_y = r.count;
                r.saved_x = m->saved.x < cols ? m->saved.x : cols - 1;
                r.saved_set = true;
            }
            int keep = len > cols ? cols : len;
            memcpy(reflow_dst(&r), src->c, (size_t)keep * sizeof(struct cell));
            r.x = keep;
            reflow_end_row(&r, wrapped);
            continue;
        }
        if (wrapped && len < old_cols) len = old_cols;
        if (y == m->y && len < m->x + m->pending) len = m->x + m->pending;
        if (y == m->saved.y && len <= m->saved.x) len = m->saved.x + 1;
        for (int x = 0; x < len; x++)
            line[x] = x < old_cols ? src->c[x] : default_blank();
        reflow_append(&r, line, len, y == m->y ? m->x + m->pending : -1,
                      y == m->saved.y ? m->saved.x : -1);
        if (!wrapped || i + 1 == total_in) reflow_end_row(&r, false);
    }
    free(line);
    if (r.x) reflow_end_row(&r, false);
    free(r.cur);

    int visible = rows - (cols > old_cols ? old_rows - active : 0);
    if (visible < 1) visible = 1;
    int start = r.count > visible ? r.count - visible : 0;
    if (start > r.cursor_y) start = r.cursor_y;
    int history = start;
    if (KNOWN_HISTORY_COUNTS_ROWS) {
        if (history > m->limit) history = m->limit;
    } else {
        int lines = 0;
        int first = start;
        for (int i = start - 1; i >= 0; i--) {
            if (i == start - 1 || !r.out[i]->wrapped) {
                if (lines == m->limit) {
                    first = i + 1;
                    break;
                }
                lines++;
            }
            if (i == 0) first = 0;
        }
        history = start - first;
    }

    m->cols = cols;
    wsap_vtcore_hist_clear(m);
    for (int i = start - history; i < start; i++) {
        hist_push(m, r.out[i]);
        r.out[i] = NULL;
    }
    free_rows(m->scr, old_rows);
    m->scr = calloc((size_t)rows, sizeof(*m->scr));
    if (!m->scr) abort();
    for (int y = 0; y < rows; y++) {
        if (start + y < r.count) {
            m->scr[y] = r.out[start + y];
            r.out[start + y] = NULL;
        } else {
            m->scr[y] = row_new(cols);
        }
    }
    for (int i = 0; i < r.count; i++) free(r.out[i]);
    free(r.out);

    m->rows = rows;
    m->x = r.cursor_x < cols ? r.cursor_x : cols - 1;
    m->y = r.cursor_y - start;
    m->pending = r.pending && m->autowrap;
    if (m->prompt >= 0) {
        if (!r.prompt_set || r.prompt - start >= rows) m->prompt = -1;
        else m->prompt = r.prompt > start ? r.prompt - start : 0;
    }
    if (r.saved_set && r.saved_y - start < rows) {
        m->saved.x = r.saved_x < cols ? r.saved_x : cols - 1;
        m->saved.y = r.saved_y < start ? 0 : r.saved_y - start;
    } else {
        m->saved.y = rows - 1;
    }
    if (m->saved.x >= cols) m->saved.x = cols - 1;
    if (m->saved.y >= rows) m->saved.y = rows - 1;
}

static void resize_plain(struct model *m, int cols, int rows)
{
    int old_cols = m->cols, old_rows = m->rows;
    if (cols != old_cols) {
        for (int y = 0; y < old_rows; y++) {
            struct row *n = row_resized(m->scr[y], old_cols, cols);
            free(m->scr[y]);
            m->scr[y] = n;
        }
        if (KNOWN_ALT_RESIZE_CUTS_NORMAL && m->alternate) {
            for (int y = 0; y < m->normal_rows; y++)
                if (m->normal[y]->len > cols) row_cut(m->normal[y], cols, m->normal_cols);
            for (int i = 0; i < m->hist_len; i++)
                if (m->hist[i]->len > cols) row_cut(m->hist[i], cols, m->normal_cols);
        }
        m->cols = cols;
    }
    if (rows < old_rows && m->y >= rows) {
        int shift = m->y - rows + 1;
        for (int i = 0; i < shift; i++) scroll_up_full(m);
        m->y -= shift;
        m->saved.y = m->saved.y >= shift ? m->saved.y - shift : 0;
    }
    if (rows != old_rows) {
        int pull = 0;
        if (rows > old_rows && !m->alternate)
            pull = m->hist_len < rows - old_rows ? m->hist_len : rows - old_rows;
        struct row **scr = calloc((size_t)rows, sizeof(*scr));
        if (!scr) abort();
        int y = 0;
        for (int i = 0; i < pull; i++) scr[y++] = m->hist[m->hist_len - pull + i];
        for (int i = 0; i < old_rows; i++) {
            if (y < rows) scr[y++] = m->scr[i];
            else free(m->scr[i]);
        }
        int first_new = y;
        while (y < rows) scr[y++] = row_new(cols);
        free(m->scr);
        m->scr = scr;
        m->rows = rows;
        for (y = first_new; y < rows; y++) erase_row(m, y);
        m->hist_len -= pull;
        m->hist_lines = 0;
        for (int i = 0; i < m->hist_len; i++)
            if (i == 0 || !m->hist[i - 1]->wrapped)
                m->hist_lines++;
        m->y += pull;
        m->saved.y += pull;
        if (m->prompt >= 0) m->prompt += pull;
    }
    if (!m->alternate && m->prompt >= rows) m->prompt = -1;
    if (cols != old_cols) m->pending = false;
    if (m->x >= cols) m->x = cols - 1;
    if (m->y >= rows) m->y = rows - 1;
    if (m->saved.x >= cols) m->saved.x = cols - 1;
    if (m->saved.y >= rows) m->saved.y = rows - 1;
}

static bool resize(struct model *m, int cols, int rows)
{
    if (cols > WSAP_VTCORE_MAX_COLS) return false;
    if (cols == m->cols && rows == m->rows) return true;
    if (cols > MAX_TABS) return false;
    m->top = 0;
    m->bottom = rows - 1;
    if (cols != m->cols && !m->alternate)
        reflow(m, cols, rows);
    else
        resize_plain(m, cols, rows);
    return true;
}

static void set_alternate(struct model *m, bool on)
{
    if (m->alternate == on) return;
    if (on) {
        m->normal = m->scr;
        m->normal_cols = m->cols;
        m->normal_rows = m->rows;
        m->normal_cursor = save_cursor(m);
        m->normal_saved = m->saved;
        m->normal_visible = m->visible;
        m->normal_style = m->style;
        m->normal_blink = m->blink;
        m->scr = rows_new(m->rows, m->cols);
        m->alternate = true;
        m->x = m->y = 0;
        m->pending = false;
    } else {
        int cols = m->cols, rows = m->rows;
        free_rows(m->scr, m->rows);
        m->scr = m->normal;
        m->normal = NULL;
        m->cols = m->normal_cols;
        m->rows = m->normal_rows;
        m->alternate = false;
        m->saved = m->normal_saved;
        m->visible = m->normal_visible;
        m->style = m->normal_style;
        m->blink = m->normal_blink;
        restore_cursor(m, &m->normal_cursor);
        m->x = m->normal_cursor.x;
        m->y = m->normal_cursor.y;
        m->pending = m->normal_cursor.pending;
        resize(m, cols, rows);
    }
    m->top = 0;
    m->bottom = m->rows - 1;
}

static void soft_reset(struct model *m)
{
    m->app_cursor = m->app_keypad = m->backarrow = false;
    m->insert = m->origin = m->reverse = m->lnm = false;
    m->focus = m->sync = false;
    m->mouse = m->mouse_enc = 0;
    m->bracketed = false;
    m->autowrap = m->visible = true;
    m->blink = true;
    m->style = WSAP_VTCORE_CURSOR_BLOCK;
    m->top = 0;
    m->bottom = m->rows - 1;
    m->decsace = 0;
    memset(m->keyboard, 0, sizeof(m->keyboard));
    pen_reset(&m->pen);
}

static void full_reset(struct model *m)
{
    set_alternate(m, false);
    soft_reset(m);
    m->x = m->y = 0;
    m->pending = false;
    m->saved = initial_saved();
    m->last_n = 0;
    m->g[0] = m->g[1] = 'B';
    m->gl = 0;
    m->prompt = -1;
    tabs_default(m);
    for (int y = 0; y < m->rows; y++) erase_row(m, y);
}

static int mode_get(const struct model *m, int mode, bool priv)
{
    if (!priv) {
        if (mode == 4) return m->insert;
        if (mode == 20) return m->lnm;
        return -1;
    }
    switch (mode) {
    case 1: return m->app_cursor;
    case 12: return m->blink;
    case 25: return m->visible;
    case 67: return m->backarrow;
    case 1004: return m->focus;
    case 2004: return m->bracketed;
    case 2026: return m->sync;
    case 5: return m->reverse;
    case 6: return m->origin;
    case 7: return m->autowrap;
    case 47: case 1047: case 1049: return m->alternate;
    case 69: return 0;
    case 9: case 1000: case 1001: case 1002: case 1003: return m->mouse == mode;
    case 1005: case 1006: case 1015: case 1016: return m->mouse_enc == mode;
    default: return -1;
    }
}

static void mode_set(struct model *m, int mode, bool on, bool priv)
{
    if (!priv) {
        if (mode == 4) m->insert = on;
        else if (mode == 20) m->lnm = on;
        return;
    }
    switch (mode) {
    case 1: m->app_cursor = on; break;
    case 12: m->blink = on; break;
    case 25: m->visible = on; break;
    case 67: m->backarrow = on; break;
    case 1004: m->focus = on; break;
    case 2004: m->bracketed = on; break;
    case 2026: m->sync = on; break;
    case 5: m->reverse = on; break;
    case 6:
        m->origin = on;
        move_to(m, 0, on ? m->top : 0);
        break;
    case 7:
        m->autowrap = on;
        m->pending = false;
        break;
    case 1048:
        if (on) m->saved = save_cursor(m);
        else restore_cursor(m, &m->saved);
        break;
    case 47: case 1047: case 1049:
        set_alternate(m, on);
        break;
    case 9: case 1000: case 1001: case 1002: case 1003:
        m->mouse = on ? mode : 0;
        break;
    case 1005: case 1006: case 1015: case 1016:
        m->mouse_enc = on ? mode : 0;
        break;
    default:
        break;
    }
}

static int count(const struct model *m)
{
    return m->param[0] > 0 ? m->param[0] : 1;
}

static bool csi_allowed(const struct model *m, unsigned char final)
{
    char prefix = m->prefix, inter = m->inter;
    if (final == 'm') return !prefix && !inter;
    if (final == 'H' || final == 'f') return !prefix && !inter && !m->sub;
    if (inter) {
        bool plain = (final == 'q' && inter == ' ') || (final == 'p' && inter == '!') ||
                     (final == 'x' && inter == '*');
        if (plain) {
            if (prefix || m->sub) return false;
        } else if (final == 'p' && inter == '$') {
            if (m->sub) return false;
        } else if (final == 'r' && inter == '$') {
            if (prefix) return false;
        } else {
            return false;
        }
    }
    if (prefix &&
        final != 'u' &&
        !(final == 'c' && (prefix == '>' || prefix == '=')) &&
        !(final == 'q' && prefix == '>' && !inter) &&
        !(final == 'p' && inter == '$' && prefix == '?') &&
        !(final == 'W' && prefix == '?') &&
        !((final == 'h' || final == 'l') && prefix == '?'))
        return false;
    if (m->sub && !(inter == '$' && final == 'r')) return false;
    return true;
}

static void keyboard_csi(struct model *m)
{
    if (m->nparam > (m->prefix == '=' ? 2 : 1)) return;
    struct keyboard *k = &m->keyboard[m->alternate];
    unsigned flags = (unsigned)m->param[0] & 31;
    if (m->prefix == '?') {
        reply(m, "\033[?%uu", k->flags);
    } else if (m->prefix == '>') {
        if (k->depth == 32) {
            memmove(k->saved, k->saved + 1, sizeof(k->saved) - sizeof(k->saved[0]));
            k->depth--;
        }
        k->saved[k->depth++] = k->flags;
        k->flags = flags;
    } else if (m->prefix == '<') {
        unsigned n = (unsigned)count(m);
        if (n > k->depth) {
            k->depth = 0;
            k->flags = 0;
        } else {
            k->depth -= n;
            k->flags = k->saved[k->depth];
        }
    } else if (m->prefix == '=') {
        int mode = m->nparam > 1 && m->param[1] ? m->param[1] : 1;
        if (mode == 1) k->flags = flags;
        else if (mode == 2) k->flags |= flags;
        else if (mode == 3) k->flags &= ~flags;
    }
}

static void csi_dispatch(struct model *m, unsigned char final)
{
    if (!csi_allowed(m, final)) return;
    int n = count(m);
    switch (final) {
    case 'm': sgr(m); break;
    case 'H': case 'f': {
        long y = (long)(m->param[0] > 0 ? m->param[0] - 1 : 0) + top_limit(m);
        if (y > bottom_limit(m)) y = bottom_limit(m);
        long x = m->nparam > 1 && m->param[1] > 0 ? m->param[1] - 1 : 0;
        move_to(m, (int)(x > m->cols - 1 ? m->cols - 1 : x), (int)y);
        break;
    }
    case 'r': {
        if (m->inter == '$') {
            deccara(m);
            break;
        }
        int top = n - 1;
        int bottom = m->nparam > 1 && m->param[1] ? m->param[1] - 1 : m->rows - 1;
        if (top < bottom && bottom < m->rows) {
            m->top = top;
            m->bottom = bottom;
            move_to(m, 0, m->origin ? top : 0);
        }
        break;
    }
    case 'L': case 'M':
        if (m->y < m->top || m->y > m->bottom) break;
        scroll_region(m, m->y, m->bottom, n, final == 'L', false);
        move_to(m, 0, m->y);
        break;
    case 'S': case 'T':
        if (m->nparam > 1) break;
        m->pending = false;
        scroll_region(m, m->top, m->bottom, n, final == 'T', true);
        break;
    case 'd': {
        long y = (long)n - 1 + top_limit(m);
        move_to(m, m->x, (int)(y > bottom_limit(m) ? bottom_limit(m) : y));
        break;
    }
    case 'b': {
        int times = n > m->cols * m->rows ? m->cols * m->rows : n;
        for (int i = 0; i < times && m->last_n; i++)
            put_glyph(m, m->last, m->last_n, m->pen.attrs);
        break;
    }
    case 'A': move_v(m, -n, false); break;
    case 'B': case 'e': move_v(m, n, false); break;
    case 'E': move_v(m, n, true); break;
    case 'F': move_v(m, -n, true); break;
    case 'a': case 'C': move_h(m, n); break;
    case 'D': move_h(m, -n); break;
    case 'G': case '`': {
        long x = (long)n - 1;
        move_to(m, (int)(x > m->cols - 1 ? m->cols - 1 : x), m->y);
        break;
    }
    case 's':
        if (!m->prefix) m->saved = save_cursor(m);
        break;
    case 'u':
        if (!m->prefix) restore_cursor(m, &m->saved);
        else keyboard_csi(m);
        break;
    case 'I': case 'Z': {
        int times = n > m->cols ? m->cols : n, dir = final == 'I' ? 1 : -1;
        while (times-- > 0) {
            int x = m->x + dir;
            while (x > 0 && x < m->cols - 1 && !m->tabs[x]) x += dir;
            m->x = x < 0 ? 0 : x >= m->cols ? m->cols - 1 : x;
        }
        m->pending = false;
        break;
    }
    case 'g':
        if (m->param[0] == 0) m->tabs[m->x] = false;
        else if (m->param[0] == 3) memset(m->tabs, 0, sizeof(m->tabs));
        break;
    case 'W':
        if (m->prefix == '?' && m->param[0] == 5) tabs_default(m);
        break;
    case 'n':
        if (m->prefix) break;
        if (m->param[0] == 5) reply(m, "\033[0n");
        else if (m->param[0] == 6) reply(m, "\033[%d;%dR", m->y - top_limit(m) + 1, m->x + 1);
        break;
    case 'J': m->pending = false; erase_display(m, m->param[0]); break;
    case 'K': m->pending = false; erase_line(m, m->param[0]); break;
    case 'X': {
        m->pending = false;
        int k = n > m->cols - m->x ? m->cols - m->x : n;
        erase_cells(m, m->y, m->x, m->x + k);
        break;
    }
    case 'x':
        if (m->inter == '*') m->decsace = m->param[0];
        break;
    case 'P': m->pending = false; delete_chars(m, n); break;
    case '@': m->pending = false; insert_chars(m, n); break;
    case 'c':
        if (m->prefix == '>') reply(m, "\033[>0;10;0c");
        else if (!m->prefix && m->param[0] == 0) reply(m, "\033[?62;22c");
        break;
    case 't':
        if (!m->prefix && m->param[0] == 14)
            reply(m, "\033[4;%d;%dt", m->rows * 20, m->cols * 10);
        break;
    case 'h': case 'l':
        for (int i = 0; i < m->nparam; i++)
            mode_set(m, m->param[i], final == 'h', m->prefix == '?');
        break;
    case 'q':
        if (m->prefix == '>' && !m->inter && (!m->nparam || !m->param[0])) {
            reply(m, "\033P>|%s\033\\", m->name);
        } else if (!m->prefix && m->inter == ' ') {
            int ps = m->nparam ? m->param[0] : 0;
            if (ps == 0) {
                m->style = WSAP_VTCORE_CURSOR_BLOCK;
                m->blink = true;
            } else if (ps <= 6) {
                m->style = ps < 3 ? WSAP_VTCORE_CURSOR_BLOCK : ps < 5 ? WSAP_VTCORE_CURSOR_UNDERLINE
                                                               : WSAP_VTCORE_CURSOR_BAR;
                m->blink = ps & 1;
            }
        }
        break;
    case 'p':
        if (m->inter == '!') {
            soft_reset(m);
            move_to(m, 0, 0);
        } else if (m->inter == '$' && m->nparam >= 1) {
            int set = mode_get(m, m->param[0], m->prefix == '?');
            reply(m, "\033[%s%d;%d$y", m->prefix == '?' ? "?" : "", m->param[0],
                  set < 0 ? 0 : set ? 1 : 2);
        }
        break;
    default:
        break;
    }
}

static void osc_finish(struct model *m)
{
    const char *s = m->str;
    size_t len = m->str_len;
    if (!m->alternate && len >= 5 && !strncmp(s, "133;", 4) &&
        (len == 5 || s[5] == ';')) {
        if (s[4] == 'A') m->prompt = m->y;
        if (s[4] == 'C' || s[4] == 'D') m->prompt = -1;
    }
}

static void dcs_finish(struct model *m)
{
    const char *s = m->str;
    size_t len = m->str_len;
    if (len < 2 || s[0] != '$' || s[1] != 'q') return;
    const char *spec = s + 2;
    size_t n = len - 2;
    char body[256];
    if (n == 2 && !memcmp(spec, " q", 2)) {
        int shape = m->style == WSAP_VTCORE_CURSOR_BLOCK ? (m->blink ? 1 : 2)
                  : m->style == WSAP_VTCORE_CURSOR_UNDERLINE ? (m->blink ? 3 : 4)
                  : (m->blink ? 5 : 6);
        reply(m, "\033P1$r%d q\033\\", shape);
    } else if (n == 1 && spec[0] == 'm') {
        sgr_report(m, body, sizeof(body));
        reply(m, "\033P1$r%sm\033\\", body);
    } else if (n == 1 && spec[0] == 'r') {
        reply(m, "\033P1$r%d;%dr\033\\", m->top + 1, m->bottom + 1);
    } else if (n == 1 && spec[0] == 's') {
        reply(m, "\033P1$r%d;%ds\033\\", 1, m->cols);
    } else if (n == 1 && spec[0] == 't') {
        reply(m, "\033P1$r%dt\033\\", m->rows);
    } else if (n == 2 && !memcmp(spec, "\"p", 2)) {
        reply(m, "\033P1$r64;1\"p\033\\");
    } else if (n == 2 && !memcmp(spec, "\"q", 2)) {
        reply(m, "\033P1$r0\"q\033\\");
    } else if (n == 2 && !memcmp(spec, "$|", 2)) {
        reply(m, "\033P1$r%d$|\033\\", m->cols);
    } else if (n == 2 && !memcmp(spec, "$}", 2)) {
        reply(m, "\033P1$r0$}\033\\");
    } else if (n == 2 && !memcmp(spec, "$~", 2)) {
        reply(m, "\033P1$r0$~\033\\");
    } else if (n == 2 && !memcmp(spec, "*x", 2)) {
        reply(m, "\033P1$r%d*x\033\\", m->decsace);
    } else if (n == 2 && !memcmp(spec, "*|", 2)) {
        reply(m, "\033P1$r%d*|\033\\", m->rows);
    } else {
        reply(m, "\033P0$r\033\\");
    }
}

static void str_add(struct model *m, unsigned char c)
{
    if (m->str_len < STR_KEEP) m->str[m->str_len] = (char)c;
    m->str_len++;
}

static void str_finish(struct model *m)
{
    if (m->str_kind == OSC) osc_finish(m);
    else if (m->str_kind == DCS) dcs_finish(m);
    m->str_len = 0;
}

static void esc_dispatch(struct model *m, unsigned char ch)
{
    m->state = GROUND;
    switch (ch) {
    case '=': m->app_keypad = true; break;
    case '>': m->app_keypad = false; break;
    case 'H': m->tabs[m->x] = true; break;
    case 'c': full_reset(m); break;
    case '[':
        m->state = CSI;
        m->nparam = 0;
        m->param[0] = 0;
        m->sub = 0;
        m->prefix = m->inter = 0;
        m->csi_phase = 0;
        break;
    case ']': m->state = STR; m->str_kind = OSC; m->str_len = 0; break;
    case 'P': m->state = STR; m->str_kind = DCS; m->str_len = 0; break;
    case '_': case '^': case 'X': m->state = STR; m->str_kind = IGNORE; m->str_len = 0; break;
    case '#': m->state = HASH; break;
    case '%': m->state = DOCS; break;
    case '(': case ')': case '*': case '+': case '-': case '.': case '/':
        m->slot = (char)ch;
        m->state = CHARSET;
        break;
    case 'D': case 'E':
        m->pending = false;
        if (ch == 'E') m->x = 0;
        line_feed(m);
        break;
    case 'M':
        m->pending = false;
        reverse_index(m);
        break;
    case '7': m->saved = save_cursor(m); break;
    case '8': restore_cursor(m, &m->saved); break;
    default: break;
    }
}

static void utf8_interrupt(struct model *m)
{
    if (m->need) {
        m->need = 0;
        print_cp(m, 0xFFFD);
    }
}

static void control(struct model *m, unsigned char c)
{
    switch (c) {
    case 0x1b: m->state = ESC; break;
    case '\n': case 0x0b: case 0x0c:
        m->pending = false;
        if (m->lnm || (m->flags & WSAP_VTCORE_FEED_ONLCR)) m->x = 0;
        line_feed(m);
        break;
    case '\r': m->pending = false; m->x = 0; break;
    case '\b':
        m->pending = false;
        if (m->x > 0) m->x--;
        break;
    case '\t': {
        m->pending = false;
        int x = m->x + 1;
        while (x < m->cols - 1 && !m->tabs[x]) x++;
        m->x = x < m->cols ? x : m->cols - 1;
        break;
    }
    case 0x0e: m->gl = 1; break;
    case 0x0f: m->gl = 0; break;
    default: break;
    }
}

static void ground(struct model *m, unsigned char c)
{
    if (c < 0x20 || c == 0x7f) {
        utf8_interrupt(m);
        if (c < 0x20) control(m, c);
        return;
    }
    if (m->need) {
        if (c >= m->lo && c <= m->hi) {
            m->acc = m->acc << 6 | (c & 0x3fu);
            m->lo = 0x80;
            m->hi = 0xbf;
            if (--m->need == 0) print_cp(m, m->acc);
            return;
        }
        utf8_interrupt(m);
    }
    if (c < 0x80) {
        print_cp(m, c);
        return;
    }
    m->lo = 0x80;
    m->hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) { m->need = 1; m->acc = c & 0x1fu; }
    else if (c == 0xe0) { m->need = 2; m->acc = c & 0x0fu; m->lo = 0xa0; }
    else if (c == 0xed) { m->need = 2; m->acc = c & 0x0fu; m->hi = 0x9f; }
    else if (c >= 0xe1 && c <= 0xef) { m->need = 2; m->acc = c & 0x0fu; }
    else if (c == 0xf0) { m->need = 3; m->acc = c & 0x07u; m->lo = 0x90; }
    else if (c == 0xf4) { m->need = 3; m->acc = c & 0x07u; m->hi = 0x8f; }
    else if (c >= 0xf1 && c <= 0xf3) { m->need = 3; m->acc = c & 0x07u; }
    else print_cp(m, 0xFFFD);
}

static void csi_byte(struct model *m, unsigned char c)
{
    if (c == 0x7f) return;
    if (c >= 0x40) {
        if (m->csi_phase != 3) csi_dispatch(m, c);
        m->state = GROUND;
        return;
    }
    if (m->csi_phase == 3) return;
    if (c >= '0' && c <= ';') {
        if (m->csi_phase == 2) {
            m->csi_phase = 3;
            return;
        }
        m->csi_phase = 1;
        if (!m->nparam) m->nparam = 1;
        int *p = &m->param[m->nparam - 1];
        if (c <= '9') {
            *p = *p < 214748364 ? *p * 10 + (c - '0') : 0x7fffffff;
        } else {
            if (m->nparam == 16) {
                m->csi_phase = 3;
                return;
            }
            if (c == ':') m->sub |= (uint16_t)(1u << m->nparam);
            m->param[m->nparam++] = 0;
        }
    } else if (c >= '<') {
        if (m->csi_phase != 0 || m->prefix) m->csi_phase = 3;
        else m->prefix = (char)c;
    } else {
        if (m->csi_phase == 2) {
            m->csi_phase = 3;
        } else {
            m->csi_phase = 2;
            m->inter = (char)c;
        }
    }
}

static void feed_byte(struct model *m, unsigned char c)
{
    if (c >= 0x80 && m->state != GROUND && m->state != STR) {
        m->state = GROUND;
        ground(m, c);
        return;
    }
    switch (m->state) {
    case GROUND:
        ground(m, c);
        break;
    case ESC:
        if (c < 0x20) {
            m->state = GROUND;
            ground(m, c);
        } else {
            esc_dispatch(m, c);
        }
        break;
    case CSI:
        if (c == 0x1b) m->state = ESC;
        else if (c == 0x18 || c == 0x1a) m->state = GROUND;
        else if (c < 0x20) control(m, c);
        else csi_byte(m, c);
        break;
    case CHARSET:
        if (m->slot == '(' || m->slot == ')') m->g[m->slot == ')'] = (char)c;
        m->state = GROUND;
        break;
    case HASH:
        m->state = GROUND;
        if (c == '8') {
            for (int y = 0; y < m->rows; y++) {
                struct row *r = m->scr[y];
                for (int x = 0; x < m->cols; x++) {
                    r->c[x] = default_blank();
                    r->c[x].cp[0] = 'E';
                }
                r->len = m->cols;
                r->wrapped = false;
            }
            move_to(m, 0, 0);
        }
        break;
    case DOCS:
        m->state = GROUND;
        if (c != 'G' && c != '@' && c != '/') {
            ground(m, '%');
            ground(m, c);
        }
        break;
    case STR:
        if (m->str_kind == IGNORE) {
            if (c == 0x07 || c == 0x18 || c == 0x1a) m->state = GROUND;
            else if (c == 0x1b) m->state = STR_ESC;
            break;
        }
        if (c == 0x07) {
            str_finish(m);
            m->state = GROUND;
        } else if (c == 0x1b) {
            m->state = STR_ESC;
        } else if (c < 0x20) {
            m->str_len = 0;
            m->state = GROUND;
            ground(m, c);
        } else {
            str_add(m, c);
        }
        break;
    case STR_ESC:
        if (m->str_kind == IGNORE) {
            if (c == '\\' || c == 0x07 || c == 0x18 || c == 0x1a) {
                m->state = GROUND;
            } else {
                m->state = ESC;
                feed_byte(m, c);
            }
            break;
        }
        if (c == '\\') {
            str_finish(m);
            m->state = GROUND;
        } else {
            m->str_len = 0;
            m->state = ESC;
            feed_byte(m, c);
        }
        break;
    }
}

struct model *model_new(struct wsap_vtcore_vector2i size, int history, const char *name)
{
    struct model *m = calloc(1, sizeof(*m));
    if (!m) abort();
    snprintf(m->name, sizeof(m->name), "%s", name && *name ? name : "wsap_vtcore");
    m->cols = size.x < 1 ? 80 : size.x;
    m->rows = size.y < 1 ? 24 : size.y;
    m->limit = history_limit(history);
    m->scr = rows_new(m->rows, m->cols);
    m->autowrap = m->visible = m->blink = true;
    m->style = WSAP_VTCORE_CURSOR_BLOCK;
    m->bottom = m->rows - 1;
    m->g[0] = m->g[1] = 'B';
    m->saved = initial_saved();
    m->prompt = -1;
    pen_reset(&m->pen);
    tabs_default(m);
    (void)KNOWN_HISTORY_COUNTS_ROWS;
    return m;
}

void model_free(struct model *m)
{
    if (!m) return;
    free_rows(m->scr, m->rows);
    free_rows(m->normal, m->normal_rows);
    wsap_vtcore_hist_clear(m);
    free(m->hist);
    free(m->reply);
    for (int i = 0; i < m->out_cap; i++) free(m->out[i]);
    free(m->out);
    free(m);
}

void model_feed(struct model *m, const char *s, size_t len, unsigned flags)
{
    m->flags = flags;
    for (size_t i = 0; i < len; i++) feed_byte(m, (unsigned char)s[i]);
}

bool model_resize(struct model *m, struct wsap_vtcore_vector2i size)
{
    if (size.x < 1 || size.y < 1) return false;
    return resize(m, size.x, size.y);
}

struct wsap_vtcore_vector2i model_size(const struct model *m)
{
    return (struct wsap_vtcore_vector2i){ .x = m->cols, .y = m->rows };
}

int model_history(const struct model *m)
{
    return m->alternate ? 0 : m->hist_len;
}

const struct model_cell *model_row(const struct model *m, int row)
{
    int hist = model_history(m);
    if (row < 0 || (row >= hist && row - hist >= m->rows)) return NULL;

    struct model *mm = (struct model *)m;
    const struct row *r = row < hist ? m->hist[row] : m->scr[row - hist];
    if (mm->out_cols != m->cols) {
        for (int i = 0; i < mm->out_cap; i++) {
            free(mm->out[i]);
            mm->out[i] = NULL;
        }
        mm->out_cols = m->cols;
    }
    if (row >= mm->out_cap) {
        int cap = mm->out_cap ? mm->out_cap : 16;
        while (cap <= row) {
            if (cap > INT_MAX / 2) abort();
            cap *= 2;
        }
        struct model_cell **out = realloc(mm->out, (size_t)cap * sizeof(*out));
        if (!out) abort();
        memset(out + mm->out_cap, 0, (size_t)(cap - mm->out_cap) * sizeof(*out));
        mm->out = out;
        mm->out_cap = cap;
    }
    if (!mm->out[row]) {
        mm->out[row] = calloc((size_t)m->cols, sizeof(*mm->out[row]));
        if (!mm->out[row]) abort();
    }
    for (int x = 0; x < m->cols; x++) {
        const struct cell *c = &r->c[x];
        struct model_cell *o = &mm->out[row][x];
        memcpy(o->cp, c->cp, sizeof(o->cp));
        o->n = c->n;
        o->width = c->width;
        o->attrs = c->attrs;
        o->fg = c->fg;
        o->bg = c->bg;
    }
    return mm->out[row];
}

struct wsap_vtcore_cursor model_cursor(const struct model *m)
{
    return (struct wsap_vtcore_cursor){
        .pos = { .x = m->x, .y = m->y },
        .style = (enum wsap_vtcore_cursor_style)m->style,
        .visible = m->visible,
        .blink = m->blink,
    };
}

struct wsap_vtcore_modes model_modes(const struct model *m)
{
    return (struct wsap_vtcore_modes){
        .keyboard = m->keyboard[m->alternate].flags,
        .mouse = (enum wsap_vtcore_mouse)m->mouse,
        .mouse_encoding = (enum wsap_vtcore_mouse_encoding)m->mouse_enc,
        .app_cursor = m->app_cursor,
        .app_keypad = m->app_keypad,
        .backarrow_bs = m->backarrow,
        .bracketed_paste = m->bracketed,
        .focus_events = m->focus,
        .alternate = m->alternate,
        .reverse_video = m->reverse,
        .synchronized = m->sync,
    };
}

const char *model_replies(const struct model *m, size_t *len)
{
    *len = m->reply_len;
    return m->reply ? m->reply : "";
}

void model_take_replies(struct model *m)
{
    m->reply_len = 0;
}
