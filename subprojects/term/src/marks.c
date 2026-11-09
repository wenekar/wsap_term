#include "marks.h"
#include "screen.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Shell integration marks (OSC 133).
 *
 * A screen row keeps its marks in its flags. A prompt row can also have an
 * entry in the row table: what OSC 133;D told about its command. The entry
 * uses the row uid as its key. A history row with marks has an entry in the
 * line ring. The ring is sorted by line and cell.
 *
 * Output controls the number of marks, so both tables have a limit. When a
 * table is full, it drops its oldest entry.
 */

#define MARKS_LINES_MAX 65536u
#define MARKS_ROWS_MAX 4096u

struct mark_row {
    uint64_t uid;
    int32_t status;
    uint8_t state;
};

struct wsap_vtcore_marks {
    struct mark_line *lines;
    uint32_t head, count, cap;
    struct mark_row *rows;
    uint32_t nrows, rows_cap;
};

static struct wsap_vtcore_marks *marks_get(struct wsap_vtcore *t)
{
    if (!t->marks)
        t->marks = calloc(1, sizeof(*t->marks));
    return t->marks;
}

void wsap_vtcore_marks_free(struct wsap_vtcore *t)
{
    if (!t->marks)
        return;
    free(t->marks->lines);
    free(t->marks->rows);
    free(t->marks);
    t->marks = NULL;
}

/* The normal screen. On the alternate screen, the core keeps it aside. */
static const struct screen *normal_screen(const struct wsap_vtcore *t)
{
    return t->alternate ? &t->alt_scr : &t->scr;
}

static int rows_find(const struct wsap_vtcore_marks *m, uint64_t uid)
{
    for (uint32_t i = 0; m && i < m->nrows; i++)
        if (m->rows[i].uid == uid)
            return (int)i;
    return -1;
}

static void rows_remove(struct wsap_vtcore_marks *m, uint32_t i)
{
    memmove(m->rows + i, m->rows + i + 1, (size_t)(m->nrows - i - 1) * sizeof(*m->rows));
    m->nrows--;
}

static void rows_drop(struct wsap_vtcore *t, uint64_t uid)
{
    int i = rows_find(t->marks, uid);
    if (i >= 0)
        rows_remove(t->marks, (uint32_t)i);
}

/* Remove the entries of rows that left the screen or lost their prompt. */
static void rows_prune(struct wsap_vtcore *t)
{
    struct wsap_vtcore_marks *m = t->marks;
    const struct screen *s = normal_screen(t);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < m->nrows; i++) {
        bool live = false;
        for (int y = 0; y < s->rows && !live; y++)
            live = s->slot[y].uid == m->rows[i].uid && (s->slot[y].flags & RF_PROMPT);
        if (live)
            m->rows[kept++] = m->rows[i];
    }
    m->nrows = kept;
}

/* Only OSC 133;D may prune: during a resize, the new rows are not on the
 * screen yet. */
static void rows_set(struct wsap_vtcore *t, uint64_t uid, uint8_t state, int32_t status, bool prune)
{
    struct wsap_vtcore_marks *m = marks_get(t);
    if (!m)
        return;
    int i = rows_find(m, uid);
    if (i < 0) {
        if (prune && m->nrows == m->rows_cap)
            rows_prune(t);
        if (m->nrows == m->rows_cap && m->rows_cap < MARKS_ROWS_MAX) {
            uint32_t cap = m->rows_cap ? m->rows_cap * 2 : 16;
            struct mark_row *rows = realloc(m->rows, cap * sizeof(*rows));
            if (rows) {
                m->rows = rows;
                m->rows_cap = cap;
            }
        }
        if (m->nrows == m->rows_cap) {
            if (!m->nrows)
                return;
            rows_remove(m, 0);
        }
        i = (int)m->nrows++;
    }
    m->rows[i] = (struct mark_row){ .uid = uid, .status = status, .state = state };
}

struct mark_info wsap_vtcore_marks_row(const struct wsap_vtcore *t, const struct srow *row)
{
    struct mark_info info = { .flags = (uint8_t)(row->flags & RF_MARKS) };
    if (info.flags & RF_PROMPT) {
        int i = rows_find(t->marks, row->uid);
        if (i >= 0) {
            info.state = t->marks->rows[i].state;
            info.status = t->marks->rows[i].status;
        }
    }
    return info;
}

void wsap_vtcore_marks_row_put(struct wsap_vtcore *t, struct srow *row, struct mark_info info)
{
    row->flags |= info.flags & RF_MARKS;
    if (info.state && (info.flags & RF_PROMPT))
        rows_set(t, row->uid, info.state, info.status, false);
}

void wsap_vtcore_marks_rows_clear(struct wsap_vtcore *t)
{
    if (t->marks)
        t->marks->nrows = 0;
}

static inline struct mark_line *line_at(const struct wsap_vtcore_marks *m, uint32_t i)
{
    return &m->lines[(m->head + i) & (m->cap - 1)];
}

/* Drop the entries of lines that left history. */
static void lines_trim(struct wsap_vtcore_marks *m, const struct hist *h)
{
    uint64_t first = hist_first_id(h);
    while (m->count && line_at(m, 0)->id < first) {
        m->head = (m->head + 1) & (m->cap - 1);
        m->count--;
    }
}

static bool lines_grow(struct wsap_vtcore_marks *m)
{
    if (m->cap >= MARKS_LINES_MAX)
        return false;
    uint32_t cap = m->cap ? m->cap * 2 : 16;
    struct mark_line *lines = malloc(cap * sizeof(*lines));
    if (!lines)
        return false;
    for (uint32_t i = 0; i < m->count; i++)
        lines[i] = *line_at(m, i);
    free(m->lines);
    m->lines = lines;
    m->head = 0;
    m->cap = cap;
    return true;
}

struct mark_at wsap_vtcore_marks_hist_at(const struct hist *h)
{
    struct mark_at at = { .end = hist_end_id(h) };
    if (hist_is_open(h)) {
        uint64_t id = at.end - 1;
        at.open = true;
        at.cell = wsap_vtcore_hist_line_cells(h, id) + (id == h->base_cell_id ? h->base_cell : 0);
    }
    return at;
}

void wsap_vtcore_marks_hist_add(struct wsap_vtcore *t, struct mark_at at, struct mark_info info)
{
    const struct hist *h = &t->hist;
    if (!(info.flags & RF_MARKS))
        return;
    /* The push did not store the row when the line count is wrong. */
    uint64_t end = hist_end_id(h);
    uint64_t id = at.open ? at.end - 1 : at.end;
    if (end != id + 1 || id < hist_first_id(h))
        return;
    if (!(info.flags & RF_PROMPT))
        info.state = 0;
    struct wsap_vtcore_marks *m = marks_get(t);
    if (!m)
        return;
    lines_trim(m, h);
    while (m->count) {
        struct mark_line *last = line_at(m, m->count - 1);
        if (last->id < id || (last->id == id && last->cell < at.cell))
            break;
        if (last->id == id && last->cell == at.cell) {
            mark_merge(&last->info, info);
            return;
        }
        m->count--;
    }
    if (m->count == m->cap && !lines_grow(m)) {
        if (!m->count)
            return;
        m->head = (m->head + 1) & (m->cap - 1);
        m->count--;
    }
    *line_at(m, m->count++) = (struct mark_line){ .id = id, .cell = at.cell, .info = info };
}

void wsap_vtcore_marks_hist_drop(struct wsap_vtcore *t, uint64_t id)
{
    struct wsap_vtcore_marks *m = t->marks;
    while (m && m->count && line_at(m, m->count - 1)->id >= id)
        m->count--;
}

/* The index of the first entry of a line after id - 1. */
static uint32_t lines_lower(const struct wsap_vtcore_marks *m, uint64_t id)
{
    uint32_t lo = 0, hi = m ? m->count : 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (line_at(m, mid)->id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

uint32_t wsap_vtcore_marks_hist_line(const struct wsap_vtcore *t, uint64_t id, uint32_t *count)
{
    uint32_t lo = lines_lower(t->marks, id), hi = lines_lower(t->marks, id + 1);
    *count = hi - lo;
    return lo;
}

const struct mark_line *wsap_vtcore_marks_hist_entry(const struct wsap_vtcore *t, uint32_t i)
{
    return line_at(t->marks, i);
}

void wsap_vtcore_marks_pull(struct wsap_vtcore *t, uint64_t id, int first, struct srow *rows, int cols)
{
    struct wsap_vtcore_marks *m = t->marks;
    while (m && m->count) {
        const struct mark_line *e = line_at(m, m->count - 1);
        if (e->id < id)
            break;
        if (e->id == id) {
            int seg = wsap_vtcore_hist_cell_segment(&t->hist, id, e->cell, cols);
            if (seg < first)
                break;
            struct mark_info info = e->info;
            m->count--;
            wsap_vtcore_marks_row_put(t, &rows[seg - first], info);
        } else {
            m->count--;
        }
    }
}

void wsap_vtcore_marks_set(struct wsap_vtcore *t, uint8_t flag)
{
    struct srow *row = &t->scr.slot[phys_row(t, t->cursor_y)];
    /* A new prompt starts a new command. Its command has no state yet. */
    if (flag & RF_PROMPT)
        rows_drop(t, row->uid);
    row->flags |= flag;
}

void wsap_vtcore_marks_finish(struct wsap_vtcore *t, bool has_status, int32_t status)
{
    uint8_t state = (uint8_t)(MARK_DONE | (has_status ? MARK_STATUS : 0));
    if (!has_status)
        status = 0;
    for (int y = t->cursor_y; y >= 0; y--) {
        const struct srow *row = &t->scr.slot[phys_row(t, y)];
        if (row->flags & RF_PROMPT) {
            rows_set(t, row->uid, state, status, true);
            return;
        }
    }
    struct wsap_vtcore_marks *m = t->marks;
    if (!m || !screen_has_history(t))
        return;
    lines_trim(m, &t->hist);
    for (uint32_t i = m->count; i-- > 0;) {
        struct mark_line *e = line_at(m, i);
        if (e->info.flags & RF_PROMPT) {
            e->info.state = state;
            e->info.status = status;
            return;
        }
    }
}

/* Public queries.
 *
 * A position is a history row (id, seg) or a screen row (seg). A walk from
 * the view top gives view rows. Its cost grows with the distance, as a
 * relative scroll does. A history position above the oldest row has a
 * negative seg. A screen position below the last row has seg >= rows.
 */

struct mpos {
    uint64_t id;
    int64_t seg;
    bool screen;
};

struct mark_query {
    const struct wsap_vtcore *t;
    const struct hist *h;
    uint64_t first, end;
    int rows, cols;
    bool hist;
};

static void query_init(struct mark_query *q, const struct wsap_vtcore *t)
{
    q->t = t;
    q->h = &t->hist;
    q->rows = t->rows;
    q->cols = t->cols;
    q->hist = screen_has_history(t) && t->hist.lines;
    q->first = q->hist ? hist_first_id(q->h) : 0;
    q->end = q->hist ? hist_end_id(q->h) : 0;
}

static inline int64_t line_rows(const struct mark_query *q, uint64_t id)
{
    return wsap_vtcore_hist_line_rows(q->h, id, q->cols);
}

static struct mpos view_top(const struct mark_query *q)
{
    const struct wsap_vtcore *t = q->t;
    struct hist_view_pos pos;
    if (wsap_vtcore_hist_view_row_pos(&t->view, q->h, q->cols, q->rows, screen_top_line(t), 0, &pos))
        return (struct mpos){ .id = pos.id, .seg = pos.seg };
    return (struct mpos){ .seg = pos.screen_y, .screen = true };
}

static struct mpos pos_step(const struct mark_query *q, struct mpos p, int64_t n)
{
    p.seg += n;
    if (p.screen) {
        if (p.seg >= 0 || !q->hist)
            return p;
        /* Rows above the screen: count up from the end of history. */
        int64_t above = p.seg;
        p = (struct mpos){ .id = q->end - 1, .seg = line_rows(q, q->end - 1) + above };
    }
    while (p.seg < 0 && p.id > q->first)
        p.seg += line_rows(q, --p.id);
    while (p.seg >= 0) {
        int64_t r = line_rows(q, p.id);
        if (p.seg < r)
            break;
        p.seg -= r;
        if (++p.id >= q->end)
            return (struct mpos){ .seg = p.seg, .screen = true };
    }
    return p;
}

static bool pos_valid(const struct mark_query *q, struct mpos p)
{
    return p.screen ? p.seg >= 0 && p.seg < q->rows : p.seg >= 0;
}

static int pos_cmp(struct mpos a, struct mpos b)
{
    if (a.screen != b.screen)
        return a.screen ? 1 : -1;
    if (!a.screen && a.id != b.id)
        return a.id < b.id ? -1 : 1;
    return a.seg < b.seg ? -1 : a.seg > b.seg;
}

/* Rows from history position p down to the top screen row. */
static int64_t rows_to_screen(const struct mark_query *q, struct mpos p)
{
    int64_t n = -p.seg;
    for (uint64_t id = p.id; id < q->end; id++)
        n += line_rows(q, id);
    return n;
}

/* The row count from a to b. It is negative when b is above a. */
static int64_t pos_dist(const struct mark_query *q, struct mpos a, struct mpos b)
{
    if (a.screen && b.screen)
        return b.seg - a.seg;
    if (!a.screen && b.screen)
        return rows_to_screen(q, a) + b.seg;
    if (a.screen)
        return -(rows_to_screen(q, b) + a.seg);
    if (a.id > b.id)
        return -pos_dist(q, b, a);
    int64_t n = b.seg - a.seg;
    for (uint64_t id = a.id; id < b.id; id++)
        n += line_rows(q, id);
    return n;
}

static inline uint8_t screen_flags(const struct mark_query *q, int y)
{
    return q->t->scr.slot[phys_row(q->t, y)].flags;
}

static inline struct mpos entry_pos(const struct mark_query *q, const struct mark_line *e)
{
    return (struct mpos){ .id = e->id, .seg = wsap_vtcore_hist_cell_segment(q->h, e->id, e->cell, q->cols) };
}


/* Find the nearest row with flag before p (dir < 0) or after p (dir > 0).
 * With at, p itself counts. */
static bool find_mark(const struct mark_query *q, struct mpos p, int dir, uint8_t flag, bool at,
                      struct mpos *out, struct mark_info *info)
{
    const struct wsap_vtcore_marks *m = q->t->marks;
    if (dir < 0) {
        if (p.screen) {
            int64_t y = p.seg >= q->rows ? q->rows - 1 : (at ? p.seg : p.seg - 1);
            for (; y >= 0; y--) {
                if (screen_flags(q, (int)y) & flag) {
                    *out = (struct mpos){ .seg = y, .screen = true };
                    goto found;
                }
            }
        }
        uint32_t i = p.screen ? (m ? m->count : 0) : lines_lower(m, p.id + 1);
        while (i-- > 0) {
            const struct mark_line *e = line_at(m, i);
            if (!q->hist || e->id < q->first)
                break;
            if (e->id >= q->end || !(e->info.flags & flag))
                continue;
            struct mpos ep = entry_pos(q, e);
            int c = pos_cmp(ep, p);
            if (c < 0 || (at && c == 0)) {
                *out = ep;
                goto found;
            }
        }
        return false;
    }
    if (!p.screen) {
        for (uint32_t i = lines_lower(m, p.id); m && i < m->count; i++) {
            const struct mark_line *e = line_at(m, i);
            if (!q->hist || e->id >= q->end)
                break;
            if (e->id < q->first || !(e->info.flags & flag))
                continue;
            struct mpos ep = entry_pos(q, e);
            int c = pos_cmp(ep, p);
            if (c > 0 || (at && c == 0)) {
                *out = ep;
                goto found;
            }
        }
    }
    int64_t y = !p.screen || p.seg < 0 ? 0 : (at ? p.seg : p.seg + 1);
    for (; y < q->rows; y++) {
        if (screen_flags(q, (int)y) & flag) {
            *out = (struct mpos){ .seg = y, .screen = true };
            goto found;
        }
    }
    return false;

found:
    if (info) {
        if (out->screen) {
            *info = wsap_vtcore_marks_row(q->t, &q->t->scr.slot[phys_row(q->t, (int)out->seg)]);
        } else {
            /* Several entries can share a row after a width change. */
            *info = (struct mark_info){0};
            for (uint32_t i = lines_lower(m, out->id); i < m->count; i++) {
                const struct mark_line *e = line_at(m, i);
                if (e->id != out->id)
                    break;
                if (entry_pos(q, e).seg == out->seg)
                    mark_merge(info, e->info);
            }
        }
    }
    return true;
}

static bool fit_int(int64_t v, int *out)
{
    if (v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

bool wsap_vtcore_find_prompt(const struct wsap_vtcore *t, int row, int dir, int *found)
{
    if (!t || !found || !dir)
        return false;
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    bool ok = false;
    if (!t->alternate) {
        struct mark_query q;
        query_init(&q, t);
        struct mpos top = view_top(&q);
        struct mpos p = pos_step(&q, top, row), a;
        ok = find_mark(&q, p, dir, RF_PROMPT, false, &a, NULL) &&
             fit_int(pos_dist(&q, top, a), found);
    }
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return ok;
}

static bool command_at(const struct mark_query *q, int row, struct wsap_vtcore_command *cmd)
{
    const struct wsap_vtcore *t = q->t;
    struct mpos top = view_top(q);
    struct mpos p = pos_step(q, top, row), a, next, out;
    struct mark_info info;
    if (!pos_valid(q, p) || !find_mark(q, p, -1, RF_PROMPT, true, &a, &info))
        return false;
    bool has_next = find_mark(q, a, 1, RF_PROMPT, false, &next, NULL);
    if (!fit_int(pos_dist(q, top, a), &cmd->prompt))
        return false;
    cmd->finished = has_next || (info.state & MARK_DONE);
    cmd->has_exit_status = (info.state & MARK_STATUS) != 0;
    cmd->exit_status = cmd->has_exit_status ? info.status : 0;
    /* A C mark on the prompt row belongs to the command before it: a
     * command without output ends on the row where the next prompt starts. */
    if (!find_mark(q, a, 1, RF_OUTPUT, false, &out, NULL) || (has_next && pos_cmp(out, next) >= 0))
        return true;
    int64_t first = pos_dist(q, top, out), last;
    if (has_next) {
        last = pos_dist(q, top, next) - 1;
    } else {
        /* The output of the last command ends at the cursor. A cursor in
         * the first column has not written to its row yet. */
        struct mpos cursor = { .seg = t->cursor_y, .screen = true };
        last = pos_dist(q, top, cursor) - (t->cursor_x == 0 && !t->pending_wrap);
    }
    if (last >= first && fit_int(first, &cmd->output_first) && fit_int(last, &cmd->output_last))
        cmd->has_output = true;
    return true;
}

bool wsap_vtcore_command_at(const struct wsap_vtcore *t, int row, struct wsap_vtcore_command *cmd)
{
    if (!t || !cmd)
        return false;
    *cmd = (struct wsap_vtcore_command){0};
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    bool ok = false;
    if (!t->alternate) {
        struct mark_query q;
        query_init(&q, t);
        ok = command_at(&q, row, cmd);
        if (!ok)
            *cmd = (struct wsap_vtcore_command){0};
    }
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return ok;
}
