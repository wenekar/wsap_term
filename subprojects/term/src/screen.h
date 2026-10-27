#ifndef WSAP_VTCORE_SCREEN_H
#define WSAP_VTCORE_SCREEN_H

#include <assert.h>
#include "compat.h"
#include "internal.h"
#include "kernels.h"
#include "simd.h"

void wsap_vtcore_screen_scroll_region(struct wsap_vtcore *t, int top, int bottom, int count, bool down, bool history);
void wsap_vtcore_screen_set_alternate(struct wsap_vtcore *t, bool enabled);
void wsap_vtcore_screen_erase(struct wsap_vtcore *t, int mode);
void wsap_vtcore_screen_erase_all(struct wsap_vtcore *t);
void wsap_vtcore_screen_erase_line(struct wsap_vtcore *t, int mode);
void wsap_vtcore_screen_delete_chars(struct wsap_vtcore *t, int count);
void wsap_vtcore_screen_insert_chars(struct wsap_vtcore *t, int count);

const struct cell *wsap_vtcore_screen_view_row(const struct wsap_vtcore *t, int y);
uint64_t wsap_vtcore_screen_view_row_key(const struct wsap_vtcore *t, int y);
bool wsap_vtcore_selection_view_spans(const struct wsap_vtcore *t, int *c0, int *c1);

int wsap_vtcore_screen_history_rows(const struct wsap_vtcore *t);

int wsap_vtcore_view_offset(const struct wsap_vtcore *t);

static inline bool screen_has_history(const struct wsap_vtcore *t)
{
    return !t->alternate && t->hist.line_limit > 0;
}

static inline int phys_row(const struct wsap_vtcore *t, int y)
{
    int p = t->scr.top + y;
    return p >= t->scr.rows ? p - t->scr.rows : p;
}

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline void set_cells_wrap_pad_avx2(struct cell *cells, int count)
{
    const __m256i mask = _mm256_setr_epi32(0, 0, 0, (int)CELL_WRAP_PAD,
                                            0, 0, 0, (int)CELL_WRAP_PAD);
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(cells + i));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(cells + i + 2));
        v0 = _mm256_or_si256(v0, mask);
        v1 = _mm256_or_si256(v1, mask);
        _mm256_storeu_si256((__m256i *)(cells + i), v0);
        _mm256_storeu_si256((__m256i *)(cells + i + 2), v1);
    }
    for (; i < count; i++)
        cells[i].flags |= CELL_WRAP_PAD;
}
#endif

static inline void set_cells_wrap_pad(struct cell *cells, int count)
{
    if (count <= 0)
        return;
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2() && count >= 4) {
        set_cells_wrap_pad_avx2(cells, count);
        return;
    }
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const __m128i mask = _mm_setr_epi32(0, 0, 0, (int)CELL_WRAP_PAD);
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)(cells + i));
        __m128i v1 = _mm_loadu_si128((const __m128i *)(cells + i + 1));
        _mm_storeu_si128((__m128i *)(cells + i), _mm_or_si128(v0, mask));
        _mm_storeu_si128((__m128i *)(cells + i + 1), _mm_or_si128(v1, mask));
    }
    for (; i < count; i++)
        cells[i].flags |= CELL_WRAP_PAD;
#elif defined(__aarch64__)
    const uint32x4_t mask = { 0, 0, 0, CELL_WRAP_PAD };
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        uint32x4_t v0 = vld1q_u32((const uint32_t *)(cells + i));
        uint32x4_t v1 = vld1q_u32((const uint32_t *)(cells + i + 1));
        vst1q_u32((uint32_t *)(cells + i), vorrq_u32(v0, mask));
        vst1q_u32((uint32_t *)(cells + i + 1), vorrq_u32(v1, mask));
    }
    for (; i < count; i++)
        cells[i].flags |= CELL_WRAP_PAD;
#else
    for (int i = 0; i < count; i++)
        cells[i].flags |= CELL_WRAP_PAD;
#endif
}

static inline void init_ascii_text_row(struct text_row *tr, uint8_t *text, uint16_t len)
{
    tr->text = text;
    tr->text_len = tr->cells = len;
    tr->wide = tr->fill_to = 0;
    tr->flags = HLINE_NARROW;
    tr->runs.len = 0;
    tr->runs.count = len;
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
    static_assert(sizeof(struct pen_attr) == 12,
                  "a 16-byte store writes one pen and 4 spare bytes");
    static_assert(offsetof(struct hist_runs, base) + 12 == offsetof(struct hist_runs, pen),
                  "the spare bytes of the store at base land on pen");
    static_assert(offsetof(struct hist_runs, pen) + 16 <= sizeof(struct hist_runs),
                  "the store at pen stays inside hist_runs");
    static_assert(offsetof(struct text_row, start) + 16 <= offsetof(struct text_row, runs),
                  "the store at start stays before runs");
#endif
#if defined(__x86_64__) || defined(_M_X64)
    __m128i pen = _mm_setr_epi32(WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, 0, WSAP_VTCORE_COLOR_FG);
    _mm_storeu_si128((__m128i *)&tr->start, pen);
    _mm_storeu_si128((__m128i *)&tr->runs.base, pen);
    _mm_storeu_si128((__m128i *)&tr->runs.pen, pen);
#elif defined(__aarch64__)
    uint32x4_t pen = { WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, 0, WSAP_VTCORE_COLOR_FG };
    vst1q_u32((uint32_t *)&tr->start, pen);
    vst1q_u32((uint32_t *)&tr->runs.base, pen);
    vst1q_u32((uint32_t *)&tr->runs.pen, pen);
#else
    tr->start = tr->runs.base = tr->runs.pen = pen_attr_default();
#endif
}

static inline void srow_materialize_text(struct srow *row)
{
    if (row->kind == ROW_ASCII) {
        init_ascii_text_row(&row->text, row->text.text, row->len);
        row->kind = ROW_TEXT;
    }
}

static inline struct srow *screen_get_row(const struct screen *s, int y)
{
    int p = s->top + y;
    struct srow *row = &s->slot[p >= s->rows ? p - s->rows : p];
    srow_materialize_text(row);
    return row;
}

static inline struct srow *screen_row_at(const struct screen *s, int y)
{
    if (y < 0 || y >= s->rows)
        return NULL;
    return screen_get_row(s, y);
}

static inline struct srow *wsap_vtcore_row(const struct wsap_vtcore *t, int y)
{
    return screen_get_row(&t->scr, y);
}

static inline int text_row_decode(const struct srow *row, struct cell *out, int cols)
{
    srow_materialize_text((struct srow *)row);
    const struct text_row *tr = &row->text;
    int n = wsap_vtcore_hist_text_decode(tr, out, cols, NULL);
    int fill = tr->fill_to < cols ? tr->fill_to : cols;
    if (n < fill) {
        fill_cells(out + n, fill - n, tr->fill.fg, tr->fill.bg);
        n = fill;
    }
    return n;
}

static inline void text_row_cells(const struct srow *row, struct cell *out, int cols)
{
    int n = text_row_decode(row, out, cols);
    if (n < cols)
        fill_cells(out + n, cols - n, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
}

static inline void srow_settle(struct srow *row, int cols)
{
    srow_materialize_text(row);
    if (row->kind == ROW_TEXT) {
        int n = text_row_decode(row, row->cells, cols);
        if (n < cols && !(row->flags & RF_TAIL_PADDED))
            fill_cells(row->cells + n, cols - n, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
        row->flags |= RF_TAIL_PADDED;
        row->kind = ROW_CELLS;
    }
}

static inline struct srow *text_append_row(struct wsap_vtcore *t)
{
    struct srow *row = wsap_vtcore_row(t, t->cursor_y);
    if (row->kind == ROW_TEXT) {
        text_row_unshare(&row->text);
        if (t->cursor_x != row->text.cells)
            return NULL;
        if (row->text.cells)
            return row;
    } else if (row->len != 0 || t->cursor_x != 0 || t->cursor_y != t->rows - 1 ||
               t->scroll_top != 0 || t->scroll_bottom != t->rows - 1 || !screen_has_history(t)) {
        return NULL;
    }
    struct pen_attr start = pen_attr_default();
    if (t->cursor_y > 0) {
        const struct srow *prev = wsap_vtcore_row(t, t->cursor_y - 1);
        if (prev->kind == ROW_TEXT && (prev->flags & RF_WRAPPED))
            start = prev->text.runs.pen;
    }
    if (row->kind == ROW_TEXT) {
        struct text_pen p = text_pen_from_attr(&start);
        pen_set_text(&row->text.start, p);
        pen_set_text(&row->text.runs.base, p);
        pen_set_text(&row->text.runs.pen, p);
        return row;
    }
    row->kind = ROW_TEXT;
    text_row_start(&row->text, &start);
    return row;
}

static inline void text_commit(struct srow *row, int nbytes, int chars, int cells, uint8_t hflags,
                               struct text_pen pen)
{
    struct text_row *tr = &row->text;
    tr->text_len = (uint16_t)(tr->text_len + nbytes);
    tr->cells = (uint16_t)(tr->cells + cells);
    tr->wide = (uint16_t)(tr->wide + cells - chars);
    tr->flags = (uint8_t)((tr->flags & hflags & HLINE_NARROW) |
                          ((tr->flags | hflags) & ~HLINE_NARROW) |
                          ((pen.sl >> 16) ? HLINE_HAS_LINK : 0));
    hist_runs_add_pen(&tr->runs, pen, (uint32_t)chars);
    if (tr->cells > row->len)
        row->len = tr->cells;
    row->version++;
    if (hflags & HLINE_HAS_WIDE)
        row->flags |= RF_WIDE;
}

static inline void text_append(struct srow *row, const uint8_t *bytes, int nbytes, int chars,
                               int cells, uint8_t hflags, struct text_pen pen)
{
    bytes_copy_small(row->text.text + row->text.text_len, (const char *)bytes, nbytes);
    text_commit(row, nbytes, chars, cells, hflags, pen);
}

static inline struct cell *row_cells(const struct wsap_vtcore *t, int p)
{
    struct srow *row = &t->scr.slot[p];
    if (row->kind != ROW_CELLS)
        srow_settle(row, t->cols);
    return row->cells;
}

static inline struct line_pos screen_top_line(const struct wsap_vtcore *t)
{
    if (!screen_has_history(t))
        return (struct line_pos){ t->scr.top_id, t->scr.top_seg };
    uint64_t end = hist_end_id(&t->hist);
    if (!hist_is_open(&t->hist))
        return (struct line_pos){ end, 0 };
    return (struct line_pos){ end - 1, (uint32_t)wsap_vtcore_hist_line_rows(&t->hist, end - 1, t->cols) };
}

static inline struct line_pos line_pos_next(struct line_pos p, const struct srow *row)
{
    return (row->flags & RF_WRAPPED) ? (struct line_pos){ p.id, p.seg + 1 }
                                     : (struct line_pos){ p.id + 1, 0 };
}

static inline uint64_t first_retained_line(const struct wsap_vtcore *t)
{
    if (screen_has_history(t) && t->hist.lines)
        return hist_first_id(&t->hist);
    return screen_top_line(t).id;
}

static inline void row_set_wrapped(struct srow *row, bool wrapped)
{
    row->flags = wrapped ? (row->flags | RF_WRAPPED) : (row->flags & ~RF_WRAPPED);
}

static inline void clear_wide_edges(struct cell *row, int from, int to, int len)
{
    if (from >= to) return;
    if (from > 0 && from < len && (row[from].flags & CELL_TRAIL))
        fill_cells(row + from - 1, 1, row[from - 1].fg, row[from - 1].bg);
    if (to < len && (row[to].flags & CELL_TRAIL))
        fill_cells(row + to, 1, row[to].fg, row[to].bg);
}

static inline void repair_wide_row(struct cell *row, int from, int to, int cols, int len)
{
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (to > cols) to = cols;
    for (int x = from; x < to; x++) {
        if ((row[x].flags & CELL_TRAIL) &&
            (x == 0 || !(row[x - 1].flags & CELL_WIDE)))
            fill_cells(row + x, 1, row[x].fg, row[x].bg);
        if ((row[x].flags & CELL_WIDE) && cols > 1 &&
            (x + 1 == cols || x + 1 >= len || !(row[x + 1].flags & CELL_TRAIL)))
            fill_cells(row + x, 1, row[x].fg, row[x].bg);
    }
}

static inline bool using_default_colors(const struct wsap_vtcore *t)
{
    return t->cur_fg == WSAP_VTCORE_COLOR_FG && t->cur_bg == WSAP_VTCORE_COLOR_BG;
}

static inline void mark_row_blank(struct wsap_vtcore *t, int p)
{
    struct srow *row = &t->scr.slot[p];
    uint8_t tail_padded = ((row->kind == ROW_CELLS && row->len == 0) || row->kind == ROW_TEXT)
                          ? (row->flags & RF_TAIL_PADDED) : 0;
    row->kind = ROW_CELLS;
    row->len = 0;
    row->flags = tail_padded;
    row->version++;
}

static inline void wsap_vtcore_row_fill_gap(struct srow *row, int x)
{
    if (x > row->len) {
        if (!(row->flags & RF_TAIL_PADDED))
            fill_cells(row->cells + row->len, x - row->len, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
        row->len = (uint16_t)x;
    }
}

static inline void write_cell_span(struct cell *row, int x,
                                   struct cell cell, int span)
{
    row[x] = cell;
    if (span == 2) {
        row[x + 1] = cell;
        row[x + 1].c = 0;
        row[x + 1].flags = (uint16_t)((cell.flags & ~CELL_WIDE) | CELL_TRAIL);
    }
}

static inline void erase_span(struct wsap_vtcore *t, int p, int from, int to)
{
    bool pristine = using_default_colors(t);
    struct srow *row = &t->scr.slot[p];
    int len = row->len;
    if (to > WSAP_VTCORE_MAX_COLS) to = WSAP_VTCORE_MAX_COLS;
    if (pristine && to > len) to = len;
    if (from >= to) return;

    if (pristine && from == 0 && to >= len) {
        mark_row_blank(t, p);
        return;
    }
    struct cell *cells = row_cells(t, p);
    if (from > 0 && from < len && (cells[from].flags & CELL_TRAIL)) from--;
    if (to < len && (cells[to].flags & CELL_TRAIL)) to++;
    if (to >= t->cols)
        row_set_wrapped(row, false);

    if (!pristine) {
        wsap_vtcore_row_fill_gap(row, from);
        fill_cells(cells + from, to - from, t->cur_fg, t->cur_bg);
        if (to > len) {
            row->len = (uint16_t)to;
            if (to < t->cols)
                row->flags &= ~RF_TAIL_PADDED;
        }
        row->version++;
        return;
    }
    if (to < len) {
        fill_cells(cells + from, to - from, t->cur_fg, t->cur_bg);
        row->version++;
        return;
    }
    if (from == 0) {
        mark_row_blank(t, p);
    } else {
        row->len = (uint16_t)from;
        row->flags &= ~RF_TAIL_PADDED;
        row->version++;
    }
}

static inline int erase_width(const struct wsap_vtcore *t, int p)
{
    return t->scr.slot[p].len > t->cols ? t->scr.slot[p].len : t->cols;
}

static inline void erase_row(struct wsap_vtcore *t, int p)
{
    erase_span(t, p, 0, erase_width(t, p));
}

static inline void clamp_selection_after_scroll(struct wsap_vtcore *t)
{
    if (WSAP_VTCORE_EXPECT(!t->selection, 1))
        return;
    uint64_t first_id = first_retained_line(t);
    if (t->sel_end.line_id < first_id) {
        t->selection = false;
    } else if (t->sel_start.line_id < first_id) {
        t->sel_start.line_id = first_id;
        t->sel_start.cell_off = 0;
    }
}

void wsap_vtcore_screen_push_row(struct wsap_vtcore *t, struct srow *row);

static inline void screen_drop_top(struct screen *s, const struct srow *row)
{
    struct line_pos p = line_pos_next((struct line_pos){ s->top_id, s->top_seg }, row);
    s->top_id = p.id;
    s->top_seg = p.seg;
}

static inline void screen_new_row_plain(struct wsap_vtcore *t, struct srow *row)
{
    row->uid = ++t->next_row_uid;
    row->version++;
    row->flags = ((row->kind == ROW_CELLS && row->len == 0) || row->kind == ROW_TEXT)
                 ? (row->flags & RF_TAIL_PADDED) : 0;
    row->kind = ROW_CELLS;
    row->len = 0;
}

static inline void screen_new_row(struct wsap_vtcore *t, struct srow *row)
{
    row->uid = ++t->next_row_uid;
    row->version++;
    row->flags = ((row->kind == ROW_CELLS && row->len == 0) || row->kind == ROW_TEXT)
                 ? (row->flags & RF_TAIL_PADDED) : 0;
    if (WSAP_VTCORE_EXPECT(using_default_colors(t) || t->cur_bg == WSAP_VTCORE_COLOR_BG, 1)) {
        row->kind = ROW_CELLS;
        row->len = 0;
    } else {
        row->kind = ROW_TEXT;
        text_row_start(&row->text, &(struct pen_attr){ .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG });
        row->text.fill_to = (uint16_t)t->scr.cols;
        row->text.fill = (struct pen_attr){ .fg = t->cur_fg, .bg = t->cur_bg };
        row->len = (uint16_t)t->scr.cols;
    }
}

static inline void wsap_vtcore_screen_scroll_up_wrap(struct wsap_vtcore *t)
{
    struct screen *s = &t->scr;
    struct srow *row = &s->slot[s->top];
    if (WSAP_VTCORE_EXPECT(screen_has_history(t), 1))
        wsap_vtcore_screen_push_row(t, row);
    else
        screen_drop_top(s, row);
    if (WSAP_VTCORE_EXPECT(t->prompt_y > 0, 0) && !t->alternate)
        t->prompt_y--;
    s->top = s->top + 1 == s->rows ? 0 : s->top + 1;
    clamp_selection_after_scroll(t);
    screen_new_row_plain(t, row);
    t->full_gen++;
}

static inline void wsap_vtcore_screen_scroll_up(struct wsap_vtcore *t)
{
    struct screen *s = &t->scr;
    struct srow *row = &s->slot[s->top];
    if (WSAP_VTCORE_EXPECT(screen_has_history(t), 1))
        wsap_vtcore_screen_push_row(t, row);
    else
        screen_drop_top(s, row);
    if (WSAP_VTCORE_EXPECT(t->prompt_y > 0, 0) && !t->alternate)
        t->prompt_y--;
    s->top = s->top + 1 == s->rows ? 0 : s->top + 1;
    clamp_selection_after_scroll(t);
    screen_new_row(t, row);
    t->full_gen++;
}

#endif
