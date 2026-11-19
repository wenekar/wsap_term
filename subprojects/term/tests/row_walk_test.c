#include "internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A fixed frame count above one, so a per-frame walk shows up. */
#define FRAMES 20

static struct wsap_vtcore *new_term(int cols, int rows)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = cols, .y = rows },
        .scrollback = 60000,
    });
    assert(term);
    return term;
}

static void feed(struct wsap_vtcore *term, const char *text)
{
    assert(wsap_vtcore_feed(term, text, strlen(text), 0));
}

/* The lines that need the exact walk: a wide cell or a cluster can leave a
 * column unused at a row end. */
static int affected_lines(struct wsap_vtcore *term)
{
    int n = 0;
    uint64_t end = hist_end_id(&term->hist);
    for (uint64_t id = hist_first_id(&term->hist); id < end; id++)
        if (wsap_vtcore_hist_line_flags(&term->hist, id) &
            (HLINE_HAS_WIDE | HLINE_HAS_CLUSTER))
            n++;
    return n;
}

static uint64_t walks(struct wsap_vtcore *term)
{
    return wsap_vtcore_hist_walk_calls(&term->hist);
}

/* The wide lines are 30 cells of text at 40 columns, so each one needs a walk
 * to give its row count. The lines are closed, so the count of the history is
 * stable across frames. */
static void walk_once_per_wide_line(void)
{
    struct wsap_vtcore *term = new_term(80, 24);
    char line[512];
    for (int i = 0; i < 60; i++) {
        if (i & 1) {
            int n = 0;
            for (int k = 0; k < 30; k++) {
                memcpy(line + n, "\u4e2d", 3);
                n += 3;
            }
            line[n++] = '\r';
            line[n++] = '\n';
            line[n] = '\0';
            feed(term, line);
        } else {
            int n = snprintf(line, sizeof(line), "line %04d abcdefghijklmnop\r\n", i);
            assert(n > 0 && (size_t)n < sizeof(line));
            feed(term, line);
        }
    }

    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(snapshot);
    /* The lines fit one row at 80 columns, so this draw needs no walk. */
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    uint64_t fixed = walks(term);
    for (int frame = 0; frame < FRAMES; frame++)
        assert(wsap_vtcore_snapshot_update(snapshot, term));
    assert(walks(term) == fixed);

    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 40, .y = 24 }));
    assert(walks(term) == fixed);
    int affected = affected_lines(term);
    assert(affected > 0);

    /* The first draw at the new width walks each affected line once. */
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    uint64_t after = walks(term);
    assert(after - fixed == (uint64_t)affected);

    /* The counts stay exact, so the next frames walk no line. */
    for (int frame = 0; frame < FRAMES; frame++)
        assert(wsap_vtcore_snapshot_update(snapshot, term));
    assert(walks(term) == after);
    printf("row walk: %d wide lines walked once, then %d frames walked none\n",
           affected, FRAMES + 1);

    wsap_vtcore_snapshot_free(snapshot);
    wsap_vtcore_free(term);
}

/* A wider change needs no walk: the cells of each line fit one row. The walk
 * count also must not grow when the width does not change. */
static void no_walk_when_wider(void)
{
    struct wsap_vtcore *term = new_term(40, 24);
    char line[512];
    for (int i = 0; i < 40; i++) {
        int n = 0;
        for (int k = 0; k < 30; k++) {
            memcpy(line + n, "\u4e2d", 3);
            n += 3;
        }
        line[n++] = '\r';
        line[n++] = '\n';
        line[n] = '\0';
        feed(term, line);
    }
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(snapshot);
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    uint64_t narrow = walks(term);
    assert(narrow > 0);
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 200, .y = 24 }));
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    assert(walks(term) == narrow);
    for (int frame = 0; frame < FRAMES; frame++)
        assert(wsap_vtcore_snapshot_update(snapshot, term));
    assert(walks(term) == narrow);
    printf("row walk: a wider change walked none\n");
    wsap_vtcore_snapshot_free(snapshot);
    wsap_vtcore_free(term);
}

int main(void)
{
    walk_once_per_wide_line();
    no_walk_when_wider();
    return 0;
}
