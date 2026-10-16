/*
 * Compare the terminal with the reference model (model.c) through the
 * public API: every history page and screen row through a snapshot, the
 * cursor, the modes, the history size, and the reply bytes.
 *
 * Each seed makes a random geometry and history limit, then runs four
 * steps of generated input. The terminal gets each step in random chunks,
 * so the fast paths and the byte parser both run. One step in four ends
 * with a random resize. Every step ends with queries (DSR, DECRQSS,
 * DECRQM), so hidden state such as the pen, the margins, and the modes
 * shows in the replies.
 *
 * On a mismatch the test shrinks the input, prints a short C string that
 * reproduces it, and fails.
 *
 * Usage: model-test [seeds] [first-seed]
 */
#include "model.h"
#include "fuzz_gen.h"
#include "wsap_vtcore/wsap_vtcore.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Queries that make hidden state visible. They change nothing. */
static const char probe[] =
    "\033[6n\033P$qm\033\\\033P$qr\033\\\033P$q q\033\\"
    "\033[?6$p\033[?7$p\033[4$p\033[20$p";

struct event {
    char *bytes;        /* NULL for a resize */
    size_t len;
    unsigned flags;
    struct wsap_vtcore_vector2i size;
};

struct script {
    struct wsap_vtcore_vector2i size;
    int history;
    struct event *ev;
    int n, cap;
};

static void script_add(struct script *s, struct event e)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->ev = realloc(s->ev, (size_t)s->cap * sizeof(*s->ev));
        if (!s->ev) abort();
    }
    s->ev[s->n++] = e;
}

static void script_free(struct script *s)
{
    for (int i = 0; i < s->n; i++) free(s->ev[i].bytes);
    free(s->ev);
    *s = (struct script){0};
}

static char *dup_bytes(const char *p, size_t n)
{
    char *d = malloc(n ? n : 1);
    if (!d) abort();
    memcpy(d, p, n);
    return d;
}

struct replies {
    char *s;
    size_t len, cap;
};

static void on_reply(void *ud, const char *bytes, size_t len)
{
    struct replies *r = ud;
    if (r->len + len > r->cap) {
        r->cap = (r->len + len) * 2;
        r->s = realloc(r->s, r->cap);
        if (!r->s) abort();
    }
    memcpy(r->s + r->len, bytes, len);
    r->len += len;
}

static void dump(FILE *f, const char *s, size_t n)
{
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 27) fputs("\\033", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c >= 32 && c < 127) fputc(c, f);
        else fprintf(f, "\\x%02x\"\"", c);
    }
    fputc('"', f);
}

static char glyph(uint32_t cp)
{
    return cp >= 32 && cp < 127 ? (char)cp : cp ? '?' : '.';
}

/* Print every retained row of both, side by side. */
static void dump_all(struct model *m, struct wsap_vtcore *t, struct wsap_vtcore_snapshot *snap, FILE *log)
{
    int total = wsap_vtcore_scrollback(t).total, mtotal = model_history(m);
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t), msize = model_size(m);
    int rows = total + size.y > mtotal + msize.y ? total + size.y : mtotal + msize.y;
    fprintf(log, "  term (history %d)%*s model (history %d)\n", total, size.x > 8 ? size.x - 8 : 0,
            "", mtotal);
    for (int r = 0; r < rows; r++) {
        fprintf(log, "  %3d |", r);
        if (r < total + size.y) {
            int offset = r < total ? total - r : 0;
            wsap_vtcore_scroll_to(t, offset);
            wsap_vtcore_snapshot_update(snap, t);
            const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, r < total ? 0 : r - total);
            for (int x = 0; x < size.x; x++) {
                uint32_t cp[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
                if (row[x].text) wsap_vtcore_snapshot_cluster(snap, row[x].text, cp, WSAP_VTCORE_GRAPHEME_MAX_LEN);
                fputc(glyph(cp[0]), log);
            }
        } else {
            fprintf(log, "%*s", size.x, "");
        }
        fprintf(log, "| |");
        if (r < mtotal + msize.y) {
            const struct model_cell *row = model_row(m, r);
            for (int x = 0; x < msize.x; x++) fputc(glyph(row[x].n ? row[x].cp[0] : 0), log);
        }
        fprintf(log, "|\n");
    }
    wsap_vtcore_scroll_to(t, 0);
    struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t), mc = model_cursor(m);
    fprintf(log, "  cursor term %d,%d model %d,%d\n", c.pos.x, c.pos.y, mc.pos.x, mc.pos.y);
}

/* Returns NULL when the terminal matches the model, else a description.
 * Details go to log when it is not NULL. */
static const char *compare(struct model *m, struct wsap_vtcore *t, struct wsap_vtcore_snapshot *snap,
                           struct replies *got, char *why, size_t cap, FILE *log)
{
    size_t want_len;
    const char *want = model_replies(m, &want_len);
    if (want_len != got->len || (want_len && memcmp(want, got->s, want_len))) {
        if (log) {
            fprintf(log, "replies: term ");
            dump(log, got->s ? got->s : "", got->len);
            fprintf(log, "\n         model ");
            dump(log, want, want_len);
            fputc('\n', log);
        }
        snprintf(why, cap, "replies differ");
        return why;
    }
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t), msize = model_size(m);
    if (size.x != msize.x || size.y != msize.y) {
        snprintf(why, cap, "size %dx%d, model %dx%d", size.x, size.y, msize.x, msize.y);
        return why;
    }
    struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t), mc = model_cursor(m);
    if (c.pos.x != mc.pos.x || c.pos.y != mc.pos.y || c.style != mc.style ||
        c.visible != mc.visible || c.blink != mc.blink) {
        snprintf(why, cap, "cursor %d,%d style %d visible %d blink %d; model %d,%d %d %d %d",
                 c.pos.x, c.pos.y, c.style, c.visible, c.blink,
                 mc.pos.x, mc.pos.y, mc.style, mc.visible, mc.blink);
        return why;
    }
    struct wsap_vtcore_modes md = wsap_vtcore_modes(t), mm = model_modes(m);
    if (md.keyboard != mm.keyboard || md.mouse != mm.mouse ||
        md.mouse_encoding != mm.mouse_encoding || md.app_cursor != mm.app_cursor ||
        md.app_keypad != mm.app_keypad || md.backarrow_bs != mm.backarrow_bs ||
        md.bracketed_paste != mm.bracketed_paste || md.focus_events != mm.focus_events ||
        md.alternate != mm.alternate || md.reverse_video != mm.reverse_video ||
        md.synchronized != mm.synchronized) {
        snprintf(why, cap, "modes differ (alternate %d/%d reverse %d/%d keypad %d/%d)",
                 md.alternate, mm.alternate, md.reverse_video, mm.reverse_video,
                 md.app_keypad, mm.app_keypad);
        return why;
    }
    /* Visit each retained row. Wide-line totals can start as estimates. */
    int total = model_history(m);
    wsap_vtcore_scroll_to(t, 0);
    wsap_vtcore_scroll(t, INT_MAX);
    for (int offset = total;; offset = offset > size.y ? offset - size.y : 0) {
        if (!offset)
            wsap_vtcore_scroll_to(t, 0);
        if (!wsap_vtcore_snapshot_update(snap, t)) {
            snprintf(why, cap, "snapshot update failed");
            return why;
        }
        for (int y = 0; y < size.y; y++) {
            const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, y);
            int retained = total - offset + y;
            const struct model_cell *want_row = model_row(m, retained);
            for (int x = 0; x < size.x; x++) {
                const struct model_cell *e = &want_row[x];
                uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
                int n = row[x].text ? wsap_vtcore_snapshot_cluster(snap, row[x].text, cps,
                                                            WSAP_VTCORE_GRAPHEME_MAX_LEN) : 0;
                if (n == e->n && row[x].width == e->width && row[x].attrs == e->attrs &&
                    row[x].fg == e->fg && row[x].bg == e->bg &&
                    !memcmp(cps, e->cp, (size_t)n * sizeof(*cps)))
                    continue;
                if (log) {
                    fprintf(log, "retained row %d (offset %d):\n  term:  ", retained, offset);
                    for (int k = 0; k < size.x; k++) {
                        uint32_t cp[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
                        if (row[k].text)
                            wsap_vtcore_snapshot_cluster(snap, row[k].text, cp, WSAP_VTCORE_GRAPHEME_MAX_LEN);
                        fputc(glyph(cp[0]), log);
                    }
                    fprintf(log, "\n  model: ");
                    want_row = model_row(m, retained);
                    for (int k = 0; k < size.x; k++)
                        fputc(glyph(want_row[k].n ? want_row[k].cp[0] : 0), log);
                    fputc('\n', log);
                    e = &want_row[x];
                }
                snprintf(why, cap,
                         "cell %d,%d: term U+%04X (%d cps) w%u attrs %#x fg %#x bg %#x; "
                         "model U+%04X (%u cps) w%u attrs %#x fg %#x bg %#x",
                         x, y, cps[0], n, row[x].width, row[x].attrs, row[x].fg, row[x].bg,
                         e->cp[0], e->n, e->width, e->attrs, e->fg, e->bg);
                wsap_vtcore_scroll_to(t, 0);
                return why;
            }
        }
        if (!offset) break;
        wsap_vtcore_scroll(t, -(offset > size.y ? size.y : offset));
    }
    int observed_total = wsap_vtcore_scrollback(t).total;
    if (observed_total != total) {
        snprintf(why, cap, "visited history %d rows, model %d", observed_total, total);
        return why;
    }
    wsap_vtcore_scroll_to(t, 0);
    return NULL;
}

enum chunking { CHUNK_RANDOM, CHUNK_WHOLE, CHUNK_BYTES };

/* Run a script. Returns the event index of the first mismatch, or -1. */
static int run(const struct script *s, enum chunking chunks, uint64_t chunk_seed,
               char *why, size_t cap, bool verbose)
{
    struct replies got = {0};
    struct wsap_vtcore_options opt = {
        .size = s->size, .scrollback = s->history,
        .callbacks = { .ud = &got, .reply = on_reply },
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    struct model *m = model_new(s->size, s->history, NULL);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    if (!t || !snap) abort();
    uint64_t saved_rng = rng;
    rng = chunk_seed | 1;
    int bad = -1;
    for (int i = 0; i < s->n && bad < 0; i++) {
        const struct event *e = &s->ev[i];
        if (e->bytes) {
            model_feed(m, e->bytes, e->len, e->flags);
            for (size_t k = 0; k < e->len;) {
                size_t n = chunks == CHUNK_WHOLE ? e->len - k
                         : chunks == CHUNK_BYTES ? 1
                         : 1 + (size_t)rn((int)(e->len - k));
                wsap_vtcore_feed(t, e->bytes + k, n, e->flags);
                k += n;
            }
        } else {
            bool a = model_resize(m, e->size);
            bool b = wsap_vtcore_resize(t, e->size);
            if (a != b) {
                snprintf(why, cap, "resize result differs");
                bad = i;
                break;
            }
        }
        if (compare(m, t, snap, &got, why, cap, verbose ? stderr : NULL)) {
            bad = i;
            if (verbose) dump_all(m, t, snap, stderr);
        }
        model_take_replies(m);
        got.len = 0;
    }
    rng = saved_rng;
    free(got.s);
    wsap_vtcore_snapshot_free(snap);
    model_free(m);
    wsap_vtcore_free(t);
    return bad;
}

static bool fails(const struct script *s, char *why, size_t cap)
{
    return run(s, CHUNK_WHOLE, 1, why, cap, false) >= 0 ||
           run(s, CHUNK_BYTES, 1, why, cap, false) >= 0;
}

/* Remove byte ranges and events while the script still fails. */
static void shrink(struct script *s)
{
    char why[512];
    for (int i = s->n - 1; i >= 0; i--) {
        struct event e = s->ev[i];
        memmove(s->ev + i, s->ev + i + 1, (size_t)(s->n - i - 1) * sizeof(*s->ev));
        s->n--;
        if (fails(s, why, sizeof(why))) {
            free(e.bytes);
            continue;
        }
        memmove(s->ev + i + 1, s->ev + i, (size_t)(s->n - i) * sizeof(*s->ev));
        s->ev[i] = e;
        s->n++;
    }
    for (int i = 0; i < s->n; i++) {
        struct event *e = &s->ev[i];
        if (!e->bytes) continue;
        for (size_t step = e->len / 2 ? e->len / 2 : 1; step >= 1; step /= 2) {
            for (size_t at = 0; at + step <= e->len;) {
                char *keep = dup_bytes(e->bytes, e->len);
                size_t keep_len = e->len;
                memmove(e->bytes + at, e->bytes + at + step, e->len - at - step);
                e->len -= step;
                if (fails(s, why, sizeof(why))) {
                    free(keep);
                    continue;
                }
                free(e->bytes);
                e->bytes = keep;
                e->len = keep_len;
                at++;
            }
            if (step == 1) break;
        }
    }
}

static void report(struct script *s, int seed, const char *why)
{
    fprintf(stderr, "seed %d: %s\n", seed, why);
    shrink(s);
    char buf2[512];
    fprintf(stderr, "shrunk: %dx%d history %d\n", s->size.x, s->size.y, s->history);
    for (int i = 0; i < s->n; i++) {
        if (s->ev[i].bytes) {
            fprintf(stderr, "  feed%s ", s->ev[i].flags ? " (ONLCR)" : "");
            dump(stderr, s->ev[i].bytes, s->ev[i].len);
            fputc('\n', stderr);
        } else {
            fprintf(stderr, "  resize %dx%d\n", s->ev[i].size.x, s->ev[i].size.y);
        }
    }
    int at = run(s, CHUNK_WHOLE, 1, buf2, sizeof(buf2), true);
    if (at < 0) at = run(s, CHUNK_BYTES, 1, buf2, sizeof(buf2), true);
    fprintf(stderr, "shrunk mismatch after event %d: %s\n", at, buf2);
}

/* Fixed cases that the generator reaches rarely. */
static int fixed_cases(void)
{
    static const struct {
        int cols, rows, history;
        const char *input;
    } cases[] = {
        { 12, 3, 5, "a中e\xcc\x81日本 \033[1;3;4;9;38;5;123;48;2;1;2;3mQ\033[0mR" },
        { 8, 4, 5, "\033[2;3r\033[?6hZ\nQ\nR\033[?6l\033[r" },
        { 12, 3, 50, "one two three four five\r\nsix seven eight\r\nnine" },
        { 10, 3, 50, "\033]133;A\007$ ls -la /usr/share\r\nout1\r\n\033]133;A\007$ " },
        { 6, 3, 5, "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\xe2\x9d\xa4\xef\xb8\x8f" "ab\xe2\x98\xba\xef\xb8\x8e" },
        { 8, 3, 5, "\033(0lqqk\033(Bx\x0e" "abc\x0f" "abc" },
        { 8, 3, 5, "abcdefgh\033[3;4H\033[31;1m\0337\033[0m\033[H\0338X" },
        { 6, 2, 5, "abcdef\xe3\x80z\xed\xa0\x80\x80" },
    };
    static const struct wsap_vtcore_vector2i sizes[] = {
        { .x = 5, .y = 3 }, { .x = 16, .y = 4 }, { .x = 7, .y = 3 }, { .x = 20, .y = 5 },
        { .x = 1, .y = 2 }, { .x = 3, .y = 6 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct script s = {
            .size = { .x = cases[i].cols, .y = cases[i].rows },
            .history = cases[i].history,
        };
        script_add(&s, (struct event){
            .bytes = dup_bytes(cases[i].input, strlen(cases[i].input)),
            .len = strlen(cases[i].input) });
        script_add(&s, (struct event){ .bytes = dup_bytes(probe, sizeof(probe) - 1),
                                       .len = sizeof(probe) - 1 });
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
            script_add(&s, (struct event){ .size = sizes[k] });
        char why[512];
        int at = run(&s, CHUNK_BYTES, 1, why, sizeof(why), true);
        if (at >= 0) {
            fprintf(stderr, "fixed case %zu event %d: %s\n", i, at, why);
            report(&s, -(int)i, why);
            script_free(&s);
            return 1;
        }
        script_free(&s);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 20000;
    int first = argc > 2 ? atoi(argv[2]) : 1;
    if (fixed_cases()) return 1;
    static const int limits[] = { 0, 5, 50, 4096 };
    for (int seed = first; seed < first + seeds; seed++) {
        rng = 0x9e3779b97f4a7c15ull * (uint64_t)seed;
        struct script s = {
            .size = { .x = 4 + rn(37), .y = 2 + rn(11) },
            .history = limits[rn(4)],
        };
        struct wsap_vtcore_vector2i size = s.size;
        for (int step = 0; step < 4; step++) {
            blen = 0;
            gen(10 + rn(30), size.x, size.y);
            put(probe);
            unsigned flags = rn(5) == 0 ? WSAP_VTCORE_FEED_ONLCR : 0;
            script_add(&s, (struct event){ .bytes = dup_bytes(buf, blen), .len = blen,
                                           .flags = flags });
            if (rn(4) == 0) {
                size = (struct wsap_vtcore_vector2i){ .x = 1 + rn(44), .y = 1 + rn(14) };
                script_add(&s, (struct event){ .size = size });
            }
        }
        if (getenv("MODEL_TEST_PRINT")) {
            fprintf(stderr, "seed %d: %dx%d history %d\n", seed, s.size.x, s.size.y, s.history);
            for (int i = 0; i < s.n; i++) {
                if (!s.ev[i].bytes) {
                    fprintf(stderr, "  resize %dx%d\n", s.ev[i].size.x, s.ev[i].size.y);
                    continue;
                }
                fprintf(stderr, "  feed ");
                dump(stderr, s.ev[i].bytes, s.ev[i].len);
                fputc('\n', stderr);
            }
        }
        char why[512];
        uint64_t chunk_seed = rng;
        if (run(&s, CHUNK_RANDOM, chunk_seed, why, sizeof(why), false) >= 0) {
            run(&s, CHUNK_RANDOM, chunk_seed, why, sizeof(why), true);
            report(&s, seed, why);
            script_free(&s);
            return 1;
        }
        script_free(&s);
    }
    return 0;
}
