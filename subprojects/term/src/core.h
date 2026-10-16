#ifndef WSAP_TERM_CORE_H
#define WSAP_TERM_CORE_H

#include "compat.h"
#include "internal.h"
#include "screen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct span {
    struct srow *row;
    struct cell *cells;
    int avail;
    int x;
    int old_len;
};

/* Begin writing a span of cells. Settle cursor row, repair left edge. */
static inline struct span sc_span_begin(struct wsap_vtcore *t, int want);

/* Finish writing a span of cells. Repair right edge, raise len, bump gen. */
static inline void sc_span_end(struct wsap_vtcore *t, struct span *s, int cols,
                               uint8_t row_flags, uint32_t last);

/* Print ASCII characters through span and SIMD chunking. */
static inline void sc_print_ascii(struct wsap_vtcore *t, const char *s, int n);

/* Print one glyph (single codepoint or cluster handle). */
void wsap_vtcore_sc_print_glyph(struct wsap_vtcore *t, uint32_t cp, int width, uint16_t flags);

static inline void sc_print_cell(struct wsap_vtcore *t, uint32_t cp, int width, uint16_t flags)
{
    if (WSAP_VTCORE_EXPECT(width == 1 && !t->pending_wrap && !t->insert_mode &&
                    !t->lr_margin_mode && t->cursor_x < t->cols, 1)) {
        struct srow *srow = wsap_vtcore_row(t, t->cursor_y);
        int cx = t->cursor_x;
        int len = srow->len;
        if (WSAP_VTCORE_EXPECT(srow->kind == ROW_CELLS && cx <= len, 1)) {
            struct cell *row = srow->cells;
            if (WSAP_VTCORE_EXPECT(!((cx < len && (row[cx].flags & CELL_TRAIL)) ||
                              (cx + 1 < len && (row[cx + 1].flags & CELL_TRAIL))), 1)) {
                row[cx] = (struct cell){cp,
                    t->inverse ? t->cur_bg : t->cur_fg,
                    t->inverse ? t->cur_fg : t->cur_bg,
                    flags, t->cur_link};
                t->last_char = cp;
                cx++;
                if (cx > len) srow->len = (uint16_t)cx;
                srow->version++;
                if (cx < t->cols) {
                    t->cursor_x = cx;
                    t->pending_wrap = false;
                } else {
                    t->cursor_x = t->cols - 1;
                    t->pending_wrap = t->autowrap;
                }
                return;
            }
        }
    }
    wsap_vtcore_sc_print_glyph(t, cp, width, flags);
}

/* Extend the last printed glyph into a grapheme cluster. */
bool wsap_vtcore_sc_extend_cluster(struct wsap_vtcore *t, uint32_t cp, int klass);

/* Multi-line or region-constrained line feed helper. */
void wsap_vtcore_sc_linefeed_slow(struct wsap_vtcore *t, size_t n);

/* Line feed: move down or scroll with BCE. */
static inline void sc_linefeed(struct wsap_vtcore *t, size_t n)
{
    if (WSAP_VTCORE_EXPECT(n == 1 && t->scroll_top == 0 &&
                         t->scroll_bottom == t->rows - 1, 1)) {
        t->pending_wrap = false;
        wsap_vtcore_row(t, t->cursor_y)->version++;
        if (WSAP_VTCORE_EXPECT(t->cursor_y == t->rows - 1, 1))
            wsap_vtcore_screen_scroll_up(t);
        else
            t->cursor_y++;
        return;
    }
    wsap_vtcore_sc_linefeed_slow(t, n);
}

/* Carriage return: move cursor to left margin. */
static inline void sc_cr(struct wsap_vtcore *t)
{
    t->pending_wrap = false;
    if (t->cursor_x != 0) {
        t->cursor_x = 0;
        wsap_vtcore_row(t, t->cursor_y)->version++;
    }
}

/* Move cursor to (x, y), margin-aware. */
static inline void sc_move(struct wsap_vtcore *t, int x, int y)
{
    t->pending_wrap = false;
    if (x < 0) x = 0;
    if (x >= t->cols) x = t->cols - 1;
    if (y < 0) y = 0;
    if (y >= t->rows) y = t->rows - 1;

    int old_y = t->cursor_y;
    if (x == t->cursor_x && y == old_y)
        return;
    t->cursor_x = x;
    t->cursor_y = y;
    wsap_vtcore_row(t, old_y)->version++;
    if (y != old_y)
        wsap_vtcore_row(t, y)->version++;
}

/* Advance cursor by n tab stops. */
void wsap_vtcore_sc_tab(struct wsap_vtcore *t, int n);

/* Erase cells in span [from, to) on physical row y. */
static inline void sc_erase_span(struct wsap_vtcore *t, int y, int from, int to)
{
    t->pending_wrap = false;
    if (to > t->cols) to = t->cols;
    int p = phys_row(t, y);
    erase_span(t, p, from, to);
    t->scr.slot[p].version++;
}

/* Restore saved cursor state and position. */
static inline void sc_restore_cursor(struct wsap_vtcore *t, const struct wsap_vtcore_saved_cursor *s)
{
    wsap_vtcore_cursor_apply(t, s);
    sc_move(t, s->x, s->y);
    t->pending_wrap = s->pending_wrap;
}

/* Change attributes in a rectangular or stream area (DECCARA). */
void wsap_vtcore_sc_deccara(struct wsap_vtcore *t, const struct csi *csi);

/* Screen alignment pattern (DECALN). */
void wsap_vtcore_sc_decaln(struct wsap_vtcore *t);

/* Reset screen and cursor state for full reset (RIS). */
void wsap_vtcore_sc_reset(struct wsap_vtcore *t);

/* Set cursor style (DECSCUSR). */
void wsap_vtcore_sc_set_cursor_style(struct wsap_vtcore *t, int ps);

/* Bulk streaming of ASCII lines with raw rows and skip-ahead. */
size_t wsap_vtcore_sc_bulk_ascii_lines(struct wsap_vtcore *t, const char *buf, size_t len);

/* Fast CSI parser. */
int wsap_vtcore_try_fast_csi(struct wsap_vtcore *t, const char *buf, size_t rem);

static inline struct span sc_span_begin(struct wsap_vtcore *t, int want)
{
    (void)want;
    if (WSAP_VTCORE_EXPECT(t->pending_wrap, 0)) {
        row_set_wrapped(wsap_vtcore_row(t, t->cursor_y), true);
        t->cursor_x = t->lr_margin_mode ? t->left_margin : 0;
        sc_linefeed(t, 1);
        t->pending_wrap = false;
    }
    struct srow *row = wsap_vtcore_row(t, t->cursor_y);
    if (WSAP_VTCORE_EXPECT(row->kind != ROW_CELLS, 0))
        srow_settle(row, t->cols);
    int cx = t->cursor_x;
    wsap_vtcore_row_fill_gap(row, cx);
    int len = row->len;
    if (WSAP_VTCORE_EXPECT(cx < len && (row->cells[cx].flags & CELL_TRAIL), 0))
        fill_cells(row->cells + cx - 1, 1, row->cells[cx - 1].fg, row->cells[cx - 1].bg);

    int right = WSAP_VTCORE_EXPECT(t->lr_margin_mode, 0) ? t->right_margin : t->cols - 1;
    int avail = right + 1 > cx ? right + 1 - cx : 0;
    return (struct span){
        .row = row,
        .cells = row->cells + cx,
        .avail = avail,
        .x = cx,
        .old_len = len,
    };
}

static inline void sc_span_end(struct wsap_vtcore *t, struct span *s, int cols,
                               uint8_t row_flags, uint32_t last)
{
    struct srow *row = s->row;
    int end_x = s->x + cols;
    if (WSAP_VTCORE_EXPECT(end_x < s->old_len && (row->cells[end_x].flags & CELL_TRAIL), 0))
        fill_cells(row->cells + end_x, 1, row->cells[end_x].fg, row->cells[end_x].bg);
    if (end_x > s->old_len)
        row->len = (uint16_t)end_x;
    if (WSAP_VTCORE_EXPECT(row_flags != 0, 0))
        row->flags |= row_flags;
    row->version++;
    t->last_char = last;

    if (WSAP_VTCORE_EXPECT(cols < s->avail, 1)) {
        t->cursor_x = end_x;
        t->pending_wrap = false;
    } else {
        t->cursor_x = s->x + s->avail - 1;
        t->pending_wrap = t->autowrap;
    }
}

/* Move the cursor after cols columns written at x, of avail. */
static inline void sc_text_advance(struct wsap_vtcore *t, int x, int cols, int avail)
{
    if (cols < avail) {
        t->cursor_x = x + cols;
        t->pending_wrap = false;
    } else {
        t->cursor_x = x + avail - 1;
        t->pending_wrap = t->autowrap;
    }
}

/* Append ASCII at the cursor, with the pen,
 * when the cursor row takes text. Return the bytes taken. */
static inline int sc_text_ascii(struct wsap_vtcore *t, const char *s, int n, struct text_pen pen)
{
    if (WSAP_VTCORE_EXPECT(t->insert_mode || t->lr_margin_mode, 0))
        return 0;
    if (t->pending_wrap) {
        row_set_wrapped(wsap_vtcore_row(t, t->cursor_y), true);
        t->cursor_x = 0;
        sc_linefeed(t, 1);
        t->pending_wrap = false;
    }
    struct srow *row = text_append_row(t);
    if (!row)
        return 0;
    int x = t->cursor_x, avail = t->cols - x;
    int take = n < avail ? n : avail;
    if (take <= 0)
        return 0;
    text_append(row, (const uint8_t *)s, take, take, take, HLINE_NARROW, pen);
    t->last_char = (unsigned char)s[take - 1];
    sc_text_advance(t, x, take, avail);
    return take;
}

static inline void sc_print_ascii(struct wsap_vtcore *t, const char *s, int n)
{
    struct text_pen pen = wsap_vtcore_text_pen(t);

    for (int k; n > 0 && (k = sc_text_ascii(t, s, n, pen)) > 0; s += k, n -= k) {}

    while (n > 0) {
        struct span span = sc_span_begin(t, n);
        int take = n < span.avail ? n : span.avail;
        if (WSAP_VTCORE_EXPECT(take <= 0, 0)) {
            t->pending_wrap = t->autowrap;
            if (!t->autowrap)
                break;
            span = sc_span_begin(t, n);
            take = n < span.avail ? n : span.avail;
            if (take <= 0)
                break;
        }
        put_ascii_chunk(span.cells, s, take, pen.fg, pen.bg, pen.sl);
        sc_span_end(t, &span, take, 0, (unsigned char)s[take - 1]);
        if (WSAP_VTCORE_EXPECT(n <= take, 1)) break;
        s += take;
        n -= take;
    }
}

/* Write a short ASCII span without settling an already writable row. */
static inline bool sc_ascii_fast(struct wsap_vtcore *t, const char *s, int n,
                                 struct text_pen pen)
{
    int cx = t->cursor_x;
    if (WSAP_VTCORE_EXPECT(t->pending_wrap || t->insert_mode ||
                         t->lr_margin_mode || t->inverse ||
                         n <= 0 || n > t->cols - cx, 0))
        return false;
    struct srow *row = wsap_vtcore_row(t, t->cursor_y);
    if (row->kind == ROW_TEXT || (row->len == 0 && cx == 0)) {
        if (sc_text_ascii(t, s, n, pen) == n)
            return true;
        if (row->kind == ROW_TEXT)
            return false;
    }
    if (WSAP_VTCORE_EXPECT(row->kind != ROW_CELLS || cx > row->len, 0))
        return false;
    int len = row->len;
    struct cell *cells = row->cells;
    if (WSAP_VTCORE_EXPECT((cx < len && (cells[cx].flags & CELL_TRAIL)) ||
                         (cx + n < len && (cells[cx + n].flags & CELL_TRAIL)), 0))
        return false;
    put_ascii_chunk(cells + cx, s, n, pen.fg, pen.bg, pen.sl);
    cx += n;
    if (cx > len)
        row->len = (uint16_t)cx;
    row->version++;
    t->last_char = (unsigned char)s[n - 1];
    if (cx < t->cols) {
        t->cursor_x = cx;
        t->pending_wrap = false;
    } else {
        t->cursor_x = t->cols - 1;
        t->pending_wrap = t->autowrap;
    }
    return true;
}

/* Fast inline check for 1-character write (fg-char fast path). */
static inline bool sc_put1_fast(struct wsap_vtcore *t, char ch, struct text_pen pen)
{
    int cx = t->cursor_x;
    int cols = t->cols;
    if (WSAP_VTCORE_EXPECT(t->pending_wrap | t->insert_mode | t->lr_margin_mode | t->inverse, 0))
        return false;
    if (WSAP_VTCORE_EXPECT(cx >= cols, 0))
        return false;
    struct srow *row = wsap_vtcore_row(t, t->cursor_y);
    if (row->kind == ROW_TEXT) {
        struct text_row *tr = &row->text;
        if (WSAP_VTCORE_EXPECT(cx != tr->cells || !tr->cells || cx + 1 == cols, 0))
            return sc_text_ascii(t, &ch, 1, pen) == 1;
        /* One character in the middle of a text row. */
        tr->text[tr->text_len++] = (uint8_t)ch;
        tr->cells++;
        hist_runs_add_pen(&tr->runs, pen, 1);
        if (pen.sl >> 16)
            tr->flags |= HLINE_HAS_LINK;
        if (tr->cells > row->len)
            row->len = tr->cells;
        row->version++;
        t->last_char = (uint8_t)ch;
        t->cursor_x = cx + 1;
        return true;
    }
    if (row->len == 0 && cx == 0) {
        if (sc_text_ascii(t, &ch, 1, pen) == 1)
            return true;
    }
    int len = row->len;
    if (WSAP_VTCORE_EXPECT(cx > len, 0))
        return false;
    struct cell *cells = row->cells;
    if (WSAP_VTCORE_EXPECT((cx < len && (cells[cx].flags & CELL_TRAIL)) ||
                         (cx + 1 < len && (cells[cx + 1].flags & CELL_TRAIL)), 0))
        return false;

    cells[cx] = (struct cell){ (uint8_t)ch, pen.fg, pen.bg,
                               (uint16_t)pen.sl, (uint16_t)(pen.sl >> 16) };
    t->last_char = (uint8_t)ch;
    cx++;
    if (cx > len)
        row->len = (uint16_t)cx;
    row->version++;
    if (WSAP_VTCORE_EXPECT(cx < cols, 1)) {
        t->cursor_x = cx;
    } else {
        t->cursor_x = cols - 1;
        t->pending_wrap = t->autowrap;
    }
    return true;
}

#endif
