#include "screen.h"
#include "osc/osc.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *grow(struct grow_buf *b, size_t bytes)
{
    if (bytes > b->cap) {
        size_t cap = wsap_vtcore_grow_cap(b->cap ? b->cap : 256, bytes);
        void *data = realloc(b->data, cap);
        if (!data)
            return NULL;
        b->data = data;
        b->cap = cap;
    }
    return b->data;
}

static bool screen_setup(struct screen *s, int cols, int rows, uint64_t *next_uid)
{
    int stride = (cols + 15) & ~15;
    size_t text = TEXT_ROW_BYTES(stride), runs = TEXT_ROW_RUNS(stride);
    size_t ascii = (size_t)rows * (size_t)cols;
    size_t cells = (size_t)rows * (size_t)stride, bytes = ascii + (size_t)rows * (text + runs);
    struct srow *slot = rows > s->slot_cap ? calloc((size_t)rows, sizeof(*slot)) : s->slot;
    struct cell *cells_slab = cells > s->cells_cap ? calloc(cells, sizeof(*cells_slab)) : s->cells_slab;
    uint8_t *text_slab = bytes > s->text_cap ? calloc(bytes, 1) : s->text_slab;
    if (!slot || !cells_slab || !text_slab) {
        if (slot != s->slot) free(slot);
        if (cells_slab != s->cells_slab) free(cells_slab);
        if (text_slab != s->text_slab) free(text_slab);
        return false;
    }
    if (slot != s->slot) {
        free(s->slot);
        s->slot = slot;
        s->slot_cap = rows;
    }
    if (cells_slab != s->cells_slab) {
        free(s->cells_slab);
        s->cells_slab = cells_slab;
        s->cells_cap = cells;
    }
    if (text_slab != s->text_slab) {
        free(s->text_slab);
        s->text_slab = text_slab;
        s->text_cap = bytes;
    }
    fill_cells(s->cells_slab, (int)cells, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
    s->rows = rows;
    s->cols = cols;
    s->top = 0;
    s->stride = stride;
    s->top_id = 0;
    s->top_seg = 0;
    for (int i = 0; i < rows; i++) {
        struct srow *r = &s->slot[i];
        *r = (struct srow){
            .cells = s->cells_slab + (size_t)i * stride,
            .ascii = s->text_slab + (size_t)i * cols,
            .text = {
                .text = s->text_slab + ascii + (size_t)i * (text + runs),
                .storage = s->text_slab + ascii + (size_t)i * (text + runs),
                .runs = { .data = s->text_slab + ascii + (size_t)i * (text + runs) + text,
                          .cap = (uint32_t)runs },
            },
            .uid = ++(*next_uid),
            .version = 1,
            .kind = ROW_CELLS,
            .flags = RF_TAIL_PADDED,
        };
    }
    return true;
}

static void screen_push_row_inner(struct wsap_vtcore *t, struct srow *row)
{
    srow_materialize_text(row);
    bool wrapped = (row->flags & RF_WRAPPED) != 0;
    int cols = t->scr.cols;
    if (row->kind == ROW_TEXT && !(wrapped && row->len < cols)) {
        struct text_row *tr = &row->text;
        int fill = (tr->fill_to < cols ? tr->fill_to : cols) - tr->cells;
        if (fill > 0) {
            text_row_unshare(tr);
            memset(tr->text + tr->text_len, ' ', (size_t)fill);
            text_commit(row, fill, fill, fill, HLINE_NARROW, (struct text_pen){ tr->fill.fg, tr->fill.bg, 0 });
        }
        wsap_vtcore_hist_push_text(&t->hist, tr, wrapped);
        return;
    }
    if (WSAP_VTCORE_EXPECT(wrapped && row->len < cols, 0)) {
        srow_settle(row, cols);
        if (!(row->flags & RF_TAIL_PADDED))
            fill_cells(row->cells + row->len, cols - row->len, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
        row->flags |= RF_TAIL_PADDED;
        row->len = (uint16_t)cols;
    }
    wsap_vtcore_hist_push_row(&t->hist, row->cells, row->len, wrapped, t->gstore);
}

void wsap_vtcore_screen_push_row(struct wsap_vtcore *t, struct srow *row)
{
    screen_push_row_inner(t, row);
    if (WSAP_VTCORE_EXPECT(t->view.anchor.active, 0))
        wsap_vtcore_hist_view_note_push(&t->view, &t->hist, t->cols);
}

static void screen_free(struct screen *s)
{
    if (!s)
        return;
    free(s->slot);
    free(s->cells_slab);
    free(s->text_slab);
    *s = (struct screen){ 0 };
}

int wsap_vtcore_screen_history_rows(const struct wsap_vtcore *t)
{
    if (!t || !screen_has_history(t))
        return 0;
    uint64_t rows = wsap_vtcore_hist_view_rows(&t->view, &t->hist, t->cols);
    return rows > INT_MAX ? INT_MAX : (int)rows;
}

int wsap_vtcore_history_rows(const struct wsap_vtcore *t)
{
    if (!t)
        return 0;
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    int rows = wsap_vtcore_screen_history_rows(t);
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return rows;
}

const struct cell *wsap_vtcore_screen_view_row(const struct wsap_vtcore *t, int y)
{
    if (!t || y < 0 || y >= t->rows)
        return NULL;
    struct hist_view_pos pos;
    if (wsap_vtcore_hist_view_row_pos(&t->view, &t->hist, t->cols, t->rows, screen_top_line(t), y, &pos)) {
        struct cell *out = t->view_row_scratch;
        int got = wsap_vtcore_hist_decode_segment(&t->hist, pos.id, pos.seg, t->cols, out, t->gstore);
        if (got < t->cols) {
            fill_cells(out + got, t->cols - got, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
            int line_rows = wsap_vtcore_hist_line_rows(&t->hist, pos.id, t->cols);
            if (pos.seg + 1 < line_rows || (pos.id + 1 == hist_end_id(&t->hist) && hist_is_open(&t->hist)))
                set_cells_wrap_pad(out + got, t->cols - got);
        }
        return out;
    }
    struct srow *srow = screen_row_at(&t->scr, pos.screen_y);
    if (!srow)
        return NULL;
    struct cell *out = t->view_row_scratch;
    if (srow->kind == ROW_CELLS) {
        if (srow->len >= t->cols || (srow->flags & RF_TAIL_PADDED))
            return srow->cells;
        if (srow->len > 0)
            memcpy(out, srow->cells, (size_t)srow->len * sizeof(struct cell));
        fill_cells(out + srow->len, t->cols - srow->len, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
        return out;
    }
    text_row_cells(srow, out, t->cols);
    return out;
}

static inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

uint64_t wsap_vtcore_screen_view_row_key(const struct wsap_vtcore *t, int y)
{
    if (!t || y < 0 || y >= t->rows)
        return 0;
    struct hist_view_pos pos;
    if (wsap_vtcore_hist_view_row_pos(&t->view, &t->hist, t->cols, t->rows, screen_top_line(t), y, &pos)) {
        uint64_t line_ver = (pos.id + 1 == hist_end_id(&t->hist) && hist_is_open(&t->hist))
            ? (wsap_vtcore_hist_line_cells(&t->hist, pos.id) << 32) : 0;
        if (pos.id == t->hist.base_cell_id)
            line_ver ^= mix64(t->hist.base_cell);
        return mix64(((uint64_t)pos.id << 16) ^ (uint64_t)pos.seg ^ line_ver ^ 0x517cc1b727220a95ull
                     ^ mix64(t->serial));
    }
    const struct srow *row = screen_row_at(&t->scr, pos.screen_y);
    if (!row)
        return 0;
    return mix64(row->uid ^ ((uint64_t)row->version << 32) ^ mix64(t->serial));
}

int wsap_vtcore_view_offset(const struct wsap_vtcore *t)
{
    if (!t)
        return 0;
    return wsap_vtcore_hist_view_scroll_offset(&t->view, &t->hist, t->cols);
}

void wsap_vtcore_scroll_to(struct wsap_vtcore *t, int offset)
{
    if (!t)
        return;
    wsap_vtcore_write_begin(t);
    if (t->alternate || offset < 0)
        offset = 0;
    struct view_anchor old = t->view.anchor;
    wsap_vtcore_hist_view_scroll_to(&t->view, &t->hist, t->cols, screen_top_line(t), offset);
    if (old.active != t->view.anchor.active || old.line_id != t->view.anchor.line_id ||
        old.seg != t->view.anchor.seg)
        t->full_gen++;
    wsap_vtcore_write_end(t);
}

void wsap_vtcore_scroll(struct wsap_vtcore *t, int lines)
{
    if (!t)
        return;
    wsap_vtcore_write_begin(t);
    if (t->alternate) {
        wsap_vtcore_write_end(t);
        return;
    }
    struct view_anchor old = t->view.anchor;
    wsap_vtcore_hist_view_scroll(&t->view, &t->hist, t->cols, screen_top_line(t), lines);
    if (old.active != t->view.anchor.active || old.line_id != t->view.anchor.line_id ||
        old.seg != t->view.anchor.seg)
        t->full_gen++;
    wsap_vtcore_write_end(t);
}

struct wsap_vtcore_scrollback wsap_vtcore_scrollback(const struct wsap_vtcore *t)
{
    if (!t)
        return (struct wsap_vtcore_scrollback){ 0 };
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    struct wsap_vtcore_scrollback sb = { 0 };
    if (!t->alternate) {
        sb.offset = wsap_vtcore_view_offset(t);
        sb.total = wsap_vtcore_screen_history_rows(t);
    }
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return sb;
}

void wsap_vtcore_screen_erase_line(struct wsap_vtcore *t, int mode)
{
    t->pending_wrap = false;
    int p = phys_row(t, t->cursor_y);
    if (mode == 0) {
        erase_span(t, p, t->cursor_x, erase_width(t, p));
    } else if (mode == 1) {
        erase_span(t, p, 0, t->cursor_x + 1);
    } else if (mode == 2) {
        erase_span(t, p, 0, erase_width(t, p));
    }
}

void wsap_vtcore_screen_delete_chars(struct wsap_vtcore *t, int n)
{
    t->pending_wrap = false;
    if (t->cursor_x >= t->cols)
        return;
    int x = t->cursor_x;
    int cols = t->cols;
    if (n > cols - x)
        n = cols - x;
    int p = phys_row(t, t->cursor_y);
    struct cell *row = row_cells(t, p);
    struct srow *srow = wsap_vtcore_row(t, t->cursor_y);
    int len = srow->len;
    clear_wide_edges(row, x, x + n, len);
    if (len > cols) {
        clear_cells_default(row + cols, len - cols);
        len = cols;
    }
    if (len > x + n) {
        memmove(&row[x], &row[x + n], (size_t)(len - (x + n)) * sizeof(struct cell));
        clear_cells_default(&row[len - n], n);
        srow->len = (uint16_t)(len - n);
    } else if (len > x) {
        clear_cells_default(&row[x], len - x);
        srow->len = (uint16_t)x;
    }
    srow->flags &= (uint8_t)~RF_TAIL_PADDED;
    bool pristine = using_default_colors(t);
    if (!pristine) {
        int from = cols - n;
        if (from < x)
            from = x;
        wsap_vtcore_row_fill_gap(srow, from);
        fill_cells(&row[from], cols - from, t->cur_fg, t->cur_bg);
        srow->len = (uint16_t)cols;
    }
    row_set_wrapped(srow, false);
    repair_wide_row(row, x - 1, len, cols, srow->len);
    srow->version++;
}

void wsap_vtcore_screen_insert_chars(struct wsap_vtcore *t, int n)
{
    t->pending_wrap = false;
    if (t->cursor_x >= t->cols)
        return;
    int x = t->cursor_x;
    int cols = t->cols;
    if (n > cols - x)
        n = cols - x;
    int p = phys_row(t, t->cursor_y);
    struct cell *row = row_cells(t, p);
    struct srow *srow = wsap_vtcore_row(t, t->cursor_y);
    wsap_vtcore_row_fill_gap(srow, x);
    int len = srow->len;
    if (x > 0 && x < len && (row[x].flags & CELL_TRAIL)) {
        fill_cells(row + x - 1, 2, t->cur_fg, t->cur_bg);
        srow->version++;
    }
    if (len > x) {
        int shift = cols - (x + n);
        if (shift > len - x)
            shift = len - x;
        if (shift > 0)
            memmove(&row[x + n], &row[x], (size_t)shift * sizeof(struct cell));
    }
    fill_cells(&row[x], n, t->cur_fg, t->cur_bg);
    bool pristine = using_default_colors(t);
    int new_len = len + n > cols ? cols : len + n;
    if (pristine && len <= x)
        new_len = len;
    else if (new_len < x + n)
        new_len = x + n;
    if (new_len < cols)
        clear_cells_default(row + new_len, cols - new_len);
    srow->len = (uint16_t)new_len;
    srow->flags &= (uint8_t)~RF_TAIL_PADDED;
    row_set_wrapped(srow, false);
    int repair_end = x + n + 1 > len + 1 ? x + n + 1 : len + 1;
    repair_wide_row(row, x - 1, repair_end, cols, srow->len);
    srow->version++;
}

void wsap_vtcore_screen_erase_all(struct wsap_vtcore *t)
{
    t->pending_wrap = false;
    struct screen *s = &t->scr;
    bool pristine = using_default_colors(t);
    for (int y = 0; y < s->rows; y++) {
        int p = phys_row(t, y);
        if (pristine && s->slot[p].len == 0 && !(s->slot[p].flags & RF_WRAPPED))
            continue;
        erase_row(t, p);
    }
    t->full_gen++;
}

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static bool text_has_text_avx2(const uint8_t *text, int len)
{
    int x = 1;
    const __m256i vspace = _mm256_set1_epi8(' ');
    for (; x + 32 <= len - 1; x += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(text + x));
        __m256i diff = _mm256_subs_epu8(chunk, vspace);
        if (!_mm256_testz_si256(diff, diff))
            return true;
    }
    for (; x < len - 1; x++)
        if (text[x] > ' ')
            return true;
    return false;
}

WSAP_VTCORE_TARGET_AVX2
static bool cells_has_text_avx2(const struct cell *cells, int len)
{
    int x = 1;
    const __m256i vspace = _mm256_set1_epi32(0x20);
    const __m256i mask = _mm256_setr_epi32(-1, 0, 0, 0, -1, 0, 0, 0);
    for (; x + 4 <= len - 1; x += 4) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(cells + x));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(cells + x + 2));
        __m256i cmp0 = _mm256_cmpgt_epi32(v0, vspace);
        __m256i cmp1 = _mm256_cmpgt_epi32(v1, vspace);
        __m256i hits = _mm256_and_si256(_mm256_or_si256(cmp0, cmp1), mask);
        if (!_mm256_testz_si256(hits, hits))
            return true;
    }
    for (; x < len - 1; x++)
        if (cells[x].c > ' ')
            return true;
    return false;
}
#endif

static inline bool text_has_text(const uint8_t *text, int len)
{
    if (len <= 0)
        return false;
    if (text[0] > ' ')
        return true;
    if (len == 1)
        return false;
    if (text[len - 1] > ' ')
        return true;

    int x = 1;
#if defined(__aarch64__)
    const uint8x16_t vspace = vdupq_n_u8(' ');
    for (; x + 16 <= len - 1; x += 16) {
        uint8x16_t chunk = vld1q_u8(text + x);
        uint8x16_t cmp = vcgtq_u8(chunk, vspace);
        if (vmaxvq_u8(cmp) != 0)
            return true;
    }
#elif defined(__x86_64__)
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return text_has_text_avx2(text, len);
#endif
    const __m128i vspace = _mm_set1_epi8(' ');
    for (; x + 16 <= len - 1; x += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(text + x));
        __m128i diff = _mm_subs_epu8(chunk, vspace);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) != 0xFFFF)
            return true;
    }
#endif
    for (; x < len - 1; x++)
        if (text[x] > ' ')
            return true;
    return false;
}

static inline bool cells_has_text(const struct cell *cells, int len)
{
    if (len <= 0)
        return false;
    if (cells[0].c > ' ')
        return true;
    if (len == 1)
        return false;
    if (cells[len - 1].c > ' ')
        return true;

    int x = 1;
#if defined(__aarch64__)
    const uint32x4_t vspace = vdupq_n_u32(' ');
    for (; x + 4 <= len - 1; x += 4) {
        uint32x4x4_t v = vld4q_u32((const uint32_t *)(cells + x));
        uint32x4_t cmp = vcgtq_u32(v.val[0], vspace);
        if (vmaxvq_u32(cmp) != 0)
            return true;
    }
#elif defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return cells_has_text_avx2(cells, len);
#endif
    for (; x < len - 1; x++)
        if (cells[x].c > ' ')
            return true;
    return false;
}

static inline bool srow_has_text(const struct srow *r, int cols)
{
    int len = r->len < cols ? r->len : cols;
    if (len <= 0)
        return false;
    if (r->kind == ROW_TEXT)
        return text_has_text(r->text.text, r->text.text_len < len ? r->text.text_len : len);
    return cells_has_text(r->cells, len);
}

void wsap_vtcore_screen_erase(struct wsap_vtcore *t, int mode)
{
    t->pending_wrap = false;
    if (mode == 3) {
        wsap_vtcore_hist_clear(&t->hist);
        t->view.anchor = (struct view_anchor){0};
        t->view.index_cols = 0;
        wsap_vtcore_select_clear(t);
        t->full_gen++;
        return;
    }
    if (mode == 2) {
        int last_active = -1;
        bool tail_clean = using_default_colors(t);
        int rows = t->rows;
        int cols = t->cols;
        const struct srow *slots = t->scr.slot;
        int p = phys_row(t, rows - 1);
        for (int y = rows - 1; y >= 0; y--) {
            const struct srow *r = &slots[p];
            p = (p == 0) ? rows - 1 : p - 1;
            if (WSAP_VTCORE_EXPECT(r->len == 0, 1)) {
                if (WSAP_VTCORE_EXPECT((r->flags & RF_WRAPPED) != 0, 0))
                    tail_clean = false;
                continue;
            }
            if (srow_has_text(r, cols)) {
                last_active = y;
                break;
            }
            tail_clean = false;
        }
        if (last_active >= 0 && !t->alternate) {
            wsap_vtcore_screen_scroll_region(t, 0, rows - 1, last_active + 1, false, true);
            if (tail_clean) {
                t->view.anchor = (struct view_anchor){0};
                return;
            }
        }
        wsap_vtcore_screen_erase_all(t);
        t->view.anchor = (struct view_anchor){0};
        t->full_gen++;
    } else if (mode == 0) {
        int cur_p = phys_row(t, t->cursor_y);
        erase_span(t, cur_p, t->cursor_x, erase_width(t, cur_p));
        bool pristine = using_default_colors(t);
        for (int r = t->cursor_y + 1; r < t->rows; r++) {
            int p = phys_row(t, r);
            if (pristine && t->scr.slot[p].len == 0 && !(t->scr.slot[p].flags & RF_WRAPPED))
                continue;
            erase_row(t, p);
        }
        t->scr.slot[cur_p].version++;
    } else if (mode == 1) {
        bool pristine = using_default_colors(t);
        for (int r = 0; r < t->cursor_y; r++) {
            int p = phys_row(t, r);
            if (pristine && t->scr.slot[p].len == 0 && !(t->scr.slot[p].flags & RF_WRAPPED))
                continue;
            erase_row(t, p);
        }
        int cur_p = phys_row(t, t->cursor_y);
        erase_span(t, cur_p, 0, t->cursor_x + 1);
        t->scr.slot[cur_p].version++;
    }
}

static inline void reset_scrolled_row(struct wsap_vtcore *t, struct srow *row, bool def)
{
    row->kind = ROW_CELLS;
    row->uid = ++t->next_row_uid;
    if (def) {
        row->len = 0;
        row->flags = 0;
    } else {
        fill_cells(row->cells, t->cols, t->cur_fg, t->cur_bg);
        row->len = (uint16_t)t->cols;
        row->flags = RF_TAIL_PADDED;
    }
}

static void rotate_and_clear_srows(struct wsap_vtcore *t, struct srow *base, int height,
                                   int count, bool down, bool def)
{
    struct srow tmp[64];
    struct srow *saved = (count <= 64) ? tmp : malloc((size_t)count * sizeof(struct srow));
    if (!saved)
        return;

    int kept = down ? count : 0;
    int cleared = down ? 0 : height - count;
    memcpy(saved, base + (down ? height - count : 0), (size_t)count * sizeof(struct srow));
    memmove(base + kept, base + (down ? 0 : count),
            (size_t)(height - count) * sizeof(struct srow));
    memcpy(base + cleared, saved, (size_t)count * sizeof(struct srow));
    for (int i = 0; i < height - count; i++)
        base[kept + i].version++;
    for (int i = 0; i < count; i++) {
        struct srow *dst = &base[cleared + i];
        reset_scrolled_row(t, dst, def);
        dst->version = 1;
    }
    if (saved != tmp)
        free(saved);
}

void wsap_vtcore_screen_scroll_region(struct wsap_vtcore *t, int top, int bottom, int count,
                               bool down, bool history)
{
    t->pending_wrap = false;
    if (count <= 0 || top > bottom || top < 0 || bottom >= t->rows)
        return;
    int height = bottom - top + 1;
    if (count > height)
        count = height;

    if (top == 0 && bottom == t->rows - 1) {
        struct screen *s = &t->scr;
        int rows = s->rows;
        int orig_top = s->top;
        bool def = using_default_colors(t);

        if (!down) {
            bool has_hist = history && screen_has_history(t);
            if (WSAP_VTCORE_EXPECT(t->prompt_y >= 0, 0) && !t->alternate) {
                if (history) {
                    if (t->prompt_y > 0)
                        t->prompt_y = t->prompt_y > count ? t->prompt_y - count : 0;
                } else {
                    t->prompt_y -= count;
                    if (t->prompt_y < 0) {
                        t->prompt_y = -1;
                        t->has_prompt = false;
                    }
                }
            }
            for (int i = 0; i < count; i++) {
                int p = orig_top + i;
                if (p >= rows) p -= rows;
                struct srow *row = &s->slot[p];
                if (WSAP_VTCORE_EXPECT(has_hist, 1))
                    wsap_vtcore_screen_push_row(t, row);
                else
                    screen_drop_top(s, row);
                if (history) {
                    screen_new_row(t, row);
                } else {
                    reset_scrolled_row(t, row, def);
                    row->version++;
                }
            }
            int new_top = orig_top + count;
            if (new_top >= rows) new_top -= rows;
            s->top = new_top;
            clamp_selection_after_scroll(t);
            t->full_gen++;
            return;
        } else {
            if (!t->alternate && t->prompt_y >= 0) {
                t->prompt_y += count;
                if (t->prompt_y >= rows) {
                    t->prompt_y = -1;
                    t->has_prompt = false;
                }
            }
            for (int i = 0; i < rows - count; i++) {
                int p = orig_top + i;
                if (p >= rows) p -= rows;
                s->slot[p].version++;
            }
            for (int i = 0; i < count; i++) {
                int p = orig_top + (rows - count) + i;
                if (p >= rows) p -= rows;
                struct srow *row = &s->slot[p];
                reset_scrolled_row(t, row, def);
                row->version = 1;
            }
            int new_top = orig_top - count;
            if (new_top < 0) new_top += rows;
            s->top = new_top;
            t->full_gen++;
            return;
        }
    }

    if (!down && history && top == 0 && !t->alternate) {
        for (int i = 0; i < count; i++) {
            wsap_vtcore_screen_scroll_up(t);
            if (bottom + 1 < t->rows)
                wsap_vtcore_screen_scroll_region(t, bottom, t->rows - 1, 1, true, false);
        }
        return;
    }

    struct screen *s = &t->scr;
    bool def = using_default_colors(t);

    if (count == height) {
        int start = phys_row(t, top);
        for (int i = 0; i < height; i++) {
            int p = start + i;
            struct srow *dst = &s->slot[p >= s->rows ? p - s->rows : p];
            reset_scrolled_row(t, dst, def);
            dst->version++;
        }
    } else {
        int start = phys_row(t, top);
        if (start + height <= s->rows) {
            rotate_and_clear_srows(t, s->slot + start, height, count, down, def);
        } else {
            struct srow tmp[256];
            struct srow *buf = (height <= 256) ? tmp : malloc((size_t)height * sizeof(struct srow));
            if (!buf)
                return;
            int n1 = s->rows - start;
            int n2 = height - n1;
            memcpy(buf, s->slot + start, (size_t)n1 * sizeof(struct srow));
            memcpy(buf + n1, s->slot, (size_t)n2 * sizeof(struct srow));

            rotate_and_clear_srows(t, buf, height, count, down, def);

            memcpy(s->slot + start, buf, (size_t)n1 * sizeof(struct srow));
            memcpy(s->slot, buf + n1, (size_t)n2 * sizeof(struct srow));
            if (buf != tmp)
                free(buf);
        }
    }

    if (!t->alternate && t->prompt_y >= top && t->prompt_y <= bottom) {
        t->prompt_y += down ? count : -count;
        if (t->prompt_y < top || t->prompt_y > bottom) {
            t->prompt_y = -1;
            t->has_prompt = false;
        }
    }
    t->full_gen++;
}

static bool wsap_vtcore_resize_internal(struct wsap_vtcore *t, int cols, int rows);

void wsap_vtcore_screen_set_alternate(struct wsap_vtcore *t, bool enabled)
{
    if (t->alternate == enabled)
        return;
    if (enabled) {
        if (!t->alt_allocated || t->alt_scr.cols != t->cols || t->alt_scr.rows != t->rows) {
            if (!screen_setup(&t->alt_scr, t->cols, t->rows, &t->next_row_uid))
                return;
            t->alt_allocated = true;
        } else {
            for (int y = 0; y < t->alt_scr.rows; y++) {
                struct srow *r = &t->alt_scr.slot[y];
                r->kind = ROW_CELLS;
                r->len = 0;
                r->flags = 0;
                r->uid = ++t->next_row_uid;
                r->version++;
            }
            t->alt_scr.top = 0;
        }
        struct screen tmp_scr = t->scr;
        t->scr = t->alt_scr;
        t->alt_scr = tmp_scr;
        t->normal_saved_cursor = wsap_vtcore_cursor_save(t);
        t->normal_saved_decsc = t->saved;
        t->normal_cursor_visible = t->cursor_visible;
        t->normal_cursor_style = (uint8_t)t->cursor_style;
        t->normal_cursor_blink = t->cursor_blink;
        t->normal_cols = t->cols;
        t->normal_rows = t->rows;
        t->alternate = true;
        t->cursor_x = 0;
        t->cursor_y = 0;
        t->pending_wrap = false;
        t->scroll_top = 0;
        t->scroll_bottom = t->rows - 1;
        t->left_margin = 0;
        t->right_margin = t->cols - 1;
        t->view.anchor = (struct view_anchor){0};
        wsap_vtcore_select_clear(t);
        t->full_gen++;
    } else {
        int cols = t->cols;
        int rows = t->rows;
        struct screen tmp_scr = t->scr;
        t->scr = t->alt_scr;
        t->alt_scr = tmp_scr;
        t->cols = t->scr.cols;
        t->rows = t->scr.rows;
        t->alternate = false;
        t->saved = t->normal_saved_decsc;
        t->cursor_visible = t->normal_cursor_visible;
        t->cursor_style = (enum wsap_vtcore_cursor_style)t->normal_cursor_style;
        t->cursor_blink = t->normal_cursor_blink;
        wsap_vtcore_cursor_apply(t, &t->normal_saved_cursor);
        t->cursor_x = t->normal_saved_cursor.x;
        t->cursor_y = t->normal_saved_cursor.y;
        t->pending_wrap = t->normal_saved_cursor.pending_wrap;
        if (cols != t->cols || rows != t->rows)
            wsap_vtcore_resize_internal(t, cols, rows);
        t->scroll_top = 0;
        t->scroll_bottom = t->rows - 1;
        t->left_margin = 0;
        t->right_margin = t->cols - 1;
        t->view.anchor = (struct view_anchor){0};
        wsap_vtcore_select_clear(t);
        t->full_gen++;
    }
}

static int scrollback_lines(int requested)
{
    return requested == 0 ? WSAP_VTCORE_SCROLLBACK_DEFAULT : requested;
}

static atomic_int tables_state;

static void tables_init(void)
{
    if (atomic_load_explicit(&tables_state, memory_order_acquire) == 2)
        return;
    int expected = 0;
    if (atomic_compare_exchange_strong(&tables_state, &expected, 1)) {
        wsap_vtcore_isa_init();
        wsap_vtcore_text_tables_init();
        atomic_store_explicit(&tables_state, 2, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&tables_state, memory_order_acquire) != 2)
        wsap_vtcore_cpu_relax();
}

bool wsap_vtcore_tables_ready(void)
{
    return atomic_load_explicit(&tables_state, memory_order_relaxed) == 2;
}

static bool name_valid(const char *s)
{
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (i >= 63 || c < 0x20 || c > 0x7e)
            return false;
    }
    return true;
}

struct wsap_vtcore *wsap_vtcore_new(const struct wsap_vtcore_options *opt)
{
    tables_init();
    static const struct wsap_vtcore_options defaults;
    if (!opt)
        opt = &defaults;
    for (size_t i = 0; i < sizeof(opt->reserved) / sizeof(opt->reserved[0]); i++)
        if (opt->reserved[i])
            return NULL;
    if (opt->size.x < 0 || opt->size.y < 0 || opt->size.x > WSAP_VTCORE_MAX_COLS)
        return NULL;
    if (opt->scrollback < 0 || opt->scrollback > WSAP_VTCORE_SCROLLBACK_MAX ||
        (unsigned)opt->cursor_style > WSAP_VTCORE_CURSOR_BAR)
        return NULL;
    const char *name = opt->name && *opt->name ? opt->name : "wsap_vtcore";
    if (!name_valid(name))
        return NULL;
    int cols = opt->size.x == 0 ? 80 : opt->size.x;
    int rows = opt->size.y == 0 ? 24 : opt->size.y;

    static _Atomic uint64_t next_serial = 1;
    struct wsap_vtcore *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    if (!wsap_vtcore_mutex_init(&t->write_lock)) {
        free(t);
        return NULL;
    }
    memcpy(t->name, name, strlen(name) + 1);
    atomic_init(&t->writer_tid, 0);
    t->write_depth = 0;
    t->serial = atomic_fetch_add_explicit(&next_serial, 1, memory_order_relaxed);
    t->cols = cols;
    t->rows = rows;
    t->scroll_bottom = rows - 1;
    t->right_margin = cols - 1;
    t->prompt_y = -1;
    t->sb_capacity = scrollback_lines(opt->scrollback);
    t->cb = opt->callbacks;
    wsap_vtcore_palette_init(&t->palette, &opt->theme);

    t->next_row_uid = 1;
    if (!screen_setup(&t->scr, cols, rows, &t->next_row_uid)) {
        wsap_vtcore_mutex_destroy(&t->write_lock);
        free(t);
        return NULL;
    }
    if (!wsap_vtcore_hist_init(&t->hist, t->sb_capacity, 0)) {
        screen_free(&t->scr);
        wsap_vtcore_mutex_destroy(&t->write_lock);
        free(t);
        return NULL;
    }
    t->hist.wrap_cols = (uint16_t)cols;

    t->gstore = wsap_vtcore_gstore_new(t);
    t->view_scratch_cap = cols > 256 ? cols : 256;
    t->view_row_scratch = calloc((size_t)t->view_scratch_cap, sizeof(struct cell));
    t->tabs_cap = cols > 256 ? cols : 256;
    t->tabs = calloc((size_t)t->tabs_cap, 1);

    if (!t->gstore || !t->view_row_scratch || !t->tabs) {
        wsap_vtcore_free(t);
        return NULL;
    }

    t->cur_fg = WSAP_VTCORE_COLOR_FG;
    t->cur_bg = WSAP_VTCORE_COLOR_BG;
    t->cursor_visible = true;
    t->default_cursor_style = opt->cursor_style;
    t->default_cursor_blink = !opt->cursor_steady;
    t->cursor_style = t->default_cursor_style;
    t->cursor_blink = t->default_cursor_blink;
    t->autowrap = true;
    t->charsets[0] = t->charsets[1] = 'B';
    t->saved = wsap_vtcore_saved_cursor_initial();
    memcpy(t->tcap_name, t->name, sizeof(t->name));
    wsap_vtcore_tabs_default(t);
    t->full_gen = 1;
    memset(t->text_width.lo, 0xFF, sizeof(t->text_width.lo));

    return t;
}

void wsap_vtcore_free(struct wsap_vtcore *t)
{
    if (!t)
        return;
    screen_free(&t->scr);
    screen_free(&t->alt_scr);
    screen_free(&t->spare);
    free(t->resize.row.data);
    free(t->resize.rows.data);
    free(t->resize.cells.data);
    free(t->resize.line.data);
    free(t->resize.pull.data);
    free(t->resize.rest.data);
    free(t->resize.lens.data);
    free(t->ascii_ends.data);
    free(t->ascii_tail.data);
    wsap_vtcore_hist_free(&t->hist);
    wsap_vtcore_hist_view_free(&t->view);
    free(t->view_row_scratch);
    free(t->tabs);
    wsap_vtcore_osc_cleanup(t);
    wsap_vtcore_gstore_free(t->gstore);
    wsap_vtcore_mutex_destroy(&t->write_lock);
    free(t);
}

struct reflow_target {
    int cols, rows;
    struct cell *cells;
    int x;
    int count;
    int cursor_x, cursor_y, saved_x, saved_y, prompt_y;
    bool cursor_set, saved_set, prompt_set, pending_wrap;

    struct reflow_out {
        int len;
        bool wrapped;
    } *out;
    struct cell *out_cells;
    int out_cap;
    struct grow_buf *out_buf, *cells_buf;
};

static inline struct cell *reflow_out_row_cells(const struct reflow_target *r, int i)
{
    return r->out_cells + (size_t)i * (size_t)r->cols;
}

static bool reflow_target_end_row(struct reflow_target *r, bool wrapped)
{
    if (r->count == r->out_cap) {
        int cap = r->out_cap ? r->out_cap * 2 : (r->rows + 16 < 32 ? 32 : r->rows + 16);
        struct reflow_out *out = grow(r->out_buf, (size_t)cap * sizeof(struct reflow_out));
        if (!out)
            return false;
        r->out = out;
        struct cell *cells = grow(r->cells_buf, (size_t)cap * (size_t)r->cols * sizeof(struct cell));
        if (!cells)
            return false;
        r->out_cells = cells;
        r->out_cap = cap;
    }
    int idx = r->count++;
    struct reflow_out *dst = &r->out[idx];
    struct cell *dst_cells = reflow_out_row_cells(r, idx);
    int len = r->x > r->cols ? r->cols : r->x;
    memcpy(dst_cells, r->cells, (size_t)len * sizeof(struct cell));
    if (len < r->cols)
        fill_cells(dst_cells + len, r->cols - len, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
    dst->len = len;
    dst->wrapped = wrapped;
    r->x = 0;
    return true;
}

static bool reflow_append_span(struct reflow_target *r, const struct cell *cells,
                               int len, int cursor, int saved)
{
    for (int i = 0; i < len;) {
        if (r->x == r->cols) {
            if (!reflow_target_end_row(r, true))
                return false;
            continue;
        }
        int room = r->cols - r->x;
        int take = len - i < room ? len - i : room;
        if (i + take < len && (cells[i + take].flags & CELL_TRAIL)) {
            if (--take == 0) {
                r->cells[r->x] = (struct cell){' ', WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, CELL_WRAP_PAD, 0};
                r->x++;
                if (!reflow_target_end_row(r, true))
                    return false;
                continue;
            }
        }
        if (cursor >= i && cursor < i + take) {
            r->cursor_x = r->x + (cursor - i);
            r->cursor_y = r->count;
            r->cursor_set = true;
            r->pending_wrap = false;
        }
        if (saved >= i && saved < i + take) {
            r->saved_x = r->x + (saved - i);
            r->saved_y = r->count;
            r->saved_set = true;
        }
        memcpy(r->cells + r->x, cells + i, (size_t)take * sizeof(*cells));
        r->x += take;
        i += take;
    }
    return true;
}

static bool reflow_append_wide(struct reflow_target *r, const struct cell *cells, int len,
                               int cursor, int saved)
{
    for (int i = 0; i < len;) {
        if (cells[i].flags & (CELL_WRAP_PAD | CELL_TRAIL)) {
            int at = (cells[i].flags & CELL_TRAIL) && r->x > 0 ? r->x - 1 : r->x;
            if (cursor == i) {
                r->cursor_x = at;
                r->cursor_y = r->count;
                r->cursor_set = true;
                r->pending_wrap = false;
            }
            if (saved == i) {
                r->saved_x = at;
                r->saved_y = r->count;
                r->saved_set = true;
            }
            i++;
            continue;
        }
        int source = (cells[i].flags & CELL_WIDE) && i + 1 < len &&
                     (cells[i + 1].flags & CELL_TRAIL) ? 2 : 1;
        int n = (cells[i].flags & CELL_WIDE) && r->cols > 1 ? 2 : 1;
        if (r->x + n > r->cols) {
            if (r->x < r->cols) {
                r->cells[r->x] = (struct cell){' ', WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, CELL_WRAP_PAD, 0};
                r->x++;
            }
            if (!reflow_target_end_row(r, true))
                return false;
        }
        if (cursor >= i && cursor < i + source) {
            r->cursor_x = r->x + (cursor - i < n ? cursor - i : 0);
            r->cursor_y = r->count;
            r->cursor_set = true;
            r->pending_wrap = false;
        }
        if (saved >= i && saved < i + source) {
            r->saved_x = r->x + (saved - i < n ? saved - i : 0);
            r->saved_y = r->count;
            r->saved_set = true;
        }
        write_cell_span(r->cells, r->x, cells[i], n);
        r->x += n;
        i += source;
    }
    return true;
}

static bool reflow_append(struct reflow_target *r, const struct cell *cells, int len,
                          int cursor, int saved)
{
    bool has_wide = false;
    const uint16_t wide_flags = CELL_WIDE | CELL_TRAIL | CELL_WRAP_PAD;
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        if ((cells[i].flags | cells[i + 1].flags | cells[i + 2].flags | cells[i + 3].flags) & wide_flags) {
            has_wide = true;
            break;
        }
    }
    if (!has_wide) {
        for (; i < len; i++) {
            if (cells[i].flags & wide_flags) {
                has_wide = true;
                break;
            }
        }
    }
    bool ok = has_wide ? reflow_append_wide(r, cells, len, cursor, saved)
                       : reflow_append_span(r, cells, len, cursor, saved);
    if (!ok)
        return false;

    if (cursor == len) {
        r->cursor_x = r->x == r->cols ? r->cols - 1 : r->x;
        r->cursor_y = r->count;
        r->pending_wrap = (r->x == r->cols);
        r->cursor_set = true;
    }
    if (saved == len) {
        r->saved_x = r->x == r->cols ? r->cols - 1 : r->x;
        r->saved_y = r->count;
        r->saved_set = true;
    }
    return true;
}

static int pull_history_rows(struct wsap_vtcore *t, struct screen *dst_scr, int cols, int max_pull)
{
    struct hist *h = &t->hist;
    int pull = 0;
    for (uint64_t id = hist_end_id(h); pull < max_pull && id > hist_first_id(h); id--)
        pull += wsap_vtcore_hist_line_rows(h, id - 1, cols);
    if (pull > max_pull)
        pull = max_pull;

    int slot = pull;
    while (slot > 0) {
        uint64_t id = hist_end_id(h) - 1;
        int line_rows = wsap_vtcore_hist_line_rows(h, id, cols);
        bool open = hist_is_open(h);
        int take = slot < line_rows ? slot : line_rows;
        int first = line_rows - take;
        struct cell *pull_buf = grow(&t->resize.pull, (size_t)take * (size_t)cols * sizeof(*pull_buf));
        int *pull_lens = grow(&t->resize.lens, (size_t)take * sizeof(*pull_lens));
        if (pull_buf && pull_lens)
            wsap_vtcore_hist_decode_segments(h, id, first, take, cols, pull_buf, pull_lens, t->gstore);
        for (int seg = line_rows - 1; seg >= first; seg--) {
            struct srow *dst = &dst_scr->slot[--slot];
            int idx = seg - first;
            int got = (pull_buf && pull_lens) ? pull_lens[idx] : 0;
            if (pull_buf)
                memcpy(dst->cells, pull_buf + (size_t)idx * cols, (size_t)cols * sizeof(struct cell));
            bool wrapped = seg + 1 < line_rows || open;
            if (got < cols) {
                fill_cells(dst->cells + got, cols - got, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
                if (wrapped) {
                    set_cells_wrap_pad(dst->cells + got, cols - got);
                    got = cols;
                }
            }
            dst->kind = ROW_CELLS;
            dst->len = (uint16_t)got;
            dst->flags = (wrapped ? RF_WRAPPED : 0) | RF_TAIL_PADDED;
            dst->version = 1;
            dst->uid = ++t->next_row_uid;
        }
        if (first == 0) {
            wsap_vtcore_hist_pop_line(h, NULL, 0, t->gstore);
            continue;
        }
        struct cell *rest = grow(&t->resize.rest, (size_t)first * (size_t)cols * sizeof(*rest));
        int *lens = grow(&t->resize.lens, (size_t)first * sizeof(*lens));
        if (rest && lens)
            wsap_vtcore_hist_decode_segments(h, id, 0, first, cols, rest, lens, t->gstore);
        wsap_vtcore_hist_pop_line(h, NULL, 0, t->gstore);
        if (rest && lens) {
            for (int seg = 0; seg < first; seg++) {
                struct cell *row = rest + (size_t)seg * cols;
                if (lens[seg] < cols)
                    fill_cells(row + lens[seg], cols - lens[seg], WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
                wsap_vtcore_hist_push_row(h, row, cols, true, t->gstore);
            }
        }
    }
    return pull;
}

static bool resize_target(struct wsap_vtcore *t, struct screen *out, int cols, int rows,
                          const struct screen *from)
{
    *out = t->spare;
    t->spare = (struct screen){ 0 };
    if (!screen_setup(out, cols, rows, &t->next_row_uid)) {
        t->spare = *out;
        return false;
    }
    out->top_id = from->top_id;
    out->top_seg = from->top_seg;
    return true;
}

static void copy_srow_resized(struct srow *dst, struct srow *src, int old_cols, int cols)
{
    int new_len = src->len < cols ? src->len : cols;
    if (src->kind == ROW_TEXT && src->len <= cols) {
        struct text_row *d = &dst->text;
        const struct text_row *s = &src->text;
        memcpy(d->text, s->text, s->text_len);
        memcpy(d->runs.data, s->runs.data, s->runs.len);
        d->text_len = s->text_len;
        d->cells = s->cells;
        d->wide = s->wide;
        d->fill_to = s->fill_to < cols ? s->fill_to : (uint16_t)cols;
        d->fill = s->fill;
        d->flags = s->flags;
        d->start = s->start;
        d->runs.len = s->runs.len;
        d->runs.base = s->runs.base;
        d->runs.pen = s->runs.pen;
        d->runs.count = s->runs.count;
        dst->kind = ROW_TEXT;
        dst->flags = (src->flags & (RF_WRAPPED | RF_WIDE)) | RF_TAIL_PADDED;
    } else {
        srow_settle(src, old_cols);
        if (new_len > 0)
            memcpy(dst->cells, src->cells, (size_t)new_len * sizeof(struct cell));
        dst->kind = ROW_CELLS;
        dst->flags = (src->flags & (RF_WRAPPED | RF_WIDE | RF_CLUSTER)) | RF_TAIL_PADDED;
    }
    dst->len = (uint16_t)new_len;
    dst->uid = src->uid;
    dst->version = src->version + 1;
}

static bool wsap_vtcore_resize_internal(struct wsap_vtcore *t, int cols, int rows)
{
    wsap_vtcore_gstore_pin(t->gstore);

    if (t->alternate || cols == t->scr.cols) {
        struct screen *active_scr = &t->scr;
        int old_cols = active_scr->cols;
        int old_rows = active_scr->rows;

        struct screen new_scr;
        if (!resize_target(t, &new_scr, cols, rows, active_scr)) {
            wsap_vtcore_gstore_unpin(t->gstore);
            return false;
        }

        if (rows < old_rows && t->cursor_y >= rows) {
            int shift = t->cursor_y - rows + 1;
            for (int i = 0; i < shift; i++)
                wsap_vtcore_screen_scroll_up(t);
            t->cursor_y -= shift;
            t->saved.y = t->saved.y >= shift ? t->saved.y - shift : 0;
        }
        new_scr.top_id = active_scr->top_id;
        new_scr.top_seg = active_scr->top_seg;

        int pull = 0;
        if (rows > old_rows && !t->alternate)
            pull = pull_history_rows(t, &new_scr, cols, rows - old_rows);

        int y = pull;
        int p_curr = active_scr->top;
        for (int i = 0; i < old_rows && y < rows; i++, y++) {
            struct srow *old_r = &active_scr->slot[p_curr];
            srow_materialize_text(old_r);
            p_curr = (p_curr + 1 == old_rows) ? 0 : p_curr + 1;
            copy_srow_resized(&new_scr.slot[y], old_r, old_cols, cols);
        }
        int first_new = y;

        t->spare = *active_scr;
        *active_scr = new_scr;
        t->cols = cols;
        t->rows = rows;

        if (rows != old_rows) {
            for (y = first_new; y < rows; y++)
                erase_row(t, phys_row(t, y));
            t->cursor_y += pull;
            t->saved.y += pull;
            if (!t->alternate && t->prompt_y >= 0)
                t->prompt_y += pull;
        }

        if (cols != old_cols)
            t->pending_wrap = false;
        if (t->cursor_x >= cols) t->cursor_x = cols - 1;
        if (t->cursor_y >= rows) t->cursor_y = rows - 1;
        if (t->saved.x >= cols) t->saved.x = cols - 1;
        if (t->saved.y >= rows) t->saved.y = rows - 1;
        if (!t->alternate && t->prompt_y >= rows) {
            t->prompt_y = -1;
            t->has_prompt = false;
        }

        t->scroll_top = 0;
        t->scroll_bottom = rows - 1;
        t->left_margin = 0;
        t->right_margin = cols - 1;
        if (!t->alternate)
            wsap_vtcore_hist_view_rewrap(&t->view, &t->hist, cols);
        t->full_gen++;
        wsap_vtcore_gstore_unpin(t->gstore);
        return true;
    }

    int old_cols = t->scr.cols;
    int old_rows = t->scr.rows;

    struct screen new_scr;
    if (!resize_target(t, &new_scr, cols, rows, &t->scr)) {
        wsap_vtcore_gstore_unpin(t->gstore);
        return false;
    }

    int active = old_rows;
    int min_active = t->cursor_y >= t->saved.y ? t->cursor_y + 1 : t->saved.y + 1;
    int p_scan = phys_row(t, old_rows - 1);
    while (active > min_active) {
        const struct srow *sr = &t->scr.slot[p_scan];
        if (sr->len != 0 || (sr->flags & RF_WRAPPED) != 0)
            break;
        active--;
        p_scan = (p_scan == 0) ? old_rows - 1 : p_scan - 1;
    }

    struct reflow_target r = {
        .cols = cols,
        .rows = rows,
        .cells = grow(&t->resize.row, (size_t)cols * sizeof(struct cell)),
        .out_buf = &t->resize.rows,
        .cells_buf = &t->resize.cells,
    };
    if (!r.cells) {
        t->spare = new_scr;
        wsap_vtcore_gstore_unpin(t->gstore);
        return false;
    }
    if (hist_is_open(&t->hist) && t->prompt_y != 0) {
        uint64_t n = wsap_vtcore_hist_line_cells(&t->hist, hist_end_id(&t->hist) - 1);
        struct cell *pulled = n ? grow(&t->resize.pull, n * sizeof(*pulled)) : NULL;
        if (n && !pulled)
            goto reflow_failed;
        int got = wsap_vtcore_hist_peek_last_line(&t->hist, pulled, pulled ? (int)n : 0, t->gstore);
        if (pulled && got > 0 && !reflow_append(&r, pulled, got, -1, -1))
            goto reflow_failed;
    }

    size_t line_cap = (size_t)old_cols + (size_t)t->saved.x + 2;
    struct cell *line_buf = grow(&t->resize.line, line_cap * sizeof(struct cell));

    int p_src = t->scr.top;
    for (int y = 0; y < active; y++) {
        struct srow *src = &t->scr.slot[p_src];
        srow_materialize_text(src);
        p_src = (p_src + 1 == old_rows) ? 0 : p_src + 1;
        int len = src->len;
        bool wrapped = (src->flags & RF_WRAPPED) != 0;

        if (src->kind != ROW_CELLS)
            srow_settle(src, old_cols);
        const struct cell *cells = src->cells;

        if (t->prompt_y >= 0 && y >= t->prompt_y) {
            if (r.x && !reflow_target_end_row(&r, false))
                goto reflow_failed;
            if (y == t->prompt_y) {
                r.prompt_y = r.count;
                r.prompt_set = true;
            }
            if (y == t->cursor_y) {
                r.cursor_y = r.count;
                r.cursor_x = t->cursor_x < cols ? t->cursor_x : cols - 1;
                r.pending_wrap = false;
                r.cursor_set = true;
            }
            if (y == t->saved.y) {
                r.saved_y = r.count;
                r.saved_x = t->saved.x < cols ? t->saved.x : cols - 1;
                r.saved_set = true;
            }
            int keep = len > cols ? cols : len;
            memcpy(r.cells, cells, (size_t)keep * sizeof(struct cell));
            r.x = keep;
            if (!reflow_target_end_row(&r, wrapped))
                goto reflow_failed;
            continue;
        }

        if (wrapped && len < old_cols)
            len = old_cols;
        if (y == t->cursor_y) {
            int pos = t->cursor_x + (t->pending_wrap ? 1 : 0);
            if (len < pos)
                len = pos;
        }
        if (y == t->saved.y && len <= t->saved.x)
            len = t->saved.x + 1;

        if (len > src->len && src->len < old_cols && !(src->flags & RF_TAIL_PADDED)) {
            fill_cells(src->cells + src->len, old_cols - src->len,
                       WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
            src->flags |= RF_TAIL_PADDED;
        }

        const struct cell *in_cells = cells;
        if (len > old_cols && line_buf && (size_t)len <= line_cap) {
            memcpy(line_buf, cells, (size_t)old_cols * sizeof(struct cell));
            fill_cells(line_buf + old_cols, len - old_cols, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
            in_cells = line_buf;
        } else if (len > old_cols) {
            len = old_cols;
        }

        int cur_x = (y == t->cursor_y) ? t->cursor_x + (t->pending_wrap ? 1 : 0) : -1;
        int sav_x = (y == t->saved.y) ? t->saved.x : -1;
        if (!reflow_append(&r, in_cells, len, cur_x, sav_x))
            goto reflow_failed;

        if ((!wrapped || y + 1 == active) && !reflow_target_end_row(&r, false))
            goto reflow_failed;
    }
    if (r.x && !reflow_target_end_row(&r, false))
        goto reflow_failed;

    if (hist_is_open(&t->hist)) {
        if (t->prompt_y == 0)
            wsap_vtcore_hist_close_open_line(&t->hist);
        else
            wsap_vtcore_hist_pop_line(&t->hist, NULL, 0, t->gstore);
    }
    wsap_vtcore_hist_set_width(&t->hist, cols);

    int visible = rows - (cols > old_cols ? old_rows - active : 0);
    if (visible < 1)
        visible = 1;

    int screen_start = 0;
    int pull = 0;
    if (r.count > visible) {
        screen_start = r.count - visible;
        if (screen_start > r.cursor_y)
            screen_start = r.cursor_y;
        for (int i = 0; i < screen_start; i++) {
            int push_len = (r.out[i].wrapped && r.out[i].len < cols) ? cols : r.out[i].len;
            wsap_vtcore_hist_push_row(&t->hist, reflow_out_row_cells(&r, i), push_len,
                          r.out[i].wrapped, t->gstore);
        }
    } else if (r.count < visible && t->hist.lines > 0) {
        int want = visible - r.count;
        if (want > rows - r.count)
            want = rows - r.count;
        pull = pull_history_rows(t, &new_scr, cols, want);
    }

    int out_idx = 0;
    for (int i = screen_start; i < r.count && (pull + out_idx) < rows; i++) {
        struct srow *dst = &new_scr.slot[pull + out_idx++];
        int len = r.out[i].len;
        memcpy(dst->cells, reflow_out_row_cells(&r, i), (size_t)len * sizeof(struct cell));
        if (len < cols)
            fill_cells(dst->cells + len, cols - len, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
        dst->kind = ROW_CELLS;
        dst->len = (uint16_t)len;
        dst->flags = (r.out[i].wrapped ? RF_WRAPPED : 0) | RF_TAIL_PADDED;
        dst->version = 1;
        dst->uid = ++t->next_row_uid;
    }

    t->spare = t->scr;
    t->scr = new_scr;
    t->cols = cols;
    t->rows = rows;
    t->cursor_x = r.cursor_x < cols ? r.cursor_x : cols - 1;
    t->cursor_y = (r.cursor_y - screen_start) + pull;
    if (t->cursor_y < 0) t->cursor_y = 0;
    if (t->cursor_y >= rows) t->cursor_y = rows - 1;
    t->pending_wrap = r.pending_wrap && t->autowrap;

    if (t->prompt_y >= 0) {
        int new_prompt = (r.prompt_y - screen_start) + pull;
        if (!r.prompt_set || new_prompt >= rows) {
            t->prompt_y = -1;
            t->has_prompt = false;
        } else {
            t->prompt_y = new_prompt > 0 ? new_prompt : 0;
            t->has_prompt = true;
        }
    }

    if (r.saved_set && (r.saved_y - screen_start) + pull < rows) {
        t->saved.x = r.saved_x < cols ? r.saved_x : cols - 1;
        int new_saved_y = (r.saved_y - screen_start) + pull;
        t->saved.y = new_saved_y < 0 ? 0 : new_saved_y;
    } else {
        t->saved.y = rows - 1;
    }
    if (t->saved.x >= cols) t->saved.x = cols - 1;
    if (t->saved.y >= rows) t->saved.y = rows - 1;

    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    t->left_margin = 0;
    t->right_margin = cols - 1;
    wsap_vtcore_hist_view_rewrap(&t->view, &t->hist, cols);
    t->full_gen++;
    wsap_vtcore_gstore_unpin(t->gstore);
    return true;

reflow_failed:
    t->spare = new_scr;
    wsap_vtcore_gstore_unpin(t->gstore);
    return false;
}

bool wsap_vtcore_resize(struct wsap_vtcore *t, struct wsap_vtcore_vector2i size)
{
    if (!t)
        return false;
    int cols = size.x;
    int rows = size.y;
    if (cols < 1 || rows < 1 || cols > WSAP_VTCORE_MAX_COLS)
        return false;
    wsap_vtcore_write_begin(t);
    if (cols == t->cols && rows == t->rows) {
        wsap_vtcore_write_end(t);
        return true;
    }

    if (cols > t->tabs_cap) {
        int new_cap = cols + 64;
        uint8_t *new_tabs = realloc(t->tabs, (size_t)new_cap);
        if (!new_tabs) {
            wsap_vtcore_write_end(t);
            return false;
        }
        memset(new_tabs + t->tabs_cap, 0, (size_t)(new_cap - t->tabs_cap));
        int first_tab = (t->tabs_cap + 7) & ~7;
        if (first_tab == 0)
            first_tab = 8;
        uint64_t tab8 = 1;
        int c = first_tab;
        for (; c + 8 <= new_cap; c += 8)
            memcpy(new_tabs + c, &tab8, sizeof(tab8));
        if (c < new_cap)
            new_tabs[c] = 1;
        t->tabs = new_tabs;
        t->tabs_cap = new_cap;
    }
    if (cols > t->view_scratch_cap) {
        int new_cap = cols + 64;
        struct cell *new_scratch = realloc(t->view_row_scratch, (size_t)new_cap * sizeof(struct cell));
        if (!new_scratch) {
            wsap_vtcore_write_end(t);
            return false;
        }
        memset(new_scratch + t->view_scratch_cap, 0, (size_t)(new_cap - t->view_scratch_cap) * sizeof(struct cell));
        t->view_row_scratch = new_scratch;
        t->view_scratch_cap = new_cap;
    }

    bool ok = wsap_vtcore_resize_internal(t, cols, rows);
    wsap_vtcore_write_end(t);
    return ok;
}
