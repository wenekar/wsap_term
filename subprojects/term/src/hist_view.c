#include "hist_view.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void wsap_vtcore_hist_view_free(struct hist_view *v)
{
    if (!v)
        return;
    free(v->fenwick);
    free(v->counts);
    *v = (struct hist_view){0};
}

static uint64_t prefix(const struct hist_view *v, uint32_t n)
{
    uint64_t sum = 0;
    for (; n; n &= n - 1)
        sum += v->fenwick[n];
    return sum;
}

static void set_count(struct hist_view *v, uint64_t id, uint32_t count)
{
    uint32_t slot = (uint32_t)id & (v->cap - 1);
    int64_t delta = (int64_t)count - v->counts[slot];
    if (!delta)
        return;
    v->counts[slot] = count;
    v->total_rows = (uint64_t)((int64_t)v->total_rows + delta);
    for (uint32_t i = slot + 1; i <= v->cap; i += i & -i)
        v->fenwick[i] = (uint64_t)((int64_t)v->fenwick[i] + delta);
}

static bool view_matches(const struct hist_view *v, const struct hist *h, int cols)
{
    return v->index_cols == cols && v->epoch == h->index_epoch &&
           v->first_id == hist_first_id(h) && v->end_id == hist_end_id(h) &&
           v->head == h->head && v->tail == h->tail &&
           v->last_cells == (h->lines ? wsap_vtcore_hist_line_cells(h, hist_end_id(h) - 1) : 0);
}

static bool current(const struct hist_view *v, const struct hist *h, int cols)
{
    return v->indexed && v->cap && v->fenwick && v->counts && view_matches(v, h, cols);
}

bool wsap_vtcore_hist_view_sync(struct hist_view *v, const struct hist *h, int cols)
{
    if (current(v, h, cols))
        return true;
    uint64_t first = hist_first_id(h), end = hist_end_id(h);
    bool rebuild = !v->indexed || v->index_cols != cols || v->epoch != h->index_epoch ||
                   first < v->first_id || first >= v->end_id || end < v->end_id;
    if (!rebuild) {
        uint64_t changed = first - v->first_id + end - v->end_id;
        rebuild = changed > v->cap / 4;
    }
    if (h->lines > v->cap || !v->cap) {
        if (h->lines > (UINT32_C(1) << 31))
            return false;
        uint32_t cap = wsap_vtcore_next_pow2_32(h->lines < 16 ? 16 : h->lines);
        if (cap == UINT32_MAX)
            return false;
        uint64_t *tree = calloc((size_t)cap + 1, sizeof(*tree));
        uint32_t *counts = calloc(cap, sizeof(*counts));
        if (!tree || !counts) {
            free(tree);
            free(counts);
            return false;
        }
        free(v->fenwick);
        free(v->counts);
        v->fenwick = tree;
        v->counts = counts;
        v->cap = cap;
        rebuild = true;
    }
    if (rebuild) {
        memset(v->counts, 0, (size_t)v->cap * sizeof(*v->counts));
        memset(v->fenwick, 0, ((size_t)v->cap + 1) * sizeof(*v->fenwick));
        for (uint64_t id = first; id < end; id++)
            v->counts[(uint32_t)id & (v->cap - 1)] = (uint32_t)wsap_vtcore_hist_line_estimate(h, id, cols);
        for (uint32_t i = 1; i <= v->cap; i++) {
            v->fenwick[i] += v->counts[i - 1];
            uint32_t parent = i + (i & -i);
            if (parent <= v->cap)
                v->fenwick[parent] += v->fenwick[i];
        }
        v->total_rows = v->fenwick[v->cap];
    } else {
        for (uint64_t id = v->first_id; id < first; id++)
            set_count(v, id, 0);
        if (v->end_id > first && wsap_vtcore_hist_line_cells(h, v->end_id - 1) != v->last_cells)
            set_count(v, v->end_id - 1, (uint32_t)wsap_vtcore_hist_line_estimate(h, v->end_id - 1, cols));
        for (uint64_t id = v->end_id; id < end; id++)
            set_count(v, id, (uint32_t)wsap_vtcore_hist_line_estimate(h, id, cols));
    }
    v->first_id = first;
    v->end_id = end;
    v->index_cols = cols;
    v->epoch = h->index_epoch;
    v->head = h->head;
    v->tail = h->tail;
    v->last_cells = h->lines ? wsap_vtcore_hist_line_cells(h, end - 1) : 0;
    v->indexed = true;
    return true;
}

static uint64_t anchor_extra(const struct hist_view *v, const struct hist *h, int cols)
{
    uint64_t id = v->anchor.line_id;
    if (!v->anchor.active || id < hist_first_id(h) || id >= hist_end_id(h))
        return 0;
    uint64_t estimated = current(v, h, cols) ? v->counts[(uint32_t)id & (v->cap - 1)] :
                         wsap_vtcore_hist_line_estimate(h, id, cols);
    uint64_t exact = (uint64_t)wsap_vtcore_hist_line_rows(h, id, cols);
    return exact > estimated ? exact - estimated : 0;
}

uint64_t wsap_vtcore_hist_view_rows(const struct hist_view *v, const struct hist *h, int cols)
{
    uint64_t rows = view_matches(v, h, cols) ? v->total_rows : wsap_vtcore_hist_estimated_rows(h, cols);
    return rows + anchor_extra(v, h, cols);
}

static uint64_t before(const struct hist_view *v, uint64_t id)
{
    if (id <= v->first_id)
        return 0;
    uint32_t start = (uint32_t)v->first_id & (v->cap - 1);
    uint32_t stop = start + (uint32_t)(id - v->first_id);
    uint64_t p_start = prefix(v, start);
    if (stop <= v->cap)
        return prefix(v, stop) - p_start;
    return v->total_rows - p_start + prefix(v, stop - v->cap);
}

static uint32_t select_slot(const struct hist_view *v, uint64_t row, int *seg)
{
    uint32_t slot = 0;
    for (uint32_t bit = v->cap >> 1; bit; bit >>= 1) {
        uint32_t next = slot + bit;
        if (v->fenwick[next] <= row) {
            row -= v->fenwick[next];
            slot = next;
        }
    }
    *seg = (int)row;
    return slot;
}

bool wsap_vtcore_hist_view_find_row(const struct hist_view *v, const struct hist *h, int row,
                        int cols, uint64_t *id, int *seg)
{
    if (!current(v, h, cols)) {
        if (row < 0)
            return false;
        uint64_t target = (uint64_t)row;
        for (uint64_t line = hist_first_id(h); line < hist_end_id(h); line++) {
            uint64_t count = wsap_vtcore_hist_line_estimate(h, line, cols);
            if (target < count) {
                *id = line;
                *seg = (int)target;
                return true;
            }
            target -= count;
        }
        return false;
    }
    uint64_t total = v->total_rows;
    if (row < 0 || (uint64_t)row >= total)
        return false;
    uint32_t start = (uint32_t)v->first_id & (v->cap - 1);
    uint64_t low = prefix(v, start), high = total - low;
    uint64_t physical = (uint64_t)row < high ? low + row : (uint64_t)row - high;
    uint32_t slot = select_slot(v, physical, seg);
    *id = v->first_id + ((slot - start) & (v->cap - 1));
    return true;
}

static int rows_as_int(uint64_t rows)
{
    return rows > INT_MAX ? INT_MAX : (int)rows;
}

int wsap_vtcore_hist_view_scroll_offset(const struct hist_view *v, const struct hist *h, int cols)
{
    if (!v->anchor.active || !h->lines)
        return 0;
    uint64_t id = v->anchor.line_id;
    if (id < hist_first_id(h))
        return rows_as_int(wsap_vtcore_hist_view_rows(v, h, cols));
    if (id >= hist_end_id(h))
        return 0;
    int seg = v->cols == cols ? (int)v->anchor.seg :
              wsap_vtcore_hist_cell_segment(h, id, v->anchor.cell, cols);
    uint64_t rows = 0;
    if (current(v, h, cols))
        rows = v->total_rows - before(v, id);
    else
        for (; id < hist_end_id(h); id++)
            rows += wsap_vtcore_hist_line_estimate(h, id, cols);
    rows += anchor_extra(v, h, cols);
    return rows > (uint64_t)seg ? rows_as_int(rows - seg) : 0;
}

void wsap_vtcore_hist_view_note_push(struct hist_view *v, const struct hist *h, int cols)
{
    if (!v->anchor.active)
        return;
    if (v->anchor.line_id < hist_first_id(h))
        v->anchor = (struct view_anchor){ .line_id = hist_first_id(h), .active = true };
    uint64_t base = wsap_vtcore_hist_segment_cell(h, v->anchor.line_id, 0, cols);
    if (v->anchor.base_cell != base) {
        if (v->anchor.cell < base)
            v->anchor.cell = base;
        v->anchor.seg = (uint32_t)wsap_vtcore_hist_cell_segment(h, v->anchor.line_id, v->anchor.cell, cols);
        v->anchor.base_cell = base;
    }
}

void wsap_vtcore_hist_view_rewrap(struct hist_view *v, const struct hist *h, int cols)
{
    wsap_vtcore_hist_view_note_push(v, h, cols);
    if (v->anchor.active) {
        if (v->anchor.line_id >= hist_end_id(h))
            v->anchor = (struct view_anchor){0};
        else
            v->anchor.seg = (uint32_t)wsap_vtcore_hist_cell_segment(h, v->anchor.line_id, v->anchor.cell, cols);
    }
    v->cols = cols;
    v->index_cols = 0;
}

void wsap_vtcore_hist_view_scroll_to(struct hist_view *v, const struct hist *h, int cols,
                         struct line_pos screen_top, int offset)
{
    (void)screen_top;
    if (offset <= 0 || cols <= 0 || !h->lines) {
        v->anchor = (struct view_anchor){0};
        return;
    }
    uint64_t total = wsap_vtcore_hist_view_rows(v, h, cols);
    if ((uint64_t)offset > total)
        offset = rows_as_int(total);
    uint64_t id;
    int seg;
    uint64_t row = total - offset;
    if (row > INT_MAX || !wsap_vtcore_hist_view_find_row(v, h, (int)row, cols, &id, &seg))
        return;
    v->anchor = (struct view_anchor){ .line_id = id, .seg = (uint32_t)seg,
        .cell = wsap_vtcore_hist_segment_cell(h, id, seg, cols),
        .base_cell = wsap_vtcore_hist_segment_cell(h, id, 0, cols), .active = true };
    v->cols = cols;
}

void wsap_vtcore_hist_view_scroll(struct hist_view *v, const struct hist *h, int cols,
                      struct line_pos screen_top, int lines)
{
    (void)screen_top;
    if (!lines || cols <= 0 || !h->lines)
        return;
    uint64_t id;
    int64_t seg;
    if (v->anchor.active) {
        id = v->anchor.line_id;
        if (id < hist_first_id(h))
            id = hist_first_id(h);
        if (id >= hist_end_id(h))
            id = hist_end_id(h) - 1;
        seg = v->cols == cols ? (int64_t)v->anchor.seg : wsap_vtcore_hist_cell_segment(h, id, v->anchor.cell, cols);
    } else {
        if (lines < 0)
            return;
        id = hist_end_id(h) - 1;
        seg = wsap_vtcore_hist_line_rows(h, id, cols);
    }
    seg -= (int64_t)lines;
    if (lines > 0) {
        while (seg < 0 && id > hist_first_id(h))
            seg += wsap_vtcore_hist_line_rows(h, --id, cols);
        if (seg < 0)
            seg = 0;
    } else {
        for (;;) {
            int count = wsap_vtcore_hist_line_rows(h, id, cols);
            if (seg < count)
                break;
            seg -= count;
            if (++id == hist_end_id(h)) {
                v->anchor = (struct view_anchor){0};
                return;
            }
        }
    }
    v->anchor = (struct view_anchor){ .line_id = id, .seg = (uint32_t)seg,
        .cell = wsap_vtcore_hist_segment_cell(h, id, (int)seg, cols),
        .base_cell = wsap_vtcore_hist_segment_cell(h, id, 0, cols), .active = true };
    v->cols = cols;
}

bool wsap_vtcore_hist_view_row_pos(const struct hist_view *v, const struct hist *h, int cols,
                       int screen_rows, struct line_pos screen_top, int y,
                       struct hist_view_pos *pos)
{
    (void)screen_top;
    if (v->anchor.active && h->lines) {
        uint64_t id = v->anchor.line_id;
        int seg = v->cols == cols ? (int)v->anchor.seg :
                  wsap_vtcore_hist_cell_segment(h, id, v->anchor.cell, cols);
        if (id < hist_first_id(h)) {
            id = hist_first_id(h);
            seg = 0;
        }
        int64_t target = (int64_t)seg + y;
        while (id < hist_end_id(h)) {
            int count = wsap_vtcore_hist_line_rows(h, id, cols);
            if (target < count) {
                pos->id = id;
                pos->seg = (int)target;
                return true;
            }
            target -= count;
            id++;
        }
        pos->screen_y = target < screen_rows ? (int)target : screen_rows - 1;
        return false;
    }
    pos->screen_y = y;
    return false;
}

void wsap_vtcore_hist_view_refine(struct hist_view *v, struct hist *h, int cols, int rows)
{
    if (!v->anchor.active || !current(v, h, cols))
        return;
    uint64_t id = v->anchor.line_id;
    int remaining = rows;
    int seg = (int)v->anchor.seg;
    if (id < hist_first_id(h)) {
        id = hist_first_id(h);
        seg = 0;
    }
    for (; id < hist_end_id(h) && remaining > 0; id++) {
        int count = wsap_vtcore_hist_line_rows(h, id, cols);
        set_count(v, id, (uint32_t)count);
        wsap_vtcore_hist_refine_rows(h, id, cols, count);
        remaining -= count - seg;
        seg = 0;
    }
}
