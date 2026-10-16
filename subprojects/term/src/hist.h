#ifndef WSAP_TERM_HIST_H
#define WSAP_TERM_HIST_H

#include "cell.h"
#include "gstore.h"
#include "compat.h"
#include "pen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HIST_PAGE_SIZE 65536
#define HIST_PAGE_SLACK 64
#define HIST_PAGE_USABLE (HIST_PAGE_SIZE - HIST_PAGE_SLACK)
#define HIST_MAX_PART_CELLS 65535

enum hline_flags {
    HLINE_NARROW      = 1 << 0,
    HLINE_HAS_WIDE    = 1 << 1,
    HLINE_HAS_CLUSTER = 1 << 2,
    HLINE_HAS_LINK    = 1 << 3,
    HLINE_CONT        = 1 << 4,
    HLINE_ROWS_EXACT  = 1 << 6,
    HLINE_OPEN        = 1 << 5,
};

struct hline {
    uint32_t off;
    uint32_t text_len;
    uint16_t cells;
    uint16_t wide;
    uint16_t attr_len;
    uint8_t flags;
    uint8_t wrap_rows;
};

_Static_assert(sizeof(struct hline) == 16, "struct hline must stay 16 bytes");

struct hist_runs {
    uint8_t *data;
    uint32_t len, cap;
    uint32_t count;
    struct pen_attr base;
    struct pen_attr pen;
};

struct hist_block {
    uint64_t seq, epoch;
    uint32_t page, refs, rows;
    uint16_t cols;
};

struct hist_ascii_line {
    const char *ptr;
    uint16_t len;
    uint8_t wrapped;
};

struct text_row {
    uint8_t *text;
    uint8_t *storage;
    uint16_t text_len;
    uint16_t cells;
    uint16_t wide;
    uint16_t fill_to;
    uint8_t flags;
    struct pen_attr start;
    struct pen_attr fill;
    struct hist_runs runs;
};

#define TEXT_ROW_BYTES(cols) ((size_t)(cols) * 4 + 16)
#define TEXT_ROW_RUNS(cols) (((size_t)(cols) + 2) * 22)

void wsap_vtcore_hist_runs_emit_slow(struct hist_runs *r);

static inline void hist_runs_switch(struct hist_runs *r, struct text_pen pen)
{
    if (r->count) {
        uint32_t psl = pen_sl(&r->pen);
        if (WSAP_VTCORE_EXPECT(r->count < 0x80 && psl == pen_sl(&r->base), 1)) {
            uint8_t *p = r->data + r->len;
            uint32_t dfg = r->pen.fg != r->base.fg, dbg = r->pen.bg != r->base.bg;
            p[0] = (uint8_t)(dfg | dbg << 1);
            p[1] = (uint8_t)r->count;
            memcpy(p + 2, &r->pen.fg, 4);
            p += 2 + dfg * 4;
            memcpy(p, &r->pen.bg, 4);
            r->len = (uint32_t)(p + dbg * 4 - r->data);
        } else {
            wsap_vtcore_hist_runs_emit_slow(r);
        }
        pen_set(&r->base, r->pen.fg, r->pen.bg, psl);
    }
    pen_set_text(&r->pen, pen);
    r->count = 0;
}

static inline void hist_runs_add_pen(struct hist_runs *r, struct text_pen pen,
                                     uint32_t n)
{
    if (pen.fg != r->pen.fg || pen.bg != r->pen.bg || pen.sl != pen_sl(&r->pen))
        hist_runs_switch(r, pen);
    r->count += n;
}

static inline void hist_runs_add(struct hist_runs *r, const struct pen_attr *pen, uint32_t n)
{
    hist_runs_add_pen(r, text_pen_from_attr(pen), n);
}

static inline void text_row_start(struct text_row *row, const struct pen_attr *start)
{
    if (row->storage) row->text = row->storage;
    struct text_pen pen = text_pen_from_attr(start);
    row->text_len = 0;
    row->cells = 0;
    row->wide = 0;
    row->fill_to = 0;
    row->flags = HLINE_NARROW;
    pen_set_text(&row->start, pen);
    row->runs.len = 0;
    row->runs.count = 0;
    pen_set_text(&row->runs.base, pen);
    pen_set_text(&row->runs.pen, pen);
}

static inline void text_row_unshare(struct text_row *row)
{
    if (row->storage && row->text != row->storage) {
        memcpy(row->storage, row->text, row->text_len);
        row->text = row->storage;
    }
}

int wsap_vtcore_hist_text_decode(const struct text_row *row, struct cell *out, int cap, struct gstore *g);
void wsap_vtcore_hist_text_mark_links(const struct text_row *row, void (*mark)(void *ud, uint16_t link), void *ud);

struct hist_dec_cache {
    const uint8_t *p, *end;
    struct pen_attr pending_pen;
    struct pen_attr pen;
    uint64_t epoch;
    uint64_t id;
    uint64_t seq;
    uint32_t seg;
    uint32_t off;
    uint32_t col;
    uint32_t pending_count;
    uint32_t left;
    uint16_t cols;
    bool pending;
    bool valid;
};

struct hist {
    struct hline *ring;
    struct hist_block *blocks;
    uint64_t tail, head;
    uint64_t base_id;
    uint64_t *cont;
    uint8_t **pages;
    uint32_t *page_used;
    uint32_t *page_parts;
    uint32_t *page_rows;
    uint8_t *page_simple;
    uint16_t *free_pages;
    uint32_t *exact_rows;
    struct hist_runs runs;
    struct hist_dec_cache dec;

    uint64_t rows_id;
    uint64_t counted_rows;
    uint64_t walk_seq;

    uint64_t index_epoch;
    uint64_t base_cell_id, base_cell;
    uint64_t last_id, last_cells, last_wide;

    uint32_t ring_mask;
    uint32_t lines;
    uint32_t line_limit;
    uint32_t cont_first, cont_count, cont_mask;
    uint32_t npages, max_pages, nfree, cur_page;
    uint32_t walk_off, walk_col, walk_rows;
    uint16_t wrap_cols;
    bool all_blank;
    bool walk_on;
};

static inline uint32_t rows_narrow(uint64_t cells, int cols)
{
    return cells <= (uint64_t)cols ? 1 :
        (uint32_t)((cells + (uint64_t)cols - 1) / (uint64_t)cols);
}

bool wsap_vtcore_hist_init(struct hist *h, int line_limit, size_t byte_cap);
void wsap_vtcore_hist_free(struct hist *h);
void wsap_vtcore_hist_clear(struct hist *h);

void wsap_vtcore_hist_push_row(struct hist *h, const struct cell *cells, int len, bool wrapped,
                   const struct gstore *g);
void wsap_vtcore_hist_push_ascii(struct hist *h, const char *bytes, int n,
                     const struct pen_attr *pen, bool wrapped);
void wsap_vtcore_hist_push_ascii_lines(struct hist *h, const struct hist_ascii_line *lines,
                           int count, const struct pen_attr *pen);
void wsap_vtcore_hist_push_ascii_lf(struct hist *h, const char *bytes, const uint16_t *ends, int count);
void wsap_vtcore_hist_push_text(struct hist *h, const struct text_row *row, bool wrapped);
void wsap_vtcore_hist_push_blank(struct hist *h, size_t n);
void wsap_vtcore_hist_close_open_line(struct hist *h);

int wsap_vtcore_hist_pop_line(struct hist *h, struct cell *out, int cap, struct gstore *g);
int wsap_vtcore_hist_peek_last_line(const struct hist *h, struct cell *out, int cap, struct gstore *g);
int wsap_vtcore_hist_pop_tail_cells(struct hist *h, int n, struct cell *out, struct gstore *g);

static inline uint64_t hist_first_id(const struct hist *h)
{
    return h->base_id;
}

static inline uint64_t hist_end_id(const struct hist *h)
{
    return h->base_id + h->lines;
}

static inline bool hist_is_open(const struct hist *h)
{
    return h->head != h->tail && (h->ring[(h->head - 1) & h->ring_mask].flags & HLINE_OPEN);
}

uint64_t wsap_vtcore_hist_line_cells(const struct hist *h, uint64_t id);
uint64_t wsap_vtcore_hist_line_wide(const struct hist *h, uint64_t id);
uint8_t wsap_vtcore_hist_line_flags(const struct hist *h, uint64_t id);
int wsap_vtcore_hist_line_rows(const struct hist *h, uint64_t id, int cols);
int wsap_vtcore_hist_decode_segment(const struct hist *h, uint64_t id, int seg, int cols,
                        struct cell *out, struct gstore *g);
int wsap_vtcore_hist_decode_segments(const struct hist *h, uint64_t id, int start_seg, int count, int cols,
                         struct cell *out, int *lens, struct gstore *g);

uint64_t wsap_vtcore_hist_segment_cell(const struct hist *h, uint64_t id, int seg, int cols);
int wsap_vtcore_hist_cell_segment(const struct hist *h, uint64_t id, uint64_t cell, int cols);

void wsap_vtcore_hist_mark_links(const struct hist *h, void (*mark)(void *ud, uint16_t link), void *ud);

void wsap_vtcore_hist_set_width(struct hist *h, int cols);

uint64_t wsap_vtcore_hist_rows(const struct hist *h, int cols);
uint64_t wsap_vtcore_hist_line_estimate(const struct hist *h, uint64_t id, int cols);
uint64_t wsap_vtcore_hist_estimated_rows(const struct hist *h, int cols);
void wsap_vtcore_hist_prepare_rows(struct hist *h, int cols);
void wsap_vtcore_hist_refine_rows(struct hist *h, uint64_t id, int cols, int rows);

#endif
