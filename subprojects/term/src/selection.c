#include "screen.h"
#include "osc/osc.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include <stdlib.h>
#include <string.h>

static char *link_at_text(const struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell)
{
    int col = cell.x, row = cell.y;
    if (!t || col < 0 || col >= t->cols || row < 0 || row >= t->rows) return NULL;
    const char *link = wsap_vtcore_osc8_url_at(t, col, row);
    if (link) return wsap_vtcore_strdup(link);
    const struct cell *cells = wsap_vtcore_screen_view_row(t, row);
    if (!cells) return NULL;
    int start = col, end = col;
    while (start > 0 && cells[start - 1].c > ' ' && cells[start - 1].c < 127) start--;
    while (end < t->cols && cells[end].c > ' ' && cells[end].c < 127) end++;
    char *text = malloc((size_t)(end - start) + 1);
    if (!text) return NULL;
    int c = start;
    for (; c + 4 <= end; c += 4) {
        uint32_t packed = (cells[c].c & 0xff) |
                          ((cells[c + 1].c & 0xff) << 8) |
                          ((cells[c + 2].c & 0xff) << 16) |
                          ((cells[c + 3].c & 0xff) << 24);
        memcpy(text + (c - start), &packed, 4);
    }
    for (; c < end; c++) text[c - start] = (char)cells[c].c;
    text[end - start] = 0;
    size_t scheme = 8;
    char *url = strstr(text, "https://");
    if (!url) {
        scheme = 7;
        url = strstr(text, "http://");
        if (!url) url = strstr(text, "file://");
    }
    char *out = NULL;
    if (url && col >= start + (int)(url - text)) {
        size_t n = strlen(url);
        static const uint64_t mask[4] = {
            0xCC00528600000000ULL,
            0x2000000020000000ULL,
            0ULL,
            0ULL
        };
        while (n && (mask[(unsigned char)url[n - 1] >> 6] & (1ULL << ((unsigned char)url[n - 1] & 63))))
            url[--n] = 0;
        if (col < start + (int)(url - text) + (int)n && n > scheme)
            out = wsap_vtcore_strdup(url);
    }
    free(text);
    return out;
}

char *wsap_vtcore_link_at(const struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell)
{
    if (!t) return NULL;
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    char *text = link_at_text(t, cell);
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return text;
}

void wsap_vtcore_select_clear(struct wsap_vtcore *t)
{
    if (t) {
        wsap_vtcore_write_begin(t);
        t->selection = false;
        wsap_vtcore_write_end(t);
    }
}

// Classify codepoint into word selection classes.
// Class 0: whitespace.
// Class 1: letters, digits, and word characters.
// Class 2: punctuation and symbols.
static inline int word_class(uint32_t c)
{
    if (c <= ' ') return 0;
    if (c < 128) {
        static const uint64_t mask[2] = {
            0x03FF000000000000ULL,
            0x07FFFFFE87FFFFFEULL
        };
        return (mask[c >> 6] & (1ULL << (c & 63))) ? 1 : 2;
    }

    // Unicode whitespace (Class 0).
    if (c == 0x00A0 || c == 0x1680 || c == 0x202F || c == 0x205F ||
        c == 0x3000 || c == 0xFEFF || (c >= 0x2000 && c <= 0x200A) ||
        c == 0x2028 || c == 0x2029) {
        return 0;
    }

    // Unicode punctuation and symbols (Class 2).
    if ((c >= 0x00A1 && c <= 0x00BF) || c == 0x00D7 || c == 0x00F7 ||
        (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) ||
        (c >= 0x20A0 && c <= 0x20CF) || (c >= 0x2190 && c <= 0x2BFF) ||
        (c >= 0x3001 && c <= 0x3004) || (c >= 0x3008 && c <= 0x3020) ||
        c == 0x3030 ||
        (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFE50 && c <= 0xFE6F) ||
        (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
        (c >= 0xFF3B && c <= 0xFF3E) || c == 0xFF40 ||
        (c >= 0xFF5B && c <= 0xFF65) || (c >= 0xFFE0 && c <= 0xFFEE)) {
        return 2;
    }

    return 1;
}

static inline int sel_point_cmp(struct wsap_vtcore_sel_point a, struct wsap_vtcore_sel_point b)
{
    if (a.line_id < b.line_id) return -1;
    if (a.line_id > b.line_id) return 1;
    if (a.cell_off < b.cell_off) return -1;
    if (a.cell_off > b.cell_off) return 1;
    return 0;
}

struct lines {
    const struct wsap_vtcore *t;
    int cols;
    bool hist;
    uint64_t hist_end;
    uint64_t first;
    struct line_pos *pos;
    struct line_pos stack[256];
};

static bool lines_init(struct lines *L, const struct wsap_vtcore *t)
{
    L->t = t;
    L->cols = t->cols;
    L->hist = screen_has_history(t);
    L->hist_end = L->hist ? hist_end_id(&t->hist) : 0;
    L->first = first_retained_line(t);
    L->pos = t->rows <= 256 ? L->stack : malloc((size_t)t->rows * sizeof(*L->pos));
    if (!L->pos) {
        L->pos = L->stack;
        return false;
    }
    struct line_pos p = screen_top_line(t);
    for (int y = 0; y < t->rows; y++) {
        L->pos[y] = p;
        p = line_pos_next(p, screen_get_row(&t->scr, y));
    }
    return true;
}

static void lines_free(struct lines *L)
{
    if (L->pos != L->stack)
        free(L->pos);
}

static inline int lines_rows(const struct lines *L)
{
    return L->t->rows;
}

static inline bool in_hist(const struct lines *L, uint64_t id)
{
    return L->hist && id >= hist_first_id(&L->t->hist) && id < L->hist_end;
}

static inline bool is_open_line(const struct lines *L, uint64_t id)
{
    return L->hist && id + 1 == L->hist_end && hist_is_open(&L->t->hist);
}

static inline int hist_segs(const struct lines *L, uint64_t id)
{
    return in_hist(L, id) ? wsap_vtcore_hist_line_rows(&L->t->hist, id, L->cols) : 0;
}

static int screen_row_of(const struct lines *L, uint64_t id, int seg)
{
    int rows = lines_rows(L);
    for (int y = 0; y < rows; y++)
        if (L->pos[y].id == id && (int)L->pos[y].seg == seg)
            return y;
    return -1;
}

static int line_total_segs(const struct lines *L, uint64_t id)
{
    int segs = hist_segs(L, id);
    int rows = lines_rows(L);
    for (int y = 0; y < rows; y++)
        if (L->pos[y].id == id && (int)L->pos[y].seg + 1 > segs)
            segs = (int)L->pos[y].seg + 1;
    return segs;
}

static const struct cell *line_seg_cells(const struct lines *L, uint64_t id, int seg,
                                         struct cell *scratch, bool *out_wrapped)
{
    const struct wsap_vtcore *t = L->t;
    struct cell *buf = scratch ? scratch : t->view_row_scratch;
    int hrows = hist_segs(L, id);
    if (seg < hrows) {
        int got = wsap_vtcore_hist_decode_segment(&t->hist, id, seg, L->cols, buf, t->gstore);
        bool wrapped = seg + 1 < hrows || is_open_line(L, id);
        if (got < L->cols) {
            fill_cells(buf + got, L->cols - got, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
            if (wrapped)
                set_cells_wrap_pad(buf + got, L->cols - got);
        }
        if (out_wrapped)
            *out_wrapped = wrapped;
        return buf;
    }
    int y = screen_row_of(L, id, seg);
    if (y < 0)
        return NULL;
    const struct srow *srow = screen_get_row(&t->scr, y);
    if (out_wrapped)
        *out_wrapped = (srow->flags & RF_WRAPPED) != 0;
    if (srow->kind == ROW_CELLS) {
        if (srow->len >= L->cols || (srow->flags & RF_TAIL_PADDED))
            return srow->cells;
        if (srow->len > 0)
            memcpy(buf, srow->cells, (size_t)srow->len * sizeof(struct cell));
        fill_cells(buf + srow->len, L->cols - srow->len, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
        return buf;
    }
    text_row_cells(srow, buf, L->cols);
    return buf;
}

static bool line_has_wide(const struct lines *L, uint64_t id)
{
    const struct wsap_vtcore *t = L->t;
    if (in_hist(L, id)) {
        if (wsap_vtcore_hist_line_flags(&t->hist, id) & (HLINE_HAS_WIDE | HLINE_HAS_CLUSTER))
            return true;
        if (is_open_line(L, id) &&
            wsap_vtcore_hist_line_cells(&t->hist, id) != (uint64_t)hist_segs(L, id) * (uint64_t)L->cols)
            return true;
    }
    int rows = lines_rows(L);
    for (int y = 0; y < rows; y++) {
        if (L->pos[y].id != id)
            continue;
        const struct srow *r = screen_get_row(&t->scr, y);
        if (r->flags & RF_WIDE)
            return true;
        if (r->kind == ROW_CELLS && r->len >= L->cols && L->cols > 1 &&
            (r->cells[L->cols - 1].flags & CELL_WRAP_PAD))
            return true;
    }
    return false;
}

static inline int seg_display_cols(int cols, const struct cell *cells, bool wrapped)
{
    if (cells && wrapped && cols > 1 && (cells[cols - 1].flags & CELL_WRAP_PAD))
        return cols - 1;
    return cols;
}

static uint32_t line_seg_start_off(const struct lines *L, uint64_t id, int seg)
{
    if (seg <= 0)
        return 0;
    if (!line_has_wide(L, id))
        return (uint32_t)seg * (uint32_t)L->cols;

    const struct wsap_vtcore *t = L->t;
    int hrows = hist_segs(L, id);
    if (in_hist(L, id)) {
        if (seg <= hrows) {
            uint64_t base = id == t->hist.base_cell_id ? t->hist.base_cell : 0;
            return (uint32_t)(wsap_vtcore_hist_segment_cell(&t->hist, id, seg, L->cols) - base);
        }
    }

    uint32_t off = 0;
    if (in_hist(L, id) && hrows > 0) {
        uint64_t base = id == t->hist.base_cell_id ? t->hist.base_cell : 0;
        off = (uint32_t)(wsap_vtcore_hist_segment_cell(&t->hist, id, hrows, L->cols) - base);
    }
    struct cell stack_buf[512];
    struct cell *scratch = L->cols <= 512 ? stack_buf : malloc((size_t)L->cols * sizeof(*scratch));
    for (int s = hrows; s < seg; s++) {
        bool wrapped = false;
        const struct cell *cells = line_seg_cells(L, id, s, scratch, &wrapped);
        off += (uint32_t)seg_display_cols(L->cols, cells, wrapped);
    }
    if (scratch != stack_buf)
        free(scratch);
    return off;
}

static bool view_row_line(const struct lines *L, int y, uint64_t *id, int *seg)
{
    const struct wsap_vtcore *t = L->t;
    struct hist_view_pos pos;
    if (wsap_vtcore_hist_view_row_pos(&t->view, &t->hist, L->cols, t->rows, screen_top_line(t), y, &pos)) {
        *id = pos.id;
        *seg = pos.seg;
        return true;
    }
    if (pos.screen_y >= lines_rows(L))
        pos.screen_y = lines_rows(L) - 1;
    *id = L->pos[pos.screen_y].id;
    *seg = (int)L->pos[pos.screen_y].seg;
    return true;
}

static struct wsap_vtcore_sel_point view_to_sel_point(const struct lines *L, int x, int y)
{
    const struct wsap_vtcore *t = L->t;
    if (x < 0) x = 0;
    if (x >= L->cols) x = L->cols - 1;
    if (y < 0) y = 0;
    if (y >= t->rows) y = t->rows - 1;

    uint64_t id;
    int seg;
    if (!view_row_line(L, y, &id, &seg))
        return (struct wsap_vtcore_sel_point){ .line_id = L->first };
    const struct cell *cells = line_seg_cells(L, id, seg, NULL, NULL);
    if (cells && x > 0 && (cells[x].flags & CELL_WRAP_PAD))
        x--;
    return (struct wsap_vtcore_sel_point){
        .line_id = id,
        .cell_off = line_seg_start_off(L, id, seg) + (uint32_t)x,
    };
}

static uint32_t line_total_display_cols(const struct lines *L, uint64_t id, int nsegs)
{
    if (nsegs <= 0)
        return 0;
    uint32_t start = line_seg_start_off(L, id, nsegs - 1);
    bool wrapped = false;
    const struct cell *cells = line_seg_cells(L, id, nsegs - 1, NULL, &wrapped);
    return start + (uint32_t)seg_display_cols(L->cols, cells, wrapped);
}

static struct wsap_vtcore_sel_point sel_point_next(const struct lines *L, struct wsap_vtcore_sel_point pt)
{
    int nsegs = line_total_segs(L, pt.line_id);
    if (nsegs <= 0)
        return pt;
    if (pt.cell_off + 1 < (uint64_t)(nsegs - 1) * (L->cols > 1 ? (uint64_t)(L->cols - 1) : 1ULL) ||
        pt.cell_off + 1 < line_total_display_cols(L, pt.line_id, nsegs)) {
        pt.cell_off++;
        return pt;
    }
    uint64_t max_id = L->pos[lines_rows(L) - 1].id;
    for (uint64_t next = pt.line_id + 1; next <= max_id; next++)
        if (line_total_segs(L, next) > 0)
            return (struct wsap_vtcore_sel_point){ .line_id = next };
    pt.cell_off++;
    return pt;
}

static struct wsap_vtcore_sel_point sel_point_prev(const struct lines *L, struct wsap_vtcore_sel_point pt)
{
    if (pt.cell_off > 0) {
        pt.cell_off--;
        return pt;
    }
    for (uint64_t prev = pt.line_id; prev-- > L->first; ) {
        int nsegs = line_total_segs(L, prev);
        if (nsegs <= 0)
            continue;
        uint32_t cols = line_total_display_cols(L, prev, nsegs);
        return (struct wsap_vtcore_sel_point){ .line_id = prev, .cell_off = cols > 0 ? cols - 1 : 0 };
    }
    return pt;
}

static void unit_span(const struct lines *L, int x, int y, bool right_half,
                      enum wsap_vtcore_select_unit unit,
                      struct wsap_vtcore_sel_point *first, struct wsap_vtcore_sel_point *last)
{
    const struct wsap_vtcore *t = L->t;
    int cols = L->cols;
    if (x < 0) x = 0;
    if (x >= cols) x = cols - 1;
    if (y < 0) y = 0;
    if (y >= t->rows) y = t->rows - 1;

    if (unit == WSAP_VTCORE_SELECT_CELL) {
        struct wsap_vtcore_sel_point pt = view_to_sel_point(L, x, y);
        if (right_half)
            pt = sel_point_next(L, pt);
        *first = *last = pt;
        return;
    }
    if (unit == WSAP_VTCORE_SELECT_LINE) {
        *first = view_to_sel_point(L, 0, y);
        *last = sel_point_next(L, view_to_sel_point(L, cols - 1, y));
        return;
    }

    const struct cell *row = wsap_vtcore_screen_view_row(t, y);
    if (row && x > 0 && (row[x].flags & CELL_WRAP_PAD))
        x--;
    int left = x, right = x;
    int kind = row ? word_class(row[x].c) : 0;
    while (left > 0 && row && word_class(row[left - 1].c) == kind)
        left--;
    while (right + 1 < cols && row && !(row[right + 1].flags & CELL_WRAP_PAD) &&
           word_class(row[right + 1].c) == kind)
        right++;
    *first = view_to_sel_point(L, left, y);
    *last = sel_point_next(L, view_to_sel_point(L, right, y));
}

static bool selection_bounds(const struct lines *L, struct wsap_vtcore_sel_point *start,
                             struct wsap_vtcore_sel_point *end)
{
    const struct wsap_vtcore *t = L->t;
    if (!t->selection || t->sel_end.line_id < L->first)
        return false;
    *start = t->sel_start;
    *end = t->sel_end;
    if (start->line_id < L->first)
        *start = (struct wsap_vtcore_sel_point){ .line_id = L->first };
    return sel_point_cmp(*start, *end) <= 0;
}

static bool line_locate_cell(const struct lines *L, uint64_t id, uint32_t cell_off,
                             int *out_seg, int *out_col)
{
    int nsegs = line_total_segs(L, id);
    if (nsegs <= 0)
        return false;
    if (!line_has_wide(L, id)) {
        int seg = (int)(cell_off / (uint32_t)L->cols);
        if (seg >= nsegs)
            return false;
        if (out_seg)
            *out_seg = seg;
        if (out_col)
            *out_col = (int)(cell_off % (uint32_t)L->cols);
        return true;
    }

    int hrows = hist_segs(L, id);
    if (in_hist(L, id) && hrows > 0) {
        uint32_t last_start = line_seg_start_off(L, id, hrows - 1);
        if (cell_off >= last_start) {
            if (hrows == nsegs) {
                if (cell_off < last_start + (uint32_t)L->cols) {
                    if (out_seg)
                        *out_seg = hrows - 1;
                    if (out_col)
                        *out_col = (int)(cell_off - last_start);
                    return true;
                }
                return false;
            }
        } else {
            uint64_t base = id == L->t->hist.base_cell_id ? L->t->hist.base_cell : 0;
            int seg = wsap_vtcore_hist_cell_segment(&L->t->hist, id, base + (uint64_t)cell_off, L->cols);
            if (seg < hrows) {
                uint32_t seg_start = line_seg_start_off(L, id, seg);
                if (cell_off >= seg_start) {
                    if (out_seg)
                        *out_seg = seg;
                    if (out_col)
                        *out_col = (int)(cell_off - seg_start);
                    return true;
                }
            }
        }
    }

    int start_seg = (in_hist(L, id) && hrows > 0 && hrows < nsegs) ? hrows - 1 : hrows;
    uint32_t start = line_seg_start_off(L, id, start_seg);
    for (int seg = start_seg; seg < nsegs; seg++) {
        bool wrapped = false;
        const struct cell *cells = line_seg_cells(L, id, seg, NULL, &wrapped);
        uint32_t w = (uint32_t)seg_display_cols(L->cols, cells, wrapped);
        if (cell_off < start + w) {
            if (out_seg)
                *out_seg = seg;
            if (out_col)
                *out_col = (int)(cell_off - start);
            return true;
        }
        start += w;
    }
    return false;
}

static void select_range(struct wsap_vtcore *t, const struct lines *L,
                         struct wsap_vtcore_sel_point start, struct wsap_vtcore_sel_point end)
{
    if (sel_point_cmp(start, end) > 0) {
        t->selection = false;
        return;
    }
    int seg = 0, col = 0;
    if (line_locate_cell(L, start.line_id, start.cell_off, &seg, &col)) {
        const struct cell *cells = line_seg_cells(L, start.line_id, seg, NULL, NULL);
        if (cells && col > 0 && (cells[col].flags & CELL_TRAIL))
            start = sel_point_prev(L, start);
    }
    if (line_locate_cell(L, end.line_id, end.cell_off, &seg, &col)) {
        const struct cell *cells = line_seg_cells(L, end.line_id, seg, NULL, NULL);
        if (cells && col + 1 < L->cols && (cells[col].flags & CELL_WIDE))
            end = sel_point_next(L, end);
    }
    t->sel_start = start;
    t->sel_end = end;
    t->selection = true;
}

bool wsap_vtcore_select_start(struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell, bool right_half,
                       enum wsap_vtcore_select_unit unit)
{
    if (!t) return false;
    wsap_vtcore_write_begin(t);
    struct lines L;
    if (!lines_init(&L, t)) {
        lines_free(&L);
        wsap_vtcore_write_end(t);
        return false;
    }
    struct wsap_vtcore_sel_point first, last;
    unit_span(&L, cell.x, cell.y, right_half, unit, &first, &last);
    lines_free(&L);
    enum wsap_vtcore_select_unit old_unit = t->select_unit;
    struct wsap_vtcore_sel_point old_first = t->sel_anchor_first, old_last = t->sel_anchor_last;
    bool old_selection = t->selection;
    t->select_unit = unit;
    t->sel_anchor_first = first;
    t->sel_anchor_last = last;
    t->selection = false;
    bool ok = wsap_vtcore_select_extend(t, cell, right_half);
    if (!ok) {
        t->select_unit = old_unit;
        t->sel_anchor_first = old_first;
        t->sel_anchor_last = old_last;
        t->selection = old_selection;
    }
    wsap_vtcore_write_end(t);
    return ok;
}

bool wsap_vtcore_select_extend(struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell, bool right_half)
{
    if (!t) return false;
    wsap_vtcore_write_begin(t);
    struct lines L;
    if (!lines_init(&L, t)) {
        lines_free(&L);
        wsap_vtcore_write_end(t);
        return false;
    }
    struct wsap_vtcore_sel_point first, last;
    unit_span(&L, cell.x, cell.y, right_half, t->select_unit, &first, &last);
    if (sel_point_cmp(first, t->sel_anchor_first) > 0) first = t->sel_anchor_first;
    if (sel_point_cmp(last, t->sel_anchor_last) < 0) last = t->sel_anchor_last;
    if (sel_point_cmp(first, last) == 0)
        t->selection = false;
    else
        select_range(t, &L, first, sel_point_prev(&L, last));
    lines_free(&L);
    wsap_vtcore_write_end(t);
    return true;
}

bool wsap_vtcore_has_selection(const struct wsap_vtcore *t)
{
    if (!t)
        return false;
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    bool has = t->selection && t->sel_end.line_id >= first_retained_line(t);
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return has;
}

bool wsap_vtcore_selection_view_spans(const struct wsap_vtcore *t, int *c0, int *c1)
{
    int rows = t->rows, cols = t->cols;
    for (int y = 0; y < rows; y++) {
        c0[y] = cols;
        c1[y] = -1;
    }
    if (!t->selection)
        return true;
    struct lines L;
    if (!lines_init(&L, t)) {
        lines_free(&L);
        return false;
    }
    struct wsap_vtcore_sel_point start, end;
    if (!selection_bounds(&L, &start, &end)) {
        lines_free(&L);
        return true;
    }
    uint64_t prev_id = UINT64_MAX;
    int prev_seg = -1;
    uint32_t prev_end = 0;

    for (int y = 0; y < rows; y++) {
        uint64_t id;
        int seg;
        if (!view_row_line(&L, y, &id, &seg) || id < start.line_id || id > end.line_id)
            continue;
        uint32_t seg_start;
        if (id == prev_id && seg == prev_seg + 1)
            seg_start = prev_end;
        else
            seg_start = line_seg_start_off(&L, id, seg);

        bool wrapped = false;
        const struct cell *cells = line_seg_cells(&L, id, seg, NULL, &wrapped);
        uint32_t seg_cols = (uint32_t)seg_display_cols(cols, cells, wrapped);
        uint32_t seg_end = seg_start + seg_cols;
        prev_id = id;
        prev_seg = seg;
        prev_end = seg_end;

        uint32_t s = 0, e = (uint32_t)(cols - 1);
        if (id == start.line_id) {
            if (start.cell_off >= seg_end)
                continue;
            s = start.cell_off > seg_start ? start.cell_off - seg_start : 0;
        }
        if (id == end.line_id) {
            if (end.cell_off < seg_start)
                continue;
            if (end.cell_off < seg_end && end.cell_off - seg_start < (uint32_t)cols)
                e = end.cell_off - seg_start;
        }
        c0[y] = (int)s;
        c1[y] = (int)e;
    }
    lines_free(&L);
    return true;
}

static bool text_reserve(char **text, size_t *cap, size_t need)
{
    if (need <= *cap)
        return true;
    size_t next = *cap * 2 > need ? *cap * 2 : need;
    char *grown = realloc(*text, next);
    if (!grown) {
        free(*text);
        *text = NULL;
        return false;
    }
    *text = grown;
    *cap = next;
    return true;
}

static char *range_text_inner(const struct wsap_vtcore *t,
                              const struct wsap_vtcore_sel_point *bounds)
{
    if (!t || (!bounds && !t->selection)) return NULL;
    struct lines L;
    if (!lines_init(&L, t)) {
        lines_free(&L);
        return NULL;
    }
    struct wsap_vtcore_sel_point start, end;
    if (bounds) {
        start = bounds[0];
        end = bounds[1];
    } else if (!selection_bounds(&L, &start, &end)) {
        lines_free(&L);
        return NULL;
    }

    size_t cap = 4096, n = 0;
    char *text = malloc(cap);
    for (uint64_t id = start.line_id; text && id <= end.line_id; id++) {
        int nsegs = line_total_segs(&L, id);
        int start_seg = 0;
        if (id == start.line_id) {
            if (!line_locate_cell(&L, id, start.cell_off, &start_seg, NULL))
                start_seg = 0;
        }
        int end_seg = nsegs - 1;
        if (id == end.line_id) {
            if (!line_locate_cell(&L, id, end.cell_off, &end_seg, NULL))
                end_seg = nsegs - 1;
        }
        uint32_t next_start = line_seg_start_off(&L, id, start_seg);
        int hrows = hist_segs(&L, id);
        enum { BATCH_SEGS = 64 };
        struct cell *batch_cells = NULL;
        int batch_lens[BATCH_SEGS];
        int batch_start = -1, batch_len = 0;

        for (int seg = start_seg; text && seg <= end_seg && seg < nsegs; seg++) {
            uint32_t seg_start = next_start;
            bool row_wrapped = false;
            const struct cell *cells = NULL;
            if (seg < hrows) {
                if (seg >= batch_start + batch_len) {
                    batch_start = seg;
                    int take = hrows - seg;
                    if (take > BATCH_SEGS)
                        take = BATCH_SEGS;
                    if (seg + take - 1 > end_seg)
                        take = end_seg - seg + 1;
                    if (!batch_cells)
                        batch_cells = malloc((size_t)BATCH_SEGS * (size_t)t->cols * sizeof(struct cell));
                    batch_len = batch_cells ? wsap_vtcore_hist_decode_segments(&t->hist, id, batch_start, take,
                                                                   t->cols, batch_cells, batch_lens,
                                                                   t->gstore) : 0;
                }
                if (batch_cells && seg >= batch_start && seg < batch_start + batch_len) {
                    int idx = seg - batch_start;
                    struct cell *row_buf = batch_cells + (size_t)idx * (size_t)t->cols;
                    int got = batch_lens[idx];
                    row_wrapped = seg + 1 < hrows || is_open_line(&L, id);
                    if (got < t->cols) {
                        fill_cells(row_buf + got, t->cols - got, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
                        if (row_wrapped)
                            set_cells_wrap_pad(row_buf + got, t->cols - got);
                    }
                    cells = row_buf;
                }
            }
            if (!cells)
                cells = line_seg_cells(&L, id, seg, NULL, &row_wrapped);
            if (!cells) {
                next_start = seg_start + (uint32_t)t->cols;
                continue;
            }
            uint32_t seg_end = seg_start + (uint32_t)seg_display_cols(t->cols, cells, row_wrapped);
            next_start = seg_end;

            uint32_t s = 0, e = (uint32_t)(t->cols - 1);
            if (id == start.line_id) {
                if (start.cell_off >= seg_end)
                    continue;
                s = start.cell_off > seg_start ? start.cell_off - seg_start : 0;
            }
            if (id == end.line_id) {
                if (end.cell_off < seg_start)
                    continue;
                if (end.cell_off < seg_end && end.cell_off - seg_start < (uint32_t)t->cols)
                    e = end.cell_off - seg_start;
            }
            int c0 = (int)s, c1 = (int)e;
            if (c1 < c0)
                continue;
            bool is_last = id == end.line_id && (end.cell_off < seg_end || seg == nsegs - 1);
            bool wrapped = !is_last && row_wrapped;
            if (!wrapped)
                while (c1 >= c0 && cells[c1].c <= ' ')
                    c1--;

            if (!text_reserve(&text, &cap, n + (size_t)(c1 - c0 + 1) + 1))
                break;
            int c = c0;
            for (; c + 3 <= c1; c += 4) {
#if defined(__aarch64__)
                uint32x4x4_t block = vld4q_u32((const uint32_t *)&cells[c]);
                uint32x4_t cp = block.val[0];
                uint32x4_t valid = vandq_u32(
                    vcltq_u32(vsubq_u32(cp, vdupq_n_u32(1)), vdupq_n_u32(127)),
                    vceqq_u32(vandq_u32(block.val[3],
                        vdupq_n_u32(CELL_TRAIL | CELL_WRAP_PAD)), vdupq_n_u32(0)));
                if (!vminvq_u32(valid)) break;
                uint16x4_t half = vmovn_u32(cp);
                uint8x8_t bytes = vmovn_u16(vcombine_u16(half, half));
                uint32_t packed = vget_lane_u32(vreinterpret_u32_u8(bytes), 0);
#else
                uint32_t a = cells[c].c, b = cells[c + 1].c;
                uint32_t d = cells[c + 2].c, f = cells[c + 3].c;
                unsigned flags = (unsigned)cells[c].flags | cells[c + 1].flags |
                                 cells[c + 2].flags | cells[c + 3].flags;
                if ((a - 1) >= 127 || (b - 1) >= 127 || (d - 1) >= 127 || (f - 1) >= 127 ||
                    (flags & (CELL_TRAIL | CELL_WRAP_PAD))) break;
                uint32_t packed = a | (b << 8) | (d << 16) | (f << 24);
#endif
                memcpy(text + n, &packed, 4);
                n += 4;
            }
            for (; c <= c1; c++) {
                uint32_t cp = cells[c].c;
                if (cells[c].flags & (CELL_TRAIL | CELL_WRAP_PAD))
                    continue;
                if (!text_reserve(&text, &cap, n + (size_t)WSAP_VTCORE_GRAPHEME_MAX_LEN * 4 + 1))
                    break;
                if (cp > 0 && cp < 128) {
                    text[n++] = (char)cp;
                } else {
                    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
                    int len = wsap_vtcore_gstore_expand(t->gstore, cp ? cp : ' ', cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
                    for (int k = 0; k < len; k++)
                        n += (size_t)wsap_vtcore_utf8_encode(cps[k], text + n);
                }
            }
            if (text && !is_last && !wrapped && text_reserve(&text, &cap, n + 2))
                text[n++] = '\n';
        }
        free(batch_cells);
    }
    if (text)
        text[n] = 0;
    lines_free(&L);
    return text;
}

char *wsap_vtcore_history_text(struct wsap_vtcore *t, int max_rows)
{
    if (!t)
        return NULL;
    wsap_vtcore_write_begin(t);
    wsap_vtcore_hist_prepare_rows(&t->hist, t->cols);
    if (!wsap_vtcore_hist_view_sync(&t->view, &t->hist, t->cols)) {
        wsap_vtcore_write_end(t);
        return NULL;
    }
    int end_row = wsap_vtcore_screen_history_rows(t) - wsap_vtcore_view_offset(t);
    if (max_rows <= 0 || end_row <= 0) {
        wsap_vtcore_write_end(t);
        return wsap_vtcore_strdup("");
    }
    int begin_row = end_row > max_rows ? end_row - max_rows : 0;
    struct lines L;
    if (!lines_init(&L, t)) {
        lines_free(&L);
        wsap_vtcore_write_end(t);
        return NULL;
    }
    struct wsap_vtcore_sel_point bounds[2] = {0};
    int first_seg, last_seg;
    char *text = NULL;
    if (wsap_vtcore_hist_view_find_row(&t->view, &t->hist, begin_row, t->cols,
                           &bounds[0].line_id, &first_seg) &&
        wsap_vtcore_hist_view_find_row(&t->view, &t->hist, end_row - 1, t->cols,
                           &bounds[1].line_id, &last_seg)) {
        bounds[0].cell_off = line_seg_start_off(&L, bounds[0].line_id, first_seg);
        uint32_t last_start = line_seg_start_off(&L, bounds[1].line_id, last_seg);
        bool wrapped = false;
        const struct cell *cells = line_seg_cells(&L, bounds[1].line_id,
                                                   last_seg, NULL, &wrapped);
        bounds[1].cell_off = last_start +
            (uint32_t)seg_display_cols(t->cols, cells, wrapped) - 1;
        text = range_text_inner(t, bounds);
        if (text && !wrapped) {
            size_t n = strlen(text);
            size_t cap = n + 1;
            if (text_reserve(&text, &cap, n + 2)) {
                text[n] = '\n';
                text[n + 1] = '\0';
            }
        }
    }
    lines_free(&L);
    wsap_vtcore_write_end(t);
    return text;
}

char *wsap_vtcore_selection_text(const struct wsap_vtcore *t)
{
    if (!t) return NULL;
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    char *text = range_text_inner(t, NULL);
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return text;
}
