#ifndef WSAP_TEST_TERM_API_H
#define WSAP_TEST_TERM_API_H

#include "wsap_vtcore/wsap_vtcore.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SCROLLBACK 4096

static inline struct wsap_vtcore *tt_new_opts(int cols, int rows, const struct wsap_vtcore_callbacks *cb)
{
    struct wsap_vtcore_options opt = {
        .size = { .x = cols, .y = rows },
        .scrollback = TEST_SCROLLBACK,
    };
    if (cb)
        opt.callbacks = *cb;
    return wsap_vtcore_new(&opt);
}

static inline struct wsap_vtcore *tt_new(int cols, int rows)
{
    return tt_new_opts(cols, rows, NULL);
}

static inline void tt_write(struct wsap_vtcore *t, const char *s, size_t len)
{
    wsap_vtcore_feed(t, s, len, 0);
}

static inline void tt_puts(struct wsap_vtcore *t, const char *s)
{
    wsap_vtcore_feed(t, s, strlen(s), 0);
}

static inline int tt_cols(const struct wsap_vtcore *t) { return wsap_vtcore_size(t).x; }
static inline int tt_rows(const struct wsap_vtcore *t) { return wsap_vtcore_size(t).y; }
static inline int tt_cursor_x(const struct wsap_vtcore *t) { return wsap_vtcore_cursor(t).pos.x; }
static inline int tt_cursor_y(const struct wsap_vtcore *t) { return wsap_vtcore_cursor(t).pos.y; }
static inline int tt_history(const struct wsap_vtcore *t) { return wsap_vtcore_scrollback(t).total; }
static inline int tt_view_offset(const struct wsap_vtcore *t) { return wsap_vtcore_scrollback(t).offset; }

static struct wsap_vtcore_snapshot *tt_snapshot_for(struct wsap_vtcore *t)
{
    static struct wsap_vtcore_snapshot *snap;
    if (!snap)
        snap = wsap_vtcore_snapshot_new();
    wsap_vtcore_snapshot_update(snap, t);
    return snap;
}

static const struct wsap_vtcore_cell tt_blank_cell = {
    .text = ' ', .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG, .width = 1,
};

static inline const struct wsap_vtcore_cell *tt_view_cell(struct wsap_vtcore *t, int col, int row)
{
    struct wsap_vtcore_snapshot *s = tt_snapshot_for(t);
    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(s, row);
    if (!cells)
        return NULL;
    if (col < 0 || col >= wsap_vtcore_snapshot_view(s)->size.x)
        return &tt_blank_cell;
    return &cells[col];
}

static inline const struct wsap_vtcore_cell *tt_cell(struct wsap_vtcore *t, int col, int row)
{
    int offset = tt_view_offset(t);
    if (offset)
        wsap_vtcore_scroll_to(t, 0);
    const struct wsap_vtcore_cell *cell = tt_view_cell(t, col, row);
    if (offset)
        wsap_vtcore_scroll_to(t, offset);
    return cell;
}

static inline int tt_cell_cps(struct wsap_vtcore *t, const struct wsap_vtcore_cell *cell,
                              uint32_t *cps, int cap)
{
    return wsap_vtcore_snapshot_cluster(tt_snapshot_for(t), cell->text, cps, cap);
}

static inline uint32_t tt_cell_cp(struct wsap_vtcore *t, const struct wsap_vtcore_cell *cell)
{
    uint32_t cp = 0;
    if (!(cell->text & WSAP_VTCORE_TEXT_CLUSTER))
        return cell->text;
    tt_cell_cps(t, cell, &cp, 1);
    return cp;
}

static inline struct wsap_vtcore_span tt_row_selection(struct wsap_vtcore *t, int row)
{
    return wsap_vtcore_snapshot_row_selection(tt_snapshot_for(t), row);
}

static inline bool tt_selected(struct wsap_vtcore *t, int col, int row)
{
    struct wsap_vtcore_span span = tt_row_selection(t, row);
    return col >= span.begin && col < span.end;
}

static inline void tt_select(struct wsap_vtcore *t, int x1, int y1, int x2, int y2)
{
    bool forward = y1 < y2 || (y1 == y2 && x1 <= x2);
    struct wsap_vtcore_vector2i a = { .x = x1, .y = y1 }, b = { .x = x2, .y = y2 };
    wsap_vtcore_select_start(t, forward ? a : b, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_select_extend(t, forward ? b : a, true);
}

static inline char *tt_link_at(struct wsap_vtcore *t, int col, int row)
{
    return wsap_vtcore_link_at(t, (struct wsap_vtcore_vector2i){ .x = col, .y = row });
}

static inline bool tt_resize(struct wsap_vtcore *t, int cols, int rows)
{
    return wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = cols, .y = rows });
}

static inline uint32_t tt_color(struct wsap_vtcore *t, wsap_vtcore_color c)
{
    return wsap_vtcore_view_color(wsap_vtcore_snapshot_view(tt_snapshot_for(t)), c);
}

#endif
