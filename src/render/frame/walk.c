#include "render/frame/walk.h"

#include <stdlib.h>
#include <string.h>

#include "font/cell_draw.h"
#include "font/glyph/intern.h"

/*
 * The segment code points of the calling thread. Only the thread that owns
 * them reads or writes them, so a walk on one thread never sees the walk of
 * another thread.
 */
static _Thread_local uint32_t *walk_cps;
static _Thread_local int walk_cps_cap;

void render_walk_thread_release(void)
{
    free(walk_cps);
    walk_cps = NULL;
    walk_cps_cap = 0;
}

static uint32_t *walk_scratch(int n)
{
    if (n <= walk_cps_cap)
        return walk_cps;
    int cap = walk_cps_cap ? walk_cps_cap : 256;
    while (cap < n)
        cap *= 2;
    uint32_t *p = realloc(walk_cps, (size_t)cap * sizeof(*p));
    if (!p)
        return NULL;
    walk_cps = p;
    walk_cps_cap = cap;
    return p;
}

/* A segment has at least two cells. A shorter row has no scratch to reserve. */
bool render_walk_thread_reserve(int cols)
{
    return cols < 2 || (walk_scratch(cols) && glyph_cache_reserve_segment(cols));
}

/* The glyph half of a resolved cell: everything except the colors. */
struct look {
    uint64_t id;
    uint32_t text;
    uint16_t style;
    uint16_t flags;
};

/* The state of one row walk. */
struct walk {
    struct font_instance *fi;
    const struct terminal_frame *f;
    const struct wsap_vtcore_view *view;
    const struct wsap_vtcore_cell *cells;
    render_cell_fn fn;
    render_run_fn run;
    render_text_fn text;
    render_span_fn span;
    void *ctx;
    int cols, c0, c1;
    bool peek;
    /* Row policy. An empty span means that the row has none. */
    struct wsap_vtcore_span sel, cur;
    uint32_t sel_fg, sel_bg;
    uint16_t cur_first, cur_rest;
    bool block;
    /* The look of the last wide head. A tail at head_next reuses it. */
    int head_next;
    struct look head;
    /* The shaped segment [base, seg_end). Columns before next are done. */
    int base, next, seg_end;
};

#define ATTR_DECOR_BITS \
    (WSAP_VTCORE_ATTR_UNDERLINE_MASK | WSAP_VTCORE_ATTR_STRIKE | \
     WSAP_VTCORE_ATTR_OVERLINE | WSAP_VTCORE_ATTR_LINK)
#define ATTR_VARIANT_BITS (WSAP_VTCORE_ATTR_BOLD | WSAP_VTCORE_ATTR_ITALIC)

static inline uint16_t cell_style(const struct wsap_vtcore_cell *c)
{
    return (uint16_t)((c->attrs & WSAP_VTCORE_ATTR_BOLD ? FONT_STYLE_BOLD : 0) |
                      (c->attrs & WSAP_VTCORE_ATTR_ITALIC ? FONT_STYLE_ITALIC : 0) |
                      (c->attrs & WSAP_VTCORE_ATTR_EMOJI ? FONT_STYLE_EMOJI : 0) |
                      (c->attrs & WSAP_VTCORE_ATTR_TEXT ? FONT_STYLE_TEXT : 0) |
                      (c->width == 2 ? FONT_STYLE_WIDE : 0));
}

/* The decoration and link bits of the attributes. */
static inline uint16_t decor_flags(uint16_t attrs)
{
    if (!(attrs & ATTR_DECOR_BITS))
        return 0;
    unsigned ul = wsap_vtcore_attr_underline(attrs);
    uint16_t fl = 0;
    if (attrs & WSAP_VTCORE_ATTR_LINK) {
        fl |= RENDER_CELL_LINK;
        if (ul == WSAP_VTCORE_UNDERLINE_NONE)
            ul = WSAP_VTCORE_UNDERLINE_SINGLE;
    }
    fl |= (uint16_t)(ul << RENDER_CELL_UNDERLINE_SHIFT);
    if (attrs & WSAP_VTCORE_ATTR_STRIKE)
        fl |= RENDER_CELL_STRIKE;
    if (attrs & WSAP_VTCORE_ATTR_OVERLINE)
        fl |= RENDER_CELL_OVERLINE;
    return fl;
}

/* FAINT goes only on a cell that draws a glyph or a decoration. */
static inline uint16_t faint_flag(uint16_t attrs, uint16_t flags)
{
    bool marks = !(flags & RENDER_CELL_BLANK) || (flags & RENDER_CELL_DECOR_MASK);
    return (attrs & WSAP_VTCORE_ATTR_FAINT) && marks ? RENDER_CELL_FAINT : 0;
}

/*
 * A shapeable cell can join a segment: text above ' ', no cluster, one
 * column, and no cell-aligned symbol. The font draws those symbols itself.
 */
static inline bool shapeable(const struct wsap_vtcore_cell *c)
{
    return c->text > ' ' && !(c->text & WSAP_VTCORE_TEXT_CLUSTER) && c->width == 1 &&
           (c->text < 0x2500 || !is_cell_aligned_symbol(c->text));
}

static inline bool same_segment(const struct wsap_vtcore_cell *a, const struct wsap_vtcore_cell *b)
{
    return a->fg == b->fg && a->bg == b->bg && !((a->attrs ^ b->attrs) & ATTR_VARIANT_BITS);
}

/*
 * A segment is a maximal run of shapeable cells with one raw fg, raw bg,
 * and variant. Any other cell is a segment of its own. Each column is in
 * exactly one segment, so the segment of a column does not depend on where
 * the walk starts.
 */
static int segment_start(const struct wsap_vtcore_cell *cells, int c)
{
    const struct wsap_vtcore_cell *first = &cells[c];
    if (!shapeable(first))
        return c;
    while (c > 0 && shapeable(&cells[c - 1]) && same_segment(&cells[c - 1], first))
        c--;
    return c;
}

static int segment_end(const struct wsap_vtcore_cell *cells, int c, int cols)
{
    const struct wsap_vtcore_cell *first = &cells[c];
    if (!shapeable(first))
        return c + 1;
    int end = c + 1;
    while (end < cols && shapeable(&cells[end]) && same_segment(&cells[end], first))
        end++;
    return end;
}

static inline struct look blank_look(uint16_t flags)
{
    return (struct look){
        .id = RENDER_GLYPH_ID_BLANK, .text = ' ', .flags = flags | RENDER_CELL_BLANK,
    };
}

/*
 * The look of a cluster. It gets its glyph_intern() handle. The epoch reads
 * around the intern give a glyph id of 0 when handles were reused during
 * it. The id keeps the epoch read after the intern.
 */
static __attribute__((noinline)) struct look
cluster_look(const struct walk *w, const struct wsap_vtcore_cell *c, uint16_t flags)
{
    uint32_t before = glyph_intern_epoch();
    uint32_t text = terminal_frame_glyph(w->f, c->text);
    uint32_t after = glyph_intern_epoch();
    if (text <= ' ')
        return blank_look(flags);
    uint16_t style = cell_style(c);
    return (struct look){
        .id = before == after ? render_glyph_id_unshaped(text, style, after) : 0,
        .text = text, .style = style, .flags = flags,
    };
}

/* The look of a cell that is not a wide tail and not a never-written cell. */
static inline __attribute__((always_inline)) struct look
cell_look(const struct walk *w, const struct wsap_vtcore_cell *c)
{
    uint16_t flags = decor_flags(c->attrs) | (c->width == 2 ? RENDER_CELL_WIDE : 0);
    struct look l;
    if (c->text <= ' ') {
        l = blank_look(flags);
    } else if (__builtin_expect(c->text & WSAP_VTCORE_TEXT_CLUSTER, 0)) {
        l = cluster_look(w, c, flags);
    } else {
        uint16_t style = cell_style(c);
        l = (struct look){
            .id = render_glyph_id_unshaped(c->text, style, 0),
            .text = c->text, .style = style, .flags = flags,
        };
    }
    l.flags |= faint_flag(c->attrs, l.flags);
    return l;
}

/* A never-written cell (text 0, width 1) has the default colors and no
 * attributes. */
static inline bool never_written(const struct wsap_vtcore_cell *c)
{
    return c->width != 0 && c->text == 0;
}

/* The look of the wide head at `col`. Its tail at col + 1 reuses it. */
static struct look head_look(struct walk *w, int col)
{
    const struct wsap_vtcore_cell *c = &w->cells[col];
    w->head = never_written(c) ? blank_look(RENDER_CELL_WIDE) : cell_look(w, c);
    w->head_next = col + 1;
    return w->head;
}

/*
 * The look of the cell at `col`, and its palette colors. A wide tail takes
 * the look of its head. Its colors stay its own.
 */
static inline __attribute__((always_inline)) struct look
resolve_look(struct walk *w, int col, struct render_cell *rc)
{
    const struct wsap_vtcore_cell *c = &w->cells[col];
    if (never_written(c)) {
        rc->fg = w->f->default_fg;
        rc->bg = w->f->default_bg;
        return c->width == 2 ? head_look(w, col) : blank_look(0);
    }
    rc->fg = wsap_vtcore_view_color(w->view, c->fg);
    rc->bg = wsap_vtcore_view_color(w->view, c->bg);
    if (__builtin_expect(c->width == 2, 0))
        return head_look(w, col);
    if (__builtin_expect(c->width == 0, 0)) {
        if (col == 0 || w->cells[col - 1].width != 2) {
            struct look l = blank_look(decor_flags(c->attrs));
            l.flags |= faint_flag(c->attrs, l.flags);
            return l;
        }
        struct look l = w->head_next == col ? w->head : head_look(w, col - 1);
        l.flags = (uint16_t)((l.flags & ~RENDER_CELL_WIDE) | RENDER_CELL_WIDE_TAIL);
        return l;
    }
    return cell_look(w, c);
}

static inline bool in_span(struct wsap_vtcore_span s, int col)
{
    return (unsigned)(col - s.begin) < (unsigned)(s.end - s.begin);
}

/* Reverse video, the selection, and the cursor, in this order. */
static inline void apply_policy(const struct walk *w, int col, struct render_cell *rc)
{
    if (w->f->reverse_video) {
        uint32_t fg = rc->fg;
        rc->fg = rc->bg;
        rc->bg = fg;
    }
    if (in_span(w->sel, col)) {
        rc->fg = w->sel_fg;
        rc->bg = w->sel_bg;
    }
    if (in_span(w->cur, col)) {
        if (w->block) {
            rc->fg = rc->bg == w->f->cursor_color ? w->f->default_bg : rc->bg;
            rc->bg = w->f->cursor_color;
        }
        rc->flags |= col == w->cur.begin ? w->cur_first : w->cur_rest;
    }
}

static inline void emit(const struct walk *w, int col, const struct render_cell *rc,
                        const struct look *l, const struct glyph *g)
{
    const struct render_glyph_ref ref = {
        .g = g, .id = l->id, .font_ = w->fi, .text_ = rc->text, .style_ = rc->style,
        .blank_ = (rc->flags & RENDER_CELL_BLANK) != 0,
    };
    w->fn(w->ctx, col, rc, &ref);
}

/* One cell outside a shaped span. The glyph fetch comes right before fn. */
static inline __attribute__((always_inline)) void
walk_cell(struct walk *w, int col, bool plain)
{
    struct render_cell rc;
    struct look l = resolve_look(w, col, &rc);
    rc.text = l.text;
    rc.style = l.style;
    rc.flags = l.flags;
    if (!plain)
        apply_policy(w, col, &rc);
    const struct glyph *g = NULL;
    if (!w->peek && !(l.flags & RENDER_CELL_BLANK))
        g = glyph_cache_fetch(w->fi, l.text, l.style);
    emit(w, col, &rc, &l, g);
}

/* A cell of a row outside the snapshot: blank, in the default colors. */
static inline __attribute__((always_inline)) void
walk_blank(const struct walk *w, int col, bool plain)
{
    struct render_cell rc = {
        .fg = w->f->default_fg, .bg = w->f->default_bg,
        .text = ' ', .flags = RENDER_CELL_BLANK,
    };
    if (!plain)
        apply_policy(w, col, &rc);
    const struct look l = blank_look(0);
    emit(w, col, &rc, &l, NULL);
}

/*
 * A cell that a text run can hold: printable ASCII, one column, and no
 * attribute that adds a flag or a style bit beyond the variant.
 */
static inline bool text_cell(const struct wsap_vtcore_cell *c)
{
    return c->text - 0x21u < 0x5Eu && c->width == 1 &&
           !(c->attrs & (ATTR_DECOR_BITS | WSAP_VTCORE_ATTR_FAINT | WSAP_VTCORE_ATTR_EMOJI |
                         WSAP_VTCORE_ATTR_TEXT));
}

static inline bool same_text_run(const struct wsap_vtcore_cell *a,
                                 const struct wsap_vtcore_cell *b)
{
    return text_cell(a) && a->fg == b->fg && a->bg == b->bg &&
           !((a->attrs ^ b->attrs) & ATTR_VARIANT_BITS);
}

/*
 * The policy of a run that starts at `col` in a policy row. It cuts
 * [col, *end) at the edges of the selection and of the cursor, and it
 * applies reverse video and the selection to the colors. The run must not
 * start in the cursor: those cells take the per-cell path.
 */
static inline __attribute__((always_inline)) void
run_policy(const struct walk *w, int col, int *end, uint32_t *fg, uint32_t *bg)
{
    if (w->f->reverse_video) {
        uint32_t t = *fg;
        *fg = *bg;
        *bg = t;
    }
    if (w->cur.begin < w->cur.end && w->cur.begin > col && w->cur.begin < *end)
        *end = w->cur.begin;
    if (in_span(w->sel, col)) {
        *fg = w->sel_fg;
        *bg = w->sel_bg;
        if (w->sel.end < *end)
            *end = w->sel.end;
    } else if (w->sel.begin < w->sel.end && w->sel.begin > col && w->sel.begin < *end) {
        *end = w->sel.begin;
    }
}

/*
 * Sends the unshaped cells [from, to) of one segment. Runs of text cells go
 * to the text function. A column that it does not draw goes to the cell
 * function, and the run goes on after it. In a policy row, a run stops at
 * the edges of the selection and of the cursor, and the cursor cells take
 * the per-cell path.
 */
static inline __attribute__((always_inline)) void
walk_text(struct walk *w, int from, int to, bool plain)
{
    const struct wsap_vtcore_cell *cells = w->cells;
    const int stride = (int)(sizeof(*cells) / sizeof(cells->text));
    for (int col = from; col < to;) {
        const struct wsap_vtcore_cell *c = &cells[col];
        if (!text_cell(c) || (!plain && in_span(w->cur, col))) {
            walk_cell(w, col, plain);
            col++;
            continue;
        }
        int end = col + 1;
        while (end < to && same_text_run(&cells[end], c))
            end++;
        uint32_t fg = wsap_vtcore_view_color(w->view, c->fg);
        uint32_t bg = wsap_vtcore_view_color(w->view, c->bg);
        if (!plain)
            run_policy(w, col, &end, &fg, &bg);
        uint16_t style = cell_style(c);
        while (col < end) {
            int done = w->text(w->ctx, col, end, fg, bg, style, &cells[col].text, stride);
            if (done < col)
                done = col;
            if (done >= end)
                break;
            walk_cell(w, done, plain);
            col = done + 1;
        }
        col = end;
    }
}

static inline __attribute__((always_inline)) void
walk_cells(struct walk *w, int from, int to, bool plain)
{
    if (from < w->c0)
        from = w->c0;
    if (to > w->c1)
        to = w->c1;
    if (w->text) {
        walk_text(w, from, to, plain);
        return;
    }
    for (int col = from; col < to; col++)
        walk_cell(w, col, plain);
}

/* One cell of a shaped span, at column col. The span starts at `first`. */
static inline __attribute__((always_inline)) void
walk_span_cell(const struct walk *w, const struct glyph_span *span, int first, int col, bool plain)
{
    const struct wsap_vtcore_cell *c = &w->cells[col];
    int i = col - first;
    struct render_cell rc = {
        .fg = wsap_vtcore_view_color(w->view, c->fg),
        .bg = wsap_vtcore_view_color(w->view, c->bg),
        .text = c->text,
        .style = cell_style(c),
        .flags = decor_flags(c->attrs),
    };
    rc.flags |= faint_flag(c->attrs, rc.flags);
    if (!plain)
        apply_policy(w, col, &rc);
    const struct look l = { .id = render_glyph_id_shaped(span->id, i) };
    emit(w, col, &rc, &l, &span->cells[i]);
}

/* A run of shaped cells holds at most this many columns. */
#define SPAN_RUN_MAX 64

/*
 * Sends the shaped cells [from, end) of one span. Runs of cells with no
 * flag go to the span function, at most SPAN_RUN_MAX at a time. The cells
 * of a segment share their raw colors, so a run resolves them once. A
 * column that the span function does not draw goes to the cell function.
 */
static inline __attribute__((always_inline)) void
walk_span_runs(struct walk *w, const struct glyph_span *span, int first, int from, int end,
               bool plain)
{
    const struct wsap_vtcore_cell *cells = w->cells;
    const uint16_t flag_attrs = ATTR_DECOR_BITS | WSAP_VTCORE_ATTR_FAINT;
    uint64_t ids[SPAN_RUN_MAX];
    for (int col = from; col < end;) {
        const struct wsap_vtcore_cell *c = &cells[col];
        if ((c->attrs & flag_attrs) || (!plain && in_span(w->cur, col))) {
            walk_span_cell(w, span, first, col, plain);
            col++;
            continue;
        }
        int stop = end - col > SPAN_RUN_MAX ? col + SPAN_RUN_MAX : end;
        int e = col + 1;
        while (e < stop && !(cells[e].attrs & flag_attrs))
            e++;
        uint32_t fg = wsap_vtcore_view_color(w->view, c->fg);
        uint32_t bg = wsap_vtcore_view_color(w->view, c->bg);
        if (!plain)
            run_policy(w, col, &e, &fg, &bg);
        for (int k = col; k < e; k++)
            ids[k - col] = render_glyph_id_shaped(span->id, k - first);
        const int base = col;
        while (col < e) {
            int done = w->span(w->ctx, col, e, fg, bg, ids + (col - base));
            if (done < col)
                done = col;
            if (done >= e)
                break;
            walk_span_cell(w, span, first, done, plain);
            col = done + 1;
        }
        col = e;
    }
}

/*
 * The emit callback of glyph_cache_shape_segment(). It walks the unshaped
 * cells before the span, then the span cells, inside the callback. So each
 * span glyph is used before the callback returns.
 */
static inline __attribute__((always_inline)) void
walk_span(struct walk *w, const struct glyph_span *span, bool plain)
{
    int first = w->base + span->start;
    int from = first > w->next ? first : w->next;
    int end = first + span->count;
    if (end > w->seg_end)
        end = w->seg_end;
    if (from >= end)
        return;
    walk_cells(w, w->next, from, plain);
    w->next = end;
    if (from < w->c0)
        from = w->c0;
    if (end > w->c1)
        end = w->c1;
    if (w->span) {
        walk_span_runs(w, span, first, from, end, plain);
        return;
    }
    for (int col = from; col < end; col++)
        walk_span_cell(w, span, first, col, plain);
}

static void walk_span_plain(void *ctx, const struct glyph_span *span)
{
    walk_span(ctx, span, true);
}

static void walk_span_policy(void *ctx, const struct glyph_span *span)
{
    walk_span(ctx, span, false);
}

/*
 * The bg of a cell that draws only its bg: a BLANK cell with no other flag.
 * Returns false for any other cell. The bg is the one before the selection
 * and the cursor. Reverse video draws the fg as the bg.
 */
static inline __attribute__((always_inline)) bool
blank_bg(const struct walk *w, const struct wsap_vtcore_cell *c, uint32_t *bg)
{
    if (c->width != 1 || c->text > ' ')
        return false;
    bool rev = w->f->reverse_video;
    if (c->text == 0) {
        *bg = rev ? w->f->default_fg : w->f->default_bg;
        return true;
    }
    if (c->attrs & ATTR_DECOR_BITS)
        return false;
    *bg = wsap_vtcore_view_color(w->view, rev ? c->fg : c->bg);
    return true;
}

/*
 * The end of the blank run that starts at `c` with bg `bg`, at most `end`.
 * A cell equal to the cell before it continues the run with no other test.
 */
static inline __attribute__((always_inline)) int
blank_run_end(const struct walk *w, int c, int end, uint32_t bg)
{
    const struct wsap_vtcore_cell *cells = w->cells;
    int e = c + 1;
    for (; e < end; e++) {
        if (!memcmp(&cells[e], &cells[e - 1], sizeof(*cells)))
            continue;
        uint32_t b;
        if (!blank_bg(w, &cells[e], &b) || b != bg)
            break;
    }
    return e;
}

/*
 * Sends the blank run [a, b) of bg `bg` to the run function. In a policy
 * row, the selection gives its bg to the part that it covers. The cells
 * of the cursor take the per-cell path.
 */
static inline __attribute__((always_inline)) void
walk_run(struct walk *w, int a, int b, uint32_t bg, bool plain)
{
    if (a < w->c0)
        a = w->c0;
    if (b > w->c1)
        b = w->c1;
    if (plain) {
        if (a < b)
            w->run(w->ctx, a, b, bg);
        return;
    }
    while (a < b) {
        int e = b;
        if (in_span(w->cur, a)) {
            if (w->cur.end < e)
                e = w->cur.end;
            for (; a < e; a++) {
                if (w->cells)
                    walk_cell(w, a, false);
                else
                    walk_blank(w, a, false);
            }
            continue;
        }
        if (w->cur.begin < w->cur.end && w->cur.begin > a && w->cur.begin < e)
            e = w->cur.begin;
        uint32_t color = bg;
        if (in_span(w->sel, a)) {
            color = w->sel_bg;
            if (w->sel.end < e)
                e = w->sel.end;
        } else if (w->sel.begin < w->sel.end && w->sel.begin > a && w->sel.begin < e) {
            e = w->sel.begin;
        }
        w->run(w->ctx, a, e, color);
        a = e;
    }
}

static inline __attribute__((always_inline)) void walk_row(struct walk *w, bool plain)
{
    if (!w->cells) {
        if (w->run) {
            walk_run(w, w->c0, w->c1,
                     w->f->reverse_video ? w->f->default_fg : w->f->default_bg, plain);
            return;
        }
        for (int col = w->c0; col < w->c1; col++)
            walk_blank(w, col, plain);
        return;
    }
    const struct wsap_vtcore_cell *cells = w->cells;
    for (int c = segment_start(cells, w->c0); c < w->c1;) {
        uint32_t bg;
        if (w->run && blank_bg(w, &cells[c], &bg)) {
            int end = blank_run_end(w, c, w->c1, bg);
            walk_run(w, c, end, bg, plain);
            c = end;
            continue;
        }
        int end = segment_end(cells, c, w->cols);
        uint32_t *cps = end - c > 1 ? walk_scratch(end - c) : NULL;
        if (cps) {
            for (int i = c; i < end; i++)
                cps[i - c] = cells[i].text;
            w->base = w->next = c;
            w->seg_end = end;
            glyph_cache_shape_segment(w->fi, cps, end - c,
                                      cell_style(&cells[c]) & FONT_STYLE_VARIANT_MASK,
                                      plain ? walk_span_plain : walk_span_policy, w);
            walk_cells(w, w->next, end, plain);
        } else {
            walk_cells(w, c, end, plain);
        }
        c = end;
    }
}

static void walk_row_plain(struct walk *w)
{
    walk_row(w, true);
}

static void walk_row_policy(struct walk *w)
{
    walk_row(w, false);
}

/*
 * The columns that the cursor covers: its cell, and the other half when
 * that cell is half of a two-column glyph.
 */
static struct wsap_vtcore_span cursor_span(const struct walk *w, int x)
{
    struct wsap_vtcore_span s = { .begin = x, .end = x + 1 };
    const struct wsap_vtcore_cell *cells = w->cells;
    if (!cells)
        return s;
    if (cells[x].width == 2 && x + 1 < w->cols && cells[x + 1].width == 0)
        s.end = x + 2;
    else if (cells[x].width == 0 && x > 0 && cells[x - 1].width == 2)
        s.begin = x - 1;
    return s;
}

void render_row_walk(struct font_instance *fi, const struct terminal_frame *f, int y,
                     int c0, int c1, unsigned flags, const struct render_walk_sink *sink)
{
    if (!fi || !f || !sink || !sink->cell || !f->source)
        return;
    if (c0 < 0)
        c0 = 0;
    if (c1 > f->cols)
        c1 = f->cols;
    if (c0 >= c1)
        return;
    struct walk w = {
        .fi = fi, .f = f, .view = wsap_vtcore_snapshot_view(f->source),
        .cells = terminal_frame_row(f, y), .fn = sink->cell, .run = sink->run,
        .text = sink->text, .span = sink->span, .ctx = sink->ctx,
        .cols = f->cols, .c0 = c0, .c1 = c1, .peek = (flags & RENDER_WALK_PEEK) != 0,
    };
    if (f->has_selection) {
        w.sel = terminal_frame_row_selection(f, y);
        w.sel_fg = terminal_frame_selection_fg(f);
        w.sel_bg = terminal_frame_selection_bg(f);
    }
    if (f->cursor_y == y && f->cursor_x >= 0 && f->cursor_x < f->cols) {
        switch (f->cursor_style) {
        case WSAP_VTCORE_CURSOR_BLOCK:
            w.block = true;
            w.cur = cursor_span(&w, f->cursor_x);
            break;
        case WSAP_VTCORE_CURSOR_BAR:
            w.cur_first = RENDER_CELL_CURSOR_BAR;
            w.cur = cursor_span(&w, f->cursor_x);
            break;
        case WSAP_VTCORE_CURSOR_UNDERLINE:
            w.cur_first = w.cur_rest = RENDER_CELL_CURSOR_UNDERLINE;
            w.cur = cursor_span(&w, f->cursor_x);
            break;
        default:
            break;
        }
    }
    bool plain = !f->reverse_video && w.sel.end <= w.sel.begin && w.cur.end <= w.cur.begin;
    if (plain)
        walk_row_plain(&w);
    else
        walk_row_policy(&w);
}
