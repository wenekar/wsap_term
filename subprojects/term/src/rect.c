/* Rectangle and column edits: DECFRA, DECERA, DECCRA, and the column shifts
 * of DECIC, DECDC, SL, SR, DECBI, and DECFI.
 */
#include "rect.h"
#include "core.h"
#include "screen.h"

static inline int area_arg(const struct csi *csi, int i)
{
    return i < csi->nparams ? csi->params[i] : 0;
}

static inline int area_min_row(const struct wsap_vtcore *t)
{
    return t->origin_mode ? t->scroll_top : 0;
}

static inline int area_max_row(const struct wsap_vtcore *t)
{
    return t->origin_mode ? t->scroll_bottom : t->rows - 1;
}

static inline int area_min_col(const struct wsap_vtcore *t)
{
    return t->origin_mode && t->lr_margin_mode ? t->left_margin : 0;
}

static inline int area_max_col(const struct wsap_vtcore *t)
{
    int right = t->origin_mode && t->lr_margin_mode ? t->right_margin : t->cols - 1;
    return right < t->cols - 1 ? right : t->cols - 1;
}

bool wsap_vtcore_area_parse(const struct wsap_vtcore *t, const struct csi *csi, int first,
                            struct area *a)
{
    int row0 = area_min_row(t), row1 = area_max_row(t);
    int col0 = area_min_col(t), col1 = area_max_col(t);
    int64_t top = row0, left = col0, bottom = row1, right = col1;
    int v;
    if ((v = area_arg(csi, first)) > 0)
        top = (int64_t)row0 + v - 1;
    if ((v = area_arg(csi, first + 1)) > 0)
        left = (int64_t)col0 + v - 1;
    if ((v = area_arg(csi, first + 2)) > 0 && (int64_t)row0 + v - 1 < bottom)
        bottom = (int64_t)row0 + v - 1;
    if ((v = area_arg(csi, first + 3)) > 0 && (int64_t)col0 + v - 1 < right)
        right = (int64_t)col0 + v - 1;
    if (top > bottom || left > right)
        return false;
    *a = (struct area){ .top = (int)top, .left = (int)left, .bottom = (int)bottom, .right = (int)right };
    return true;
}

void wsap_vtcore_area_row_span(const struct wsap_vtcore *t, const struct area *a, int y,
                               int *x0, int *x1)
{
    bool stream = t->decsace != 2;
    *x0 = !stream || y == a->top ? a->left : area_min_col(t);
    *x1 = !stream || y == a->bottom ? a->right : area_max_col(t);
}

void wsap_vtcore_sc_decfra(struct wsap_vtcore *t, const struct csi *csi)
{
    /* DEC allows the printable characters of GL and GR only. */
    int ch = area_arg(csi, 0);
    if (!((ch >= 32 && ch <= 126) || (ch >= 160 && ch <= 255)))
        return;
    struct area a;
    if (!wsap_vtcore_area_parse(t, csi, 1, &a))
        return;
    /* The fill takes the SGR attributes, but not the hyperlink. */
    struct cell fill = {
        .c = (uint32_t)ch,
        .fg = t->inverse ? t->cur_bg : t->cur_fg,
        .bg = t->inverse ? t->cur_fg : t->cur_bg,
        .flags = (uint16_t)(wsap_vtcore_current_flags(t) & ~CELL_LINK),
    };
    for (int y = a.top; y <= a.bottom; y++) {
        int p = phys_row(t, y);
        struct cell *cells = row_cells(t, p);
        struct srow *row = &t->scr.slot[p];
        clear_wide_edges(cells, a.left, a.right + 1, row->len);
        wsap_vtcore_row_fill_gap(row, a.left);
        for (int x = a.left; x <= a.right; x++)
            cells[x] = fill;
        if (row->len < a.right + 1)
            row->len = (uint16_t)(a.right + 1);
        row->version++;
    }
}

void wsap_vtcore_sc_decera(struct wsap_vtcore *t, const struct csi *csi)
{
    struct area a;
    if (!wsap_vtcore_area_parse(t, csi, 0, &a))
        return;
    /* Each row is erased as ECH erases it. A wide character cut by an edge
     * loses both halves. */
    for (int y = a.top; y <= a.bottom; y++) {
        int p = phys_row(t, y);
        erase_span(t, p, a.left, a.right + 1);
        t->scr.slot[p].version++;
    }
}

/* Write the cells of src to columns x0 to x1 of a row. The source has ext
 * cells. The cells after them read as blank, because the source row ends
 * before them. The row length grows only over written cells, and shrinks when
 * the blank cells reach the end of the row.
 */
static void area_put_cells(struct srow *dst, int x0, int x1, const struct cell *src, int ext)
{
    int old_len = dst->len;
    if (ext > 0) {
        wsap_vtcore_row_fill_gap(dst, x0);
        memcpy(dst->cells + x0, src, (size_t)ext * sizeof(struct cell));
    }
    int clear_end = old_len < x1 + 1 ? old_len : x1 + 1;
    if (x0 + ext < clear_end)
        clear_cells_default(dst->cells + x0 + ext, clear_end - (x0 + ext));
    if (old_len <= x1 + 1) {
        int keep = old_len < x0 ? old_len : x0;
        dst->len = (uint16_t)(ext > 0 ? x0 + ext : keep);
    }
}

static void deccra_row(struct wsap_vtcore *t, int sy, int sx, int dy, int dx, int width)
{
    /* Copy the source cells first. The source can be the destination row. */
    struct cell *tmp = t->view_row_scratch;
    int ps = phys_row(t, sy);
    const struct cell *src = row_cells(t, ps);
    const struct srow *srow = &t->scr.slot[ps];
    int ext = srow->len - sx;
    if (ext > width) ext = width;
    if (ext < 0) ext = 0;
    if (ext > 0)
        memcpy(tmp, src + sx, (size_t)ext * sizeof(struct cell));
    uint8_t content = srow->flags & (RF_WIDE | RF_CLUSTER);

    int pd = phys_row(t, dy);
    struct cell *dst = row_cells(t, pd);
    struct srow *drow = &t->scr.slot[pd];
    int old_len = drow->len;
    clear_wide_edges(dst, dx, dx + width, old_len);
    area_put_cells(drow, dx, dx + width - 1, tmp, ext);
    if (ext > 0)
        drow->flags |= content;
    /* A row that now ends earlier does not continue on the next row. */
    if (drow->len < old_len)
        row_set_wrapped(drow, false);
    /* A wide character cut by a source edge loses its other half. */
    repair_wide_row(dst, dx - 1, dx + width + 1, t->cols, drow->len);
    drow->version++;
}

void wsap_vtcore_sc_deccra(struct wsap_vtcore *t, const struct csi *csi)
{
    struct area src;
    if (!wsap_vtcore_area_parse(t, csi, 0, &src))
        return;
    /* Parameters 4 and 7 select pages. The core has one page. */
    int row1 = area_max_row(t), col1 = area_max_col(t);
    int64_t top = area_min_row(t), left = area_min_col(t);
    int v;
    if ((v = area_arg(csi, 5)) > 0)
        top += v - 1;
    if ((v = area_arg(csi, 6)) > 0)
        left += v - 1;
    if (top > row1 || left > col1)
        return;
    /* The destination keeps the size of the source, clipped like it. */
    int height = src.bottom - src.top + 1, width = src.right - src.left + 1;
    if (height > row1 - top + 1)
        height = row1 - (int)top + 1;
    if (width > col1 - left + 1)
        width = col1 - (int)left + 1;
    /* Copy the rows in an order that reads each source row before a copy
     * writes it. */
    bool up = top > src.top;
    for (int i = 0; i < height; i++) {
        int k = up ? height - 1 - i : i;
        deccra_row(t, src.top + k, src.left, (int)top + k, (int)left, width);
    }
}

void wsap_vtcore_screen_shift_cols(struct wsap_vtcore *t, int x, int count, bool insert)
{
    /* Each row changes as ICH or DCH at column x changes it. */
    int cx = t->cursor_x, cy = t->cursor_y;
    for (int y = t->scroll_top; y <= t->scroll_bottom; y++) {
        t->cursor_x = x;
        t->cursor_y = y;
        if (insert)
            wsap_vtcore_screen_insert_chars(t, count);
        else
            wsap_vtcore_screen_delete_chars(t, count);
    }
    t->cursor_x = cx;
    t->cursor_y = cy;
    t->pending_wrap = false;
}
