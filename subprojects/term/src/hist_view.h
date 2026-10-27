#ifndef WSAP_VTCORE_HIST_VIEW_H
#define WSAP_VTCORE_HIST_VIEW_H

#include "hist.h"

struct line_pos {
    uint64_t id;
    uint32_t seg;
};

struct view_anchor {
    uint64_t line_id;
    uint64_t cell;
    uint64_t base_cell;
    uint32_t seg;
    bool active;
};

struct hist_view {
    struct view_anchor anchor;

    uint64_t *fenwick;
    uint32_t *counts;
    uint64_t total_rows;
    uint64_t first_id, end_id;
    uint64_t epoch, head, tail, last_cells;
    int cols;
    uint32_t cap;
    int index_cols;
    bool indexed;
};

void wsap_vtcore_hist_view_free(struct hist_view *v);

bool wsap_vtcore_hist_view_sync(struct hist_view *v, const struct hist *h, int cols);
void wsap_vtcore_hist_view_refine(struct hist_view *v, struct hist *h, int cols, int rows);
uint64_t wsap_vtcore_hist_view_rows(const struct hist_view *v, const struct hist *h, int cols);
bool wsap_vtcore_hist_view_find_row(const struct hist_view *v, const struct hist *h, int row,
                        int cols, uint64_t *id, int *seg);

int wsap_vtcore_hist_view_scroll_offset(const struct hist_view *v, const struct hist *h, int cols);
void wsap_vtcore_hist_view_scroll(struct hist_view *v, const struct hist *h, int cols,
                      struct line_pos screen_top, int lines);
void wsap_vtcore_hist_view_scroll_to(struct hist_view *v, const struct hist *h, int cols,
                         struct line_pos screen_top, int offset);

struct hist_view_pos {
    uint64_t id;
    int seg;
    int screen_y;
};

void wsap_vtcore_hist_view_note_push(struct hist_view *v, const struct hist *h, int cols);
void wsap_vtcore_hist_view_rewrap(struct hist_view *v, const struct hist *h, int cols);

bool wsap_vtcore_hist_view_row_pos(const struct hist_view *v, const struct hist *h, int cols,
                       int screen_rows, struct line_pos screen_top, int y,
                       struct hist_view_pos *pos);

#endif
