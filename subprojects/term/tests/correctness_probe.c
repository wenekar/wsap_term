#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/snapshot.c"

static int fail_realloc;
static int fail_resize_calloc;
void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size)
{
    if (fail_resize_calloc && n == 2 && size == sizeof(struct srow)) {
        fail_resize_calloc = 0;
        return NULL;
    }
    return __real_calloc(n, size);
}
void *__wrap_realloc(void *ptr, size_t size)
{
    if (fail_realloc) { fail_realloc = 0; return NULL; }
    return __real_realloc(ptr, size);
}

int main(void)
{
    bool failed = false;
    struct wsap_vtcore_options options = { .size = { 8, 3 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s && wsap_vtcore_snapshot_update(s, t));
    const char accented[] = "e\xcc\x81";
    wsap_vtcore_feed(t, accented, sizeof(accented) - 1, 0);
    fail_realloc = 1;
    bool ok = wsap_vtcore_snapshot_update(s, t);
    uint32_t cps[16];
    printf("snapshot arena allocation failure: update=%d size=%dx%d (expected false and the previous 8x3 view)\n",
           ok, wsap_vtcore_snapshot_view(s)->size.x, wsap_vtcore_snapshot_view(s)->size.y);
    failed |= ok || wsap_vtcore_snapshot_view(s)->size.x != 8 || wsap_vtcore_snapshot_view(s)->size.y != 3;
    assert(wsap_vtcore_snapshot_update(s, t));
    int n = wsap_vtcore_snapshot_cluster(s, wsap_vtcore_snapshot_row(s, 0)[0].text, cps, 16);
    printf("retry after allocation recovers: cluster_length=%d (expected 2)\n", n);
    failed |= n != 2 || cps[0] != 'e' || cps[1] != 0x301;
    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);

    options.size.y = 24;
    t = wsap_vtcore_new(&options);
    s = wsap_vtcore_snapshot_new();
    assert(t && s);
    const char line[] = "e\xcc\x81\r\n";
    for (int i = 1; i <= 10000; i++) {
        wsap_vtcore_feed(t, line, sizeof(line) - 1, 0);
        assert(wsap_vtcore_snapshot_update(s, t));
        if (i == 1000 || i == 10000)
            printf("after %d lines: arena_words=%u dead_words=%u history_rows=%d\n",
                   i, s->arena_len, s->arena_dead, wsap_vtcore_history_rows(t));
    }
    failed |= s->arena_len >= 8192 || wsap_vtcore_history_rows(t) != 5;
    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);

    options.size = (struct wsap_vtcore_vector2i){ 8, 4 };
    t = wsap_vtcore_new(&options);
    s = wsap_vtcore_snapshot_new();
    assert(t && s);
    const char rows[] = "A\r\nB\r\nC\r\nD";
    wsap_vtcore_feed(t, rows, sizeof(rows) - 1, 0);
    assert(wsap_vtcore_snapshot_update(s, t));
    printf("before failed height shrink: top=%c cursor_y=%d history=%d\n",
           (char)wsap_vtcore_snapshot_row(s, 0)[0].text, wsap_vtcore_cursor(t).pos.y, wsap_vtcore_history_rows(t));
    fail_resize_calloc = 1;
    ok = wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ 8, 2 });
    assert(!fail_resize_calloc);
    assert(wsap_vtcore_snapshot_update(s, t));
    printf("after failed height shrink: resize=%d height=%d top=%c cursor_y=%d history=%d\n",
           ok, wsap_vtcore_size(t).y, (char)wsap_vtcore_snapshot_row(s, 0)[0].text,
           wsap_vtcore_cursor(t).pos.y, wsap_vtcore_history_rows(t));
    failed |= ok || wsap_vtcore_size(t).y != 4 || wsap_vtcore_snapshot_row(s, 0)[0].text != 'A' ||
              wsap_vtcore_cursor(t).pos.y != 3 || wsap_vtcore_history_rows(t) != 0;
    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    return failed ? 1 : 0;
}
