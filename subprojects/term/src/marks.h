#ifndef WSAP_VTCORE_MARKS_H
#define WSAP_VTCORE_MARKS_H

#include <stdbool.h>
#include <stdint.h>

struct wsap_vtcore;
struct srow;
struct hist;

/* Shell integration marks (OSC 133) in struct srow.flags. A prompt row got
 * OSC 133;A. An output row got OSC 133;C. A row can have both. */
enum { RF_PROMPT = 0x02, RF_OUTPUT = 0x80, RF_MARKS = RF_PROMPT | RF_OUTPUT };

/* What OSC 133;D told about the command of a prompt row. */
enum { MARK_DONE = 1, MARK_STATUS = 2 };

/* The marks of one row. flags holds RF_PROMPT and RF_OUTPUT. state and
 * status apply to a prompt row only. */
struct mark_info {
    int32_t status;
    uint8_t flags;
    uint8_t state;
};

/* The end of history before a push. A pushed row starts at cell `cell` of
 * the open line, or starts a new line. */
struct mark_at {
    uint64_t end;
    uint64_t cell;
    bool open;
};

/* A history row with marks. cell is the first cell of the row in its line.
 * It counts the cells that eviction removed from the line, as the view
 * anchor does. */
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

/* OSC 133. Set flag on the cursor row. */
void wsap_vtcore_marks_set(struct wsap_vtcore *t, uint8_t flag);
/* OSC 133;D. Mark the command of the nearest prompt as done. */
void wsap_vtcore_marks_finish(struct wsap_vtcore *t, bool has_status, int32_t status);

/* The marks of a screen row. The prompt state stays in the table. */
struct mark_info wsap_vtcore_marks_row(const struct wsap_vtcore *t, const struct srow *row);
/* Add the marks to a screen row. */
void wsap_vtcore_marks_row_put(struct wsap_vtcore *t, struct srow *row, struct mark_info info);
/* Forget the prompt state of all screen rows. */
void wsap_vtcore_marks_rows_clear(struct wsap_vtcore *t);

struct mark_at wsap_vtcore_marks_hist_at(const struct hist *h);
/* Record the marks of a row that a history push after `at` stored. */
void wsap_vtcore_marks_hist_add(struct wsap_vtcore *t, struct mark_at at, struct mark_info info);
/* Forget the marks of history line id and of all later lines. */
void wsap_vtcore_marks_hist_drop(struct wsap_vtcore *t, uint64_t id);
/* The marks of history line id: *count entries from the returned index.
 * Read them with wsap_vtcore_marks_hist_entry. */
uint32_t wsap_vtcore_marks_hist_line(const struct wsap_vtcore *t, uint64_t id, uint32_t *count);
const struct mark_line *wsap_vtcore_marks_hist_entry(const struct wsap_vtcore *t, uint32_t i);
/* Move the marks of segments first and later of the last history line to
 * rows, which holds those segments at width cols. */
void wsap_vtcore_marks_pull(struct wsap_vtcore *t, uint64_t id, int first, struct srow *rows, int cols);

#endif
