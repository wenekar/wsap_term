#include "wsap_vtcore/wsap_vtcore.h"
#include "term_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t state;

static unsigned random_below(unsigned limit)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (unsigned)(state % limit);
}

static char *logical_text(struct wsap_vtcore *t)
{
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t);
    wsap_vtcore_scroll_to(t, wsap_vtcore_scrollback(t).total);
    wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 0 }, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_scroll_to(t, 0);
    wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = size.x - 1, .y = size.y - 1 }, true);
    char *text = tt_selection_text(t);
    wsap_vtcore_select_clear(t);
    if (!text) return NULL;
    size_t out = 0;
    for (size_t i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            while (out && text[out - 1] == ' ') out--;
        }
        text[out++] = text[i];
    }
    while (out && (text[out - 1] == '\n' || text[out - 1] == ' ')) out--;
    text[out] = 0;
    return text;
}

static uint32_t cell_at(struct wsap_vtcore *t, struct wsap_vtcore_snapshot *s, int x, int y)
{
    if (!wsap_vtcore_snapshot_update(s, t)) return 0;
    return wsap_vtcore_snapshot_row(s, y)[x].text;
}

static int saved_cursor_follows_history(struct wsap_vtcore_snapshot *s)
{
    static const struct { int cols, rows, grow_cols, grow_rows; } cases[] = {
        { 8, 3, 8, 5 },
        { 8, 3, 10, 5 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct wsap_vtcore_options opt = {
            .size = { .x = cases[i].cols, .y = cases[i].rows }, .scrollback = 10,
        };
        struct wsap_vtcore *t = wsap_vtcore_new(&opt);
        if (!t) return 1;
        const char *text = "l1\r\nl2\r\nl3\r\nl4\r\nab\0337\033[1;1H";
        wsap_vtcore_feed(t, text, strlen(text), 0);
        struct wsap_vtcore_vector2i size = { .x = cases[i].grow_cols, .y = cases[i].grow_rows };
        if (!wsap_vtcore_resize(t, size)) return 1;
        wsap_vtcore_feed(t, "\0338X", 3, 0);
        struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t);
        int y = c.pos.y;
        if (cell_at(t, s, 0, y) != 'a' || cell_at(t, s, 1, y) != 'b' ||
            cell_at(t, s, 2, y) != 'X') {
            fprintf(stderr, "case %zu: DECRC after a resize lost the saved row\n", i);
            return 1;
        }
        wsap_vtcore_free(t);
    }
    return 0;
}

static int narrow_resize_keeps_cursor_row(struct wsap_vtcore_snapshot *s)
{
    struct wsap_vtcore_options opt = { .size = { .x = 40, .y = 12 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    if (!t) return 1;
    for (int y = 0; y < 12; y++) {
        char line[64];
        memset(line, 'a' + y, 40);
        line[40] = 0;
        wsap_vtcore_feed(t, line, 40, 0);
    }
    wsap_vtcore_feed(t, "\033[1;1H", 6, 0);
    if (!wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = 1, .y = 12 })) return 1;
    struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t);
    if (c.pos.x != 0 || c.pos.y != 0 || cell_at(t, s, 0, 0) != 'a') {
        fprintf(stderr, "narrow resize: cursor %d,%d, cell %x\n", c.pos.x, c.pos.y,
                cell_at(t, s, 0, 0));
        return 1;
    }
    wsap_vtcore_feed(t, "Z", 1, 0);
    if (cell_at(t, s, 0, 0) != 'Z') return 1;
    wsap_vtcore_free(t);
    return 0;
}

int main(int argc, char **argv)
{
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    if (!snap || saved_cursor_follows_history(snap) || narrow_resize_keeps_cursor_row(snap))
        return 1;
    wsap_vtcore_snapshot_free(snap);

    int seeds = argc > 1 ? atoi(argv[1]) : 100;
    static const char *word[] = {
        "alpha", " x", "中", "日", "e\xcc\x81", "🦊", "👍", " beta", "123", "a",
    };
    for (int seed = 1; seed <= seeds; seed++) {
        state = 0x9e3779b97f4a7c15ull * (uint64_t)seed;
        struct wsap_vtcore_options opt = {
            .size = { .x = 40, .y = 12 },
            .scrollback = 4096,
        };
        struct wsap_vtcore *t = wsap_vtcore_new(&opt);
        if (!t) return 1;
        for (int line = 0; line < 16; line++) {
            int words = 1 + (int)random_below(4);
            for (int i = 0; i < words; i++) {
                const char *s = word[random_below(sizeof(word) / sizeof(word[0]))];
                wsap_vtcore_feed(t, s, strlen(s), 0);
            }
            if (line != 15) wsap_vtcore_feed(t, "\r\n", 2, 0);
        }
        char *before = logical_text(t);
        if (!before) return 1;
        for (int step = 0; step < 20; step++) {
            struct wsap_vtcore_vector2i size = {
                .x = 1 + (int)random_below(200),
                .y = 1 + (int)random_below(60),
            };
            if (!wsap_vtcore_resize(t, size)) return 1;
            char *after = logical_text(t);
            if (!after) return 1;
            if (strcmp(before, after)) {
                fprintf(stderr, "seed %d step %d resize %dx%d: logical text changed\n"
                        "before: <%s>\nafter:  <%s>\n",
                        seed, step, size.x, size.y, before, after);
                return 1;
            }
            free(after);
        }
        free(before);
        wsap_vtcore_free(t);
    }
    return 0;
}
