#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static volatile int alloc_countdown = -1;
static volatile bool alloc_was_failed;

static bool alloc_should_fail(void)
{
    if (alloc_countdown < 0)
        return false;
    if (alloc_countdown > 0) {
        alloc_countdown--;
        return false;
    }
    alloc_countdown = -1;
    alloc_was_failed = true;
    return true;
}

void *__real_malloc(size_t);
void *__wrap_malloc(size_t size)
{
    if (alloc_should_fail())
        return NULL;
    return __real_malloc(size);
}

void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *p, size_t size)
{
    if (alloc_should_fail())
        return NULL;
    return __real_realloc(p, size);
}

#define CPS_MAX 32
#define CPS_SLOT (CPS_MAX + 1)

struct frame {
    struct wsap_vtcore_view view;
    uint32_t palette[WSAP_VTCORE_PALETTE_SIZE];
    int cols, rows;
    struct wsap_vtcore_cell *cells;
    uint32_t *cps;
    uint64_t *keys;
    struct wsap_vtcore_span *sel;
};

static void frame_capture(const struct wsap_vtcore_snapshot *s, struct frame *f)
{
    const struct wsap_vtcore_view *v = wsap_vtcore_snapshot_view(s);
    f->view = *v;
    memcpy(f->palette, v->palette, sizeof(f->palette));
    f->cols = v->size.x;
    f->rows = v->size.y;
    size_t n = (size_t)f->cols * (size_t)f->rows;
    f->cells = calloc(n + 1, sizeof(*f->cells));
    f->cps = calloc(n + 1, CPS_SLOT * sizeof(*f->cps));
    f->keys = calloc((size_t)f->rows + 1, sizeof(*f->keys));
    f->sel = calloc((size_t)f->rows + 1, sizeof(*f->sel));
    assert(f->cells && f->cps && f->keys && f->sel);
    for (int y = 0; y < f->rows; y++) {
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, y);
        assert(row);
        f->keys[y] = wsap_vtcore_snapshot_row_key(s, y);
        f->sel[y] = wsap_vtcore_snapshot_row_selection(s, y);
        for (int x = 0; x < f->cols; x++) {
            size_t i = (size_t)y * f->cols + x;
            f->cells[i] = row[x];
            uint32_t *cps = f->cps + i * CPS_SLOT;
            cps[0] = 0;
            if (row[x].text & WSAP_VTCORE_TEXT_CLUSTER)
                cps[0] = (uint32_t)wsap_vtcore_snapshot_cluster(s, row[x].text, cps + 1, CPS_MAX);
        }
    }
}

static void frame_free(struct frame *f)
{
    free(f->cells);
    free(f->cps);
    free(f->keys);
    free(f->sel);
}

static bool same_view(const struct frame *a, const struct frame *b)
{
    const struct wsap_vtcore_view *x = &a->view, *y = &b->view;
    return x->size.x == y->size.x && x->size.y == y->size.y &&
           x->cursor.pos.x == y->cursor.pos.x && x->cursor.pos.y == y->cursor.pos.y &&
           x->cursor.style == y->cursor.style && x->cursor.visible == y->cursor.visible &&
           x->cursor.blink == y->cursor.blink &&
           x->scroll_offset == y->scroll_offset && x->scrolled == y->scrolled &&
           x->reverse_video == y->reverse_video && x->version == y->version &&
           x->palette_version == y->palette_version &&
           memcmp(a->palette, b->palette, sizeof(a->palette)) == 0;
}

static bool frame_same(const struct frame *a, const struct frame *b)
{
    if (!same_view(a, b))
        return false;
    size_t n = (size_t)a->cols * (size_t)a->rows;
    return memcmp(a->cells, b->cells, n * sizeof(*a->cells)) == 0 &&
           memcmp(a->cps, b->cps, n * CPS_SLOT * sizeof(*a->cps)) == 0 &&
           memcmp(a->keys, b->keys, (size_t)a->rows * sizeof(*a->keys)) == 0 &&
           memcmp(a->sel, b->sel, (size_t)a->rows * sizeof(*a->sel)) == 0;
}

static bool frame_shows_same(const struct frame *a, const struct frame *b)
{
    const struct wsap_vtcore_view *x = &a->view, *y = &b->view;
    if (a->cols != b->cols || a->rows != b->rows || x->cursor.pos.x != y->cursor.pos.x ||
        x->cursor.pos.y != y->cursor.pos.y || x->cursor.visible != y->cursor.visible ||
        x->scroll_offset != y->scroll_offset || x->reverse_video != y->reverse_video ||
        memcmp(a->palette, b->palette, sizeof(a->palette)) != 0)
        return false;
    for (int r = 0; r < a->rows; r++) {
        if (memcmp(&a->sel[r], &b->sel[r], sizeof(a->sel[r])) != 0)
            return false;
        for (int c = 0; c < a->cols; c++) {
            size_t i = (size_t)r * a->cols + c;
            const struct wsap_vtcore_cell *p = &a->cells[i], *q = &b->cells[i];
            const uint32_t *cp = a->cps + i * CPS_SLOT, *cq = b->cps + i * CPS_SLOT;
            if (p->fg != q->fg || p->bg != q->bg || p->attrs != q->attrs || p->width != q->width)
                return false;
            if (p->text & WSAP_VTCORE_TEXT_CLUSTER) {
                if (cp[0] != cq[0] || memcmp(cp + 1, cq + 1, cp[0] * sizeof(*cp)) != 0)
                    return false;
            } else if (p->text != q->text) {
                return false;
            }
        }
    }
    return true;
}

static bool update_failing_at(struct wsap_vtcore_snapshot *s, struct wsap_vtcore *t, int k)
{
    struct frame before, after;
    frame_capture(s, &before);
    alloc_was_failed = false;
    alloc_countdown = k;
    bool ok = wsap_vtcore_snapshot_update(s, t);
    alloc_countdown = -1;
    bool injected = alloc_was_failed;
    if (!ok) {
        assert(injected);
        frame_capture(s, &after);
        assert(frame_same(&before, &after));
        frame_free(&after);
        assert(wsap_vtcore_snapshot_update(s, t));
    }
    frame_free(&before);
    return injected;
}

static int sweep(bool (*attempt)(int k))
{
    int k = 0;
    while (attempt(k)) {
        k++;
        assert(k < 1000);
    }
    return k;
}

static int update_each_failure(struct wsap_vtcore_snapshot *s, struct wsap_vtcore *t)
{
    struct frame before, after;
    frame_capture(s, &before);
    int failures = 0;
    bool ok = false;
    for (int k = 0; !ok; k++) {
        assert(k < 1000);
        alloc_was_failed = false;
        alloc_countdown = k;
        ok = wsap_vtcore_snapshot_update(s, t);
        alloc_countdown = -1;
        if (ok)
            break;
        assert(alloc_was_failed);
        failures++;
        frame_capture(s, &after);
        assert(frame_same(&before, &after));
        frame_free(&after);
    }
    frame_free(&before);
    return failures;
}

static void expect_matches_fresh(struct wsap_vtcore *t, const struct wsap_vtcore_snapshot *s)
{
    struct wsap_vtcore_snapshot *fresh = wsap_vtcore_snapshot_new();
    assert(fresh && wsap_vtcore_snapshot_update(fresh, t));
    struct frame a, b;
    frame_capture(s, &a);
    frame_capture(fresh, &b);
    assert(frame_shows_same(&a, &b));
    frame_free(&a);
    frame_free(&b);
    wsap_vtcore_snapshot_free(fresh);
}

static const char flag_us[] = "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8";
static const char family[] = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";

static uint32_t cluster_cps(const struct wsap_vtcore_snapshot *s, const struct wsap_vtcore_cell *cell,
                            uint32_t *cps, int cap)
{
    return (uint32_t)wsap_vtcore_snapshot_cluster(s, cell->text, cps, cap);
}

static bool first_update_at(int k)
{
    struct wsap_vtcore_options options = { .size = { 8, 3 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s);

    const char accented[] = "e\xcc\x81 ";
    wsap_vtcore_feed(t, accented, sizeof(accented) - 1, 0);
    wsap_vtcore_feed(t, flag_us, sizeof(flag_us) - 1, 0);

    bool injected = update_failing_at(s, t, k);

    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, 0);
    assert(row && (row[0].text & WSAP_VTCORE_TEXT_CLUSTER));
    uint32_t cps[8];
    assert(cluster_cps(s, &row[0], cps, 8) == 2 && cps[0] == 'e' && cps[1] == 0x301);
    assert(cluster_cps(s, &row[2], cps, 8) == 2 && cps[0] == 0x1F1FA && cps[1] == 0x1F1F8);
    expect_matches_fresh(t, s);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    return injected;
}

static bool full_cluster_screen_at(int k)
{
    struct wsap_vtcore_options options = { .size = { 32, 16 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s);

    for (int y = 0; y < 16; y++) {
        for (int i = 0; i < 16; i++) {
            if (y % 2)
                wsap_vtcore_feed(t, flag_us, sizeof(flag_us) - 1, 0);
            else
                wsap_vtcore_feed(t, family, sizeof(family) - 1, 0);
        }
        if (y < 15)
            wsap_vtcore_feed(t, "\r\n", 2, 0);
    }

    bool injected = update_failing_at(s, t, k);

    uint32_t cps[8];
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, 0);
    assert(cluster_cps(s, &row[0], cps, 8) == 5 && cps[0] == 0x1F468 && cps[1] == 0x200D);
    row = wsap_vtcore_snapshot_row(s, 1);
    assert(cluster_cps(s, &row[0], cps, 8) == 2 && cps[0] == 0x1F1FA);
    expect_matches_fresh(t, s);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    return injected;
}

static bool resize_at(int k)
{
    struct wsap_vtcore_options options = { .size = { 8, 3 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s);
    wsap_vtcore_feed(t, "old", 3, 0);
    assert(wsap_vtcore_snapshot_update(s, t));

    assert(wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ 32, 16 }));
    wsap_vtcore_feed(t, "\x1b[H\x1b[2J", 7, 0);
    wsap_vtcore_feed(t, family, sizeof(family) - 1, 0);

    bool injected = update_failing_at(s, t, k);

    assert(wsap_vtcore_snapshot_view(s)->size.x == 32 && wsap_vtcore_snapshot_view(s)->size.y == 16);
    uint32_t cps[8];
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, 0);
    assert(cluster_cps(s, &row[0], cps, 8) == 5 && cps[0] == 0x1F468);
    expect_matches_fresh(t, s);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    return injected;
}

static bool tall_screen_at(int k)
{
    struct wsap_vtcore_options options = { .size = { 8, 300 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s);
    for (int y = 0; y < 299; y++)
        wsap_vtcore_feed(t, "\r\n", 2, 0);
    wsap_vtcore_feed(t, "e\xcc\x81yes", 5, 0);
    assert(wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 299 }, false,
                             WSAP_VTCORE_SELECT_CELL));
    assert(wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 3, .y = 299 }, true));

    bool injected = update_failing_at(s, t, k);

    assert(wsap_vtcore_snapshot_view(s)->size.y == 300);
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, 299);
    uint32_t cps[4];
    assert(cluster_cps(s, &row[0], cps, 4) == 2 && cps[0] == 'e' && cps[1] == 0x301);
    struct wsap_vtcore_span span = wsap_vtcore_snapshot_row_selection(s, 299);
    assert(span.begin == 0 && span.end == 4);
    expect_matches_fresh(t, s);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    return injected;
}

static void tall_selection_is_kept(void)
{
    struct wsap_vtcore_options options = { .size = { 8, 300 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s);
    for (int y = 0; y < 299; y++)
        wsap_vtcore_feed(t, "\r\n", 2, 0);
    wsap_vtcore_feed(t, "e\xcc\x81yes", 5, 0);
    wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 299 }, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 3, .y = 299 }, true);

    assert(wsap_vtcore_snapshot_update(s, t));
    struct wsap_vtcore_span span = wsap_vtcore_snapshot_row_selection(s, 299);
    assert(span.begin == 0 && span.end == 4);
    expect_matches_fresh(t, s);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
}

static void tall_selection_fails_cleanly(void)
{
    struct wsap_vtcore_options options = { .size = { 8, 300 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    assert(t);
    for (int y = 0; y < 299; y++)
        wsap_vtcore_feed(t, "\r\n", 2, 0);
    wsap_vtcore_feed(t, "yes", 3, 0);
    assert(wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 299 }, false,
                             WSAP_VTCORE_SELECT_CELL));
    assert(wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 1, .y = 299 }, true));
    char *before = wsap_vtcore_selection_text(t);
    assert(before && !strcmp(before, "ye"));

    alloc_was_failed = false;
    alloc_countdown = 0;
    assert(!wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 2, .y = 299 }, true));
    alloc_countdown = -1;
    assert(alloc_was_failed);

    alloc_was_failed = false;
    alloc_countdown = 0;
    assert(!wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 2, .y = 299 }, false,
                              WSAP_VTCORE_SELECT_WORD));
    alloc_countdown = -1;
    assert(alloc_was_failed);

    char *after = wsap_vtcore_selection_text(t);
    assert(after && !strcmp(after, before));
    wsap_vtcore_text_free(before);
    wsap_vtcore_text_free(after);

    assert(wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 2, .y = 299 }, true));
    char *grown = wsap_vtcore_selection_text(t);
    assert(grown && !strcmp(grown, "yes"));
    wsap_vtcore_text_free(grown);
    wsap_vtcore_free(t);
}

static void scrolling_fails_cleanly(void)
{
    struct wsap_vtcore_options options = { .size = { 8, 3 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s);

    int failures = 0;
    for (int i = 0; i < 300; i++) {
        wsap_vtcore_feed(t, "e\xcc\x81", 3, 0);
        if (i % 3 == 0)
            wsap_vtcore_feed(t, flag_us, sizeof(flag_us) - 1, 0);
        if (i % 5 == 0)
            wsap_vtcore_feed(t, family, sizeof(family) - 1, 0);
        wsap_vtcore_feed(t, "\r\n", 2, 0);
        if (i == 150)
            wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 1 }, false, WSAP_VTCORE_SELECT_CELL);
        if (i == 151)
            wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 4, .y = 2 }, true);
        failures += update_each_failure(s, t);
        expect_matches_fresh(t, s);
    }
    assert(failures > 0);
    assert(wsap_vtcore_history_rows(t) == 5);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
}

int main(void)
{
    int failures = sweep(first_update_at);
    assert(failures >= 5);
    failures = sweep(full_cluster_screen_at);
    assert(failures >= 5);
    failures = sweep(resize_at);
    assert(failures >= 5);
    failures = sweep(tall_screen_at);
    assert(failures >= 7);
    tall_selection_is_kept();
    tall_selection_fails_cleanly();
    scrolling_fails_cleanly();
    return 0;
}
