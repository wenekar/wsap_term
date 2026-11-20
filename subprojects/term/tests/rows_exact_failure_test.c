/* A line of 30000 cells takes 375 rows at 80 columns. The count does not fit
 * in the line byte, so it goes in the exact-row table. When that table cannot
 * be allocated, the line must still report the count of the new width. */
#include "internal.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile bool fail_exact_table;
static volatile int failed;

void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
    if (fail_exact_table && size == sizeof(uint32_t)) {
        fail_exact_table = false;
        failed++;
        return NULL;
    }
    return __real_calloc(n, size);
}

int main(void)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 200, .y = 24 }, .scrollback = 1000 });
    assert(term);

    static char line[30002];
    memset(line, 'x', 30000);
    line[30000] = '\r';
    line[30001] = '\n';
    assert(wsap_vtcore_feed(term, line, sizeof(line), 0));
    for (int i = 0; i < 30; i++) {
        char short_line[32];
        int n = snprintf(short_line, sizeof(short_line), "short %02d\r\n", i);
        assert(n > 0 && (size_t)n < sizeof(short_line));
        assert(wsap_vtcore_feed(term, short_line, (size_t)n, 0));
    }

    /* Count the rows at 200 columns. The line takes 150 rows, so it fits in
     * the line byte and it has an exact count of the old width. */
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(snapshot);
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    assert(wsap_vtcore_hist_line_rows(&term->hist,
                                      hist_first_id(&term->hist), 200) == 150);
    assert(wsap_vtcore_resize(term,
                              (struct wsap_vtcore_vector2i){ .x = 80, .y = 24 }));

    /* The rewrap to 80 columns needs 375 rows, so it needs the exact-row
     * table. Fail that one calloc. */
    fail_exact_table = true;
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    assert(!fail_exact_table);
    assert(failed == 1);

    uint64_t first = hist_first_id(&term->hist);
    assert(wsap_vtcore_hist_line_rows(&term->hist, first, 80) == 375);
    uint64_t sum = 0;
    for (uint64_t id = hist_first_id(&term->hist);
         id < hist_end_id(&term->hist); id++)
        sum += (uint64_t)wsap_vtcore_hist_line_rows(&term->hist, id, 80);
    assert(sum == (uint64_t)wsap_vtcore_history_rows(term));

    int total = wsap_vtcore_history_rows(term);
    char *text = wsap_vtcore_history_text_range(term, 0, total, NULL);
    assert(text);
    size_t bytes = 0;
    for (const char *p = text; *p; p++)
        bytes += *p == 'x';
    assert(bytes == 30000);
    wsap_vtcore_text_free(text);

    wsap_vtcore_snapshot_free(snapshot);
    wsap_vtcore_free(term);
    return 0;
}
