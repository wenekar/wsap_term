#ifndef WSAP_VTCORE_MARKS_H
#define WSAP_VTCORE_MARKS_H

#include <stdbool.h>
#include <stdint.h>

struct wsap_vtcore;
struct srow;
struct hist;

/* Shell integration marks (OSC 133). RF_PROMPT marks a prompt row and
 * RF_OUTPUT a row with command output; a row can have both. The flag values
 * are in internal.h with the other row flags. */

/* What OSC 133;D reported about the command of a prompt row. MARK_DONE
 * means that a D arrived; MARK_STATUS means that the command has an exit
 * status, which mark_info.status holds. */
enum { MARK_DONE = 1, MARK_STATUS = 2 };

/* The marks of one row. flags holds RF_PROMPT and RF_OUTPUT bits. state
 * and status apply only when flags has RF_PROMPT. */
struct mark_info {
    int32_t status;
    uint8_t flags;
    uint8_t state;
};

/* The end of history before a push. If open, the pushed row starts at cell
 * `cell` of the open line, whose id is `end` - 1. Otherwise the row starts
 * at line `end`. */
struct mark_at {
    uint64_t end;
    uint64_t cell;
    bool open;
};

/* An entry for a history row with marks. cell is the first cell of the row.
 * Like the view anchor cell, it counts from the line start and includes the
 * cells that eviction removed. */
struct mark_line {
    uint64_t id;
    uint64_t cell;
    struct mark_info info;
};

static inline void mark_merge(struct mark_info *dst, struct mark_info src)
{
    dst->flags |= src.flags;
    if (src.state) {
        dst->state = src.state;
        dst->status = src.status;
    }
}

void wsap_vtcore_marks_free(struct wsap_vtcore *t);

/* Sets flag on the cursor row. RF_PROMPT drops the command state that the
 * row carried, because a new prompt starts a new command. */
void wsap_vtcore_marks_set(struct wsap_vtcore *t, uint8_t flag);
/* Marks the command of the nearest prompt as done. A status is recorded only
 * when has_status; otherwise the command reports no exit status. */
void wsap_vtcore_marks_finish(struct wsap_vtcore *t, bool has_status, int32_t status);

/* Returns the marks of a screen row. For a prompt row, state and status come
 * from the row table that OSC 133;D fills. */
struct mark_info wsap_vtcore_marks_row(const struct wsap_vtcore *t, const struct srow *row);
/* Adds the marks to a screen row. Records state in the row table when info
 * carries a prompt state. */
void wsap_vtcore_marks_row_put(struct wsap_vtcore *t, struct srow *row, struct mark_info info);
/* Forgets all prompt state and clears the marks of the normal screen
 * rows. */
void wsap_vtcore_marks_rows_clear(struct wsap_vtcore *t);

struct mark_at wsap_vtcore_marks_hist_at(const struct hist *h);
/* Records the marks of a row that a history push after `at` stored. */
void wsap_vtcore_marks_hist_add(struct wsap_vtcore *t, struct mark_at at, struct mark_info info);
/* Forgets the marks of history line id and of all later lines. */
void wsap_vtcore_marks_hist_drop(struct wsap_vtcore *t, uint64_t id);
/* Returns the index of the first entry of history line id and stores the
 * number of its entries in *count. Read the entries in order with
 * wsap_vtcore_marks_hist_entry. */
uint32_t wsap_vtcore_marks_hist_line(const struct wsap_vtcore *t, uint64_t id, uint32_t *count);
const struct mark_line *wsap_vtcore_marks_hist_entry(const struct wsap_vtcore *t, uint32_t i);
/* Moves the marks of history line id from segment first onward to rows,
 * which holds those segments at width cols. Marks of later lines are
 * dropped; marks of earlier segments stay. */
void wsap_vtcore_marks_pull(struct wsap_vtcore *t, uint64_t id, int first, struct srow *rows, int cols);

#endif
