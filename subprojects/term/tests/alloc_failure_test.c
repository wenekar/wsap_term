/* Fail allocation N of one workload, for every N, once and from N onward.
 * A failure can lose output. It must not crash, leak, or break the terminal:
 * once memory returns, the terminal must work like a new one.
 */
#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* More than the core keeps in one cluster. */
enum { CLUSTER_CAP = 32 };

static long alloc_seen;
static long fail_at = -1;
static bool fail_rest;
static bool failed;
static long live_blocks;

static bool should_fail(void)
{
    if (fail_at < 0)
        return false;
    long n = alloc_seen++;
    if (n == fail_at || (fail_rest && n > fail_at)) {
        failed = true;
        return true;
    }
    return false;
}

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    void *p = should_fail() ? NULL : __real_malloc(size);
    live_blocks += p != NULL;
    return p;
}

void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
    void *p = should_fail() ? NULL : __real_calloc(n, size);
    live_blocks += p != NULL;
    return p;
}

void *__real_realloc(void *p, size_t size);
void *__wrap_realloc(void *p, size_t size)
{
    if (should_fail())
        return NULL;
    void *q = __real_realloc(p, size);
    /* realloc(p, 0) can free p and return NULL. */
    live_blocks += (q != NULL) - (p != NULL);
    if (!q && size)
        live_blocks += p != NULL;
    return q;
}

void __real_free(void *p);
void __wrap_free(void *p)
{
    live_blocks -= p != NULL;
    __real_free(p);
}

static void on_reply(void *ud, const char *bytes, size_t len) { (void)ud; (void)bytes; (void)len; }
static void on_title(void *ud, const char *title) { (void)ud; (void)title; }
static void on_clipboard(void *ud, const char *sel, const char *data, size_t len)
{
    (void)ud; (void)sel; (void)data; (void)len;
}
static void on_notify(void *ud, const char *title, const char *body) { (void)ud; (void)title; (void)body; }
static void on_bell(void *ud) { (void)ud; }
static void on_trace(void *ud, const char *line) { (void)ud; (void)line; }

static const struct wsap_vtcore_callbacks callbacks = {
    .reply = on_reply, .title = on_title, .clipboard = on_clipboard,
    .notify = on_notify, .bell = on_bell, .trace = on_trace,
};

static const char *const inputs[] = {
    "hello \xe4\xb8\xad\xe6\x96\x87 e\xcc\x81 \xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd\r\n",
    "\033[1;31;48;2;1;2;3mred\033[m \033[4:3;58;5;9mcurl\033[m\r\n",
    "\033]8;;https://example.test/\033\\link\033]8;;\033\\ text\r\n",
    "\033]0;title\007\033]7;file://host/tmp/dir\007\033]22;pointer\007",
    "\033]52;c;aGVsbG8=\007\033]777;notify;head;body\033\\\a",
    "\033[6n\033[c\033[>c\033P$qm\033\\\033P+q544e\033\\\033[?2026$p",
    "\033]4;1;rgb:12/34/56\007\033]10;?\007\033]104;1\007",
    "\033[?1049h\033[2Jalt \xe2\x9d\xa4\xef\xb8\x8f\033[?1049l",
    "a long line that wraps across several rows of the screen \xe4\xb8\xad\r\n",
    "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8 e\xcc\x81\xcc\x82\r\n9\r\n",
    "\033[2;5r\033[5;1H\n\n\n\033[r\033[4hins\033[4l\033[3@\033[2P\033[2L\033[2M",
    "\033[3g\033H\tA\tB\033#8\033[H\033[2J",
    "\033[?1049h\033[?1000;1006hX\033[?1049l\033[?1000;1006l",
};

/* Generated inputs. main() builds them before any failure. */
static char lines[96 * 1024], long_osc[10 * 1024], clusters[16 * 1024], long_line[3200];
static size_t lines_len, long_osc_len, clusters_len, long_line_len;
/* One logical line longer than a 64 KiB history page. */
static char huge_line[100 * 1024];
static char crlf_lines[8 * 1024];
static size_t crlf_lines_len;

static void build_large_inputs(void)
{
    for (int i = 0; i < 2500; i++)
        lines_len += (size_t)snprintf(lines + lines_len, sizeof(lines) - lines_len,
                                      "line %d of a bulk ascii stream\n", i);
    long_osc_len = (size_t)snprintf(long_osc, sizeof(long_osc), "\033]2;");
    for (; long_osc_len < sizeof(long_osc) - 2; long_osc_len++)
        long_osc[long_osc_len] = (char)('a' + long_osc_len % 26);
    long_osc[long_osc_len++] = '\a';
    /* Distinct clusters fill the grapheme store; scrolled-off rows free them. */
    for (int i = 0; i < 600; i++) {
        unsigned mark = 0x300 + (unsigned)i % 0x70;
        clusters_len += (size_t)snprintf(clusters + clusters_len, sizeof(clusters) - clusters_len,
                                         "%c\xcc%c\xcd%c%s", 'a' + i % 26, 0x80 + (int)(mark % 0x40),
                                         0x80 + i / 26 % 0x30, i % 40 == 39 ? "\r\n" : "");
    }
    while (long_line_len < sizeof(long_line) - 2)
        long_line[long_line_len++] = 'w';
    long_line[long_line_len++] = '\r';
    long_line[long_line_len++] = '\n';
    for (int i = 0; crlf_lines_len < sizeof(crlf_lines) - 64; i++)
        crlf_lines_len += (size_t)snprintf(crlf_lines + crlf_lines_len,
                                           sizeof(crlf_lines) - crlf_lines_len,
                                           "crlf line %d of a tty stream\r\n", i);
    memset(huge_line, 'h', sizeof(huge_line) - 2);
    memcpy(huge_line + sizeof(huge_line) - 2, "\r\n", 2);
}

static void feed(struct wsap_vtcore *t, const char *s)
{
    size_t len = strlen(s);
    /* Split each input once, so chunk boundaries fall inside sequences. */
    size_t half = len / 2;
    (void)wsap_vtcore_feed(t, s, half, 0);
    (void)wsap_vtcore_feed(t, s + half, len - half, 0);
}

static void use_text(char *text)
{
    wsap_vtcore_text_free(text);
}

static void small_workload(struct wsap_vtcore *t, struct wsap_vtcore_snapshot *s)
{
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        feed(t, inputs[i]);
        if (i % 4 == 3)
            (void)wsap_vtcore_snapshot_update(s, t);
    }
    feed(t, "\033[20h");
    (void)wsap_vtcore_feed(t, lines, 4096, 0);
    feed(t, "\033[20l");
    (void)wsap_vtcore_feed(t, crlf_lines, crlf_lines_len, 0);
    (void)wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = 24, .y = 4 });
    (void)wsap_vtcore_snapshot_update(s, t);
    (void)wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = 7, .y = 9 });
    feed(t, inputs[0]);
    (void)wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = 16, .y = 6 });

    wsap_vtcore_scroll(t, 3);
    (void)wsap_vtcore_snapshot_update(s, t);
    (void)wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 1, .y = 0 }, false,
                                   WSAP_VTCORE_SELECT_WORD);
    (void)wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 5, .y = 3 }, true);
    (void)wsap_vtcore_snapshot_update(s, t);
    use_text(wsap_vtcore_selection_text(t));
    wsap_vtcore_scroll_to(t, 0);
    for (int y = 0; y < 6; y++)
        use_text(wsap_vtcore_link_at(t, (struct wsap_vtcore_vector2i){ .x = 1, .y = y }));
    use_text(wsap_vtcore_cwd(t));
    use_text(wsap_vtcore_pointer_shape(t));
    use_text(wsap_vtcore_history_text(t, 20));

    (void)wsap_vtcore_set_scrollback_limit(t, 3);
    (void)wsap_vtcore_set_scrollback_limit(t, 40);
    struct wsap_vtcore_theme theme;
    wsap_vtcore_theme_high_contrast(&theme);
    wsap_vtcore_set_theme(t, &theme);
    feed(t, inputs[1]);
    (void)wsap_vtcore_snapshot_update(s, t);
    wsap_vtcore_select_clear(t);
}

/* Sizes past the stack buffers: over 256 rows, over 512 columns. */
static void large_workload(struct wsap_vtcore *t, struct wsap_vtcore_snapshot *s)
{
    (void)wsap_vtcore_feed(t, lines, lines_len, WSAP_VTCORE_FEED_ONLCR);
    (void)wsap_vtcore_snapshot_update(s, t);
    (void)wsap_vtcore_feed(t, long_osc, long_osc_len, 0);
    feed(t, "\033P+Q544e;31\033\\\033P+q544e\033\\");
    (void)wsap_vtcore_feed(t, clusters, clusters_len, 0);
    (void)wsap_vtcore_feed(t, lines, lines_len / 2, WSAP_VTCORE_FEED_ONLCR);
    (void)wsap_vtcore_feed(t, long_line, long_line_len, 0);
    (void)wsap_vtcore_feed(t, huge_line, sizeof(huge_line), 0);
    (void)wsap_vtcore_feed(t, clusters, clusters_len, 0);
    (void)wsap_vtcore_snapshot_update(s, t);
    feed(t, "\r\nscroll\r\nrows\r\n");
    (void)wsap_vtcore_snapshot_update(s, t);

    (void)wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = 7, .y = 9 });
    wsap_vtcore_scroll(t, 500);
    (void)wsap_vtcore_snapshot_update(s, t);
    wsap_vtcore_scroll_to(t, 0);
    (void)wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = 700, .y = 300 });
    (void)wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = 600, .y = 290 });
    (void)wsap_vtcore_snapshot_update(s, t);

    (void)wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ 0 }, false, WSAP_VTCORE_SELECT_WORD);
    (void)wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 599, .y = 289 }, true);
    use_text(wsap_vtcore_selection_text(t));
    use_text(wsap_vtcore_history_text(t, 400));
    (void)wsap_vtcore_set_scrollback_limit(t, WSAP_VTCORE_SCROLLBACK_MAX);
    (void)wsap_vtcore_set_scrollback_limit(t, 60);
    feed(t, "\033[2;280r\033[280;1H\n\n\n\033[r\033[5M\033[5L");
    (void)wsap_vtcore_snapshot_update(s, t);

    /* Rotate the screen ring, then scroll a region that wraps around it. */
    feed(t, "\033[290;1H\n\n\n\n\n\n\n\n\n\n\033[2;280r\033[100S\033[100T\033[r");
    /* A wide line with two rows in history and two on the screen. */
    feed(t, "\033[289;1H\xe4\xb8\xad");
    (void)wsap_vtcore_feed(t, long_line, 2000, 0);
    (void)wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 5, .y = 289 }, false,
                                   WSAP_VTCORE_SELECT_CELL);
    (void)wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 9, .y = 289 }, true);
    use_text(wsap_vtcore_selection_text(t));
    (void)wsap_vtcore_snapshot_update(s, t);
}

struct workload {
    const char *name;
    struct wsap_vtcore_options options;
    void (*run)(struct wsap_vtcore *t, struct wsap_vtcore_snapshot *s);
};

static const struct workload workloads[] = {
    { "small", { .size = { .x = 16, .y = 6 }, .scrollback = 40, .callbacks = callbacks },
      small_workload },
    { "large", { .size = { .x = 600, .y = 290 }, .scrollback = 60, .callbacks = callbacks },
      large_workload },
};

static void check_snapshot(const struct wsap_vtcore_snapshot *s)
{
    const struct wsap_vtcore_view *v = wsap_vtcore_snapshot_view(s);
    assert(v && v->palette && v->size.x > 0 && v->size.y > 0);
    for (int y = 0; y < v->size.y; y++) {
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, y);
        assert(row);
        for (int x = 0; x < v->size.x; x++) {
            assert(row[x].width <= 2);
            uint32_t cps[CLUSTER_CAP];
            int n = wsap_vtcore_snapshot_cluster(s, row[x].text, cps, CLUSTER_CAP);
            assert(n >= 1 && n <= CLUSTER_CAP);
        }
    }
}

static void expect_same_screen(const struct wsap_vtcore_snapshot *a,
                               const struct wsap_vtcore_snapshot *b, long n)
{
    const struct wsap_vtcore_view *va = wsap_vtcore_snapshot_view(a);
    const struct wsap_vtcore_view *vb = wsap_vtcore_snapshot_view(b);
    bool same = va->size.x == vb->size.x && va->size.y == vb->size.y &&
                va->cursor.pos.x == vb->cursor.pos.x && va->cursor.pos.y == vb->cursor.pos.y;
    for (int y = 0; same && y < va->size.y; y++) {
        const struct wsap_vtcore_cell *ra = wsap_vtcore_snapshot_row(a, y);
        const struct wsap_vtcore_cell *rb = wsap_vtcore_snapshot_row(b, y);
        for (int x = 0; same && x < va->size.x; x++) {
            uint32_t ca[CLUSTER_CAP], cb[CLUSTER_CAP];
            int na = wsap_vtcore_snapshot_cluster(a, ra[x].text, ca, CLUSTER_CAP);
            int nb = wsap_vtcore_snapshot_cluster(b, rb[x].text, cb, CLUSTER_CAP);
            same = na == nb && !memcmp(ca, cb, (size_t)na * sizeof(*ca)) &&
                   wsap_vtcore_view_color(va, ra[x].fg) == wsap_vtcore_view_color(vb, rb[x].fg) &&
                   wsap_vtcore_view_color(va, ra[x].bg) == wsap_vtcore_view_color(vb, rb[x].bg) &&
                   ra[x].attrs == rb[x].attrs && ra[x].width == rb[x].width;
            if (!same)
                fprintf(stderr, "allocation %ld: cell %d,%d differs after recovery\n", n, x, y);
        }
    }
    assert(same);
}

static const char recovery[] =
    "\033cOK \xe4\xb8\xad e\xcc\x81\r\n\033[1;32mgreen\033[m\033]8;;x:y\033\\L\033]8;;\033\\";

/* Returns true when the workload reached allocation fail_at. */
static bool run(const struct workload *w, long n, bool rest)
{
    const struct wsap_vtcore_options options = w->options;
    long live = live_blocks;
    alloc_seen = 0;
    fail_at = n;
    fail_rest = rest;
    failed = false;

    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    if (t && s)
        w->run(t, s);
    fail_at = -1;
    bool hit = failed;

    if (t && s) {
        /* Memory is back. The terminal must recover with a full reset. */
        struct wsap_vtcore_theme theme;
        wsap_vtcore_theme_high_contrast(&theme);
        wsap_vtcore_set_theme(t, &theme);
        wsap_vtcore_scroll_to(t, 0);
        assert(wsap_vtcore_resize(t, options.size));
        assert(wsap_vtcore_feed(t, recovery, sizeof(recovery) - 1, 0));
        assert(wsap_vtcore_snapshot_update(s, t));
        check_snapshot(s);

        struct wsap_vtcore_options fresh_options = options;
        fresh_options.theme = theme;
        struct wsap_vtcore *fresh = wsap_vtcore_new(&fresh_options);
        struct wsap_vtcore_snapshot *fs = wsap_vtcore_snapshot_new();
        assert(fresh && fs);
        assert(wsap_vtcore_feed(fresh, recovery, sizeof(recovery) - 1, 0));
        assert(wsap_vtcore_snapshot_update(fs, fresh));
        expect_same_screen(s, fs, n);

        char *link = wsap_vtcore_link_at(t, (struct wsap_vtcore_vector2i){ .x = 5, .y = 1 });
        assert(link && !strcmp(link, "x:y"));
        wsap_vtcore_text_free(link);
        assert(wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ 0 }, false,
                                        WSAP_VTCORE_SELECT_LINE));
        char *line = wsap_vtcore_selection_text(t);
        static const char first[] = "OK \xe4\xb8\xad e\xcc\x81";
        assert(line && !strncmp(line, first, sizeof(first) - 1));
        wsap_vtcore_text_free(line);

        wsap_vtcore_snapshot_free(fs);
        wsap_vtcore_free(fresh);
    }
    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    if (live_blocks != live)
        fprintf(stderr, "%s allocation %ld%s: %ld blocks leaked\n", w->name, n,
                rest ? "+" : "", live_blocks - live);
    assert(live_blocks == live);
    return hit;
}

static long sweep(const struct workload *w, bool rest)
{
    long n = 0;
    while (run(w, n, rest))
        n++;
    return n;
}

int main(void)
{
    build_large_inputs();
    for (size_t i = 0; i < sizeof(workloads) / sizeof(workloads[0]); i++) {
        const struct workload *w = &workloads[i];
        assert(!run(w, -1, false));
        long once = sweep(w, false);
        long onward = sweep(w, true);
        assert(once > 0 && once == onward);
        printf("%s: failed each of %ld allocations, once and from there onward\n",
               w->name, once);
    }
    return 0;
}
