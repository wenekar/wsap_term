#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session/session.h"
#include "util/time.h"
#include "wsap_vtcore/wsap_vtcore.h"
#include "yasl/yasl.h"

/*
 * A test of a flood of PTY output.
 *
 * The child writes more output than the terminal parses in one pass, so the
 * session serves it in several passes and returns to the loop between them. The
 * loop also serves input and timers there.
 *
 * The output is one numbered line for each 32 bytes of the flood, and the
 * scrollback holds every line. Every line must reach the terminal once, in
 * order, so no pass lost, repeated, or moved a byte. The exit event comes after
 * the last line, so the end of the child does not overtake its output.
 *
 * This test holds no timing result: a pass ends on a budget, but the parse of
 * one chunk can overrun it, and the machine decides the rest. benchmarks/ holds
 * the speed measurements.
 */

/* A line of 32 bytes: seven digits, 24 letters, and a newline. The grid is
 * wide enough that a line does not wrap. */
#define LINE_BYTES 32u
#define LINE_LETTERS 24u
#define LINE_TEXT (LINE_BYTES - 1u)
#define FLOOD_LINES 60000u
#define GRID_COLS 40

struct watch {
    struct session *session;
    struct yasl_loop *loop;
    bool exited;
    unsigned passes;
    /* The last line number that the terminal showed when the child exited. */
    long line_at_exit;
};

static void on_output(void *ud)
{
    struct watch *w = ud;
    w->passes++;
}

/* The number of the line that a row of the terminal shows, or -1 for another
 * row. A row of the flood is seven digits, 24 letters, and a space where the
 * newline was. */
static long row_line(const char *row)
{
    size_t length = strlen(row);
    /* A screen row keeps the blank cell after the text, and a history row is
     * trimmed. */
    if (length == LINE_TEXT + 1 && row[LINE_TEXT] == ' ')
        length = LINE_TEXT;
    if (length != LINE_TEXT)
        return -1;
    for (unsigned i = 0; i < LINE_TEXT; i++) {
        char c = row[i];
        if (i < 7 ? (c < '0' || c > '9') : c != 'x')
            return -1;
    }
    return strtol(row, NULL, 10);
}

/* The text of one visible row, without trailing blanks. Returns false without a
 * row. */
static bool screen_row(struct wsap_vtcore *term, int y, char *out, size_t cap)
{
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    if (!snap || !wsap_vtcore_snapshot_update(snap, term)) {
        wsap_vtcore_snapshot_free(snap);
        return false;
    }
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(snap);
    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snap, y);
    bool ok = view && cells && y >= 0 && y < view->size.y;
    if (ok) {
        size_t used = 0;
        for (int x = 0; x < view->size.x && used + 1 < cap; x++) {
            uint32_t text = cells[x].text;
            out[used++] = text >= 0x20 && text < 0x7F ? (char)text : ' ';
        }
        while (used && out[used - 1] == ' ')
            used--;
        out[used] = 0;
    }
    wsap_vtcore_snapshot_free(snap);
    return ok;
}

/* The last line of the flood that the terminal shows, or -1. */
static long last_line(struct wsap_vtcore *term)
{
    char row[128];
    for (int y = 23; y >= 0; y--) {
        if (!screen_row(term, y, row, sizeof row))
            continue;
        long line = row_line(row);
        if (line >= 0)
            return line;
    }
    return -1;
}

static void on_exited(void *ud)
{
    struct watch *w = ud;
    w->exited = true;
    /* The child is gone, so every byte that it wrote has arrived. */
    w->line_at_exit = last_line(w->session->term);
    yasl_loop_quit(w->loop);
}

static uint64_t on_timeout(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    yasl_loop_quit(ud);
    return 0;
}

static void on_signal(void *ud, int signo)
{
    (void)ud;
    if (signo == SIGCHLD)
        session_reap_exited();
}

static const struct session_host HOST = { .output = on_output, .exited = on_exited };

/* Writes the flood to a file, so that the child can `cat` it. */
static int make_flood(char *path)
{
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    static char block[LINE_BYTES * 256];
    unsigned used = 0;
    for (unsigned line = 0; line < FLOOD_LINES; line++) {
        char *at = block + (size_t)used * LINE_BYTES;
        snprintf(at, LINE_BYTES, "%07u", line);
        memset(at + 7, 'x', LINE_LETTERS);
        at[LINE_BYTES - 1] = '\n';
        if (++used == 256) {
            if (write(fd, block, sizeof block) != (ssize_t)sizeof block) {
                close(fd);
                return -1;
            }
            used = 0;
        }
    }
    if (used && write(fd, block, (size_t)used * LINE_BYTES) != (ssize_t)used * (ssize_t)LINE_BYTES) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* Every line of the flood reached the terminal once, in order. */
static int check_history(struct session *s)
{
    char *history = wsap_vtcore_history_text(s->term, (int)FLOOD_LINES);
    if (!history) {
        fprintf(stderr, "FAIL: no history text\n");
        return 1;
    }
    long expected = 0;
    long last = -1;
    char *line = history;
    int rows = 0;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        long shown = row_line(line);
        if (shown != expected) {
            fprintf(stderr, "FAIL: history row %d shows line %ld, expected %ld\n", rows, shown,
                    expected);
            free(history);
            return 1;
        }
        last = shown;
        expected++;
        rows++;
        line = end ? end + 1 : NULL;
    }
    free(history);
    if (rows < (int)FLOOD_LINES / 2) {
        fprintf(stderr, "FAIL: the history holds %d of %u lines\n", rows, FLOOD_LINES);
        return 1;
    }
    /* The flood starts at line 0 and runs on to the screen. */
    if (last + 1 >= (long)FLOOD_LINES) {
        fprintf(stderr, "FAIL: the history ends at line %ld of %u\n", last, FLOOD_LINES);
        return 1;
    }
    return 0;
}

/* The lines on screen follow the history without a gap. */
static int check_screen(struct session *s, long line_at_exit)
{
    char row[128];
    long expected = line_at_exit;
    int checked = 0;
    for (int y = 23; y >= 0; y--) {
        if (!screen_row(s->term, y, row, sizeof row))
            continue;
        long shown = row_line(row);
        if (shown < 0) {
            /* The last line of the file ends with a newline, so the bottom row
             * of the screen is empty. */
            if (!checked)
                continue;
            break;
        }
        if (shown != expected) {
            fprintf(stderr, "FAIL: screen row %d shows line %ld, expected %ld\n", y, shown,
                    expected);
            return 1;
        }
        expected--;
        checked++;
    }
    if (checked < 12) {
        fprintf(stderr, "FAIL: the screen shows %d lines of the flood\n", checked);
        return 1;
    }
    return 0;
}

int main(void)
{
    char path[] = "/tmp/wsap-drain-flood-XXXXXX";
    if (make_flood(path) < 0) {
        fprintf(stderr, "FAIL: no flood file\n");
        return 1;
    }
    char command[64];
    snprintf(command, sizeof command, "/bin/sh -c 'cat %s'", path);

    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    assert(yasl_loop_watch_signals(loop, (const int[]){SIGCHLD}, 1, on_signal, NULL) == 0);

    static alignas(64) struct session s;
    struct watch w = { .session = &s, .loop = loop, .line_at_exit = -1 };
    assert(session_init(&s, loop, &(struct session_spec){
        .command = command,
        .grid = { .x = GRID_COLS, .y = 24 },
        .scrollback = FLOOD_LINES,
    }));
    session_set_host(&s, &HOST, &w);
    assert(yasl_loop_add_timer(loop, MS_TO_NS(120000), on_timeout, loop));
    assert(yasl_loop_run(loop) == 0);

    int status = 0;
    if (!w.exited) {
        fprintf(stderr, "FAIL: the flood did not finish\n");
        status = 1;
    } else if (w.passes < 2) {
        /* One pass served the whole flood, so the loop did not return between
         * the parts of it. */
        fprintf(stderr, "FAIL: one pass served the whole flood (%u)\n", w.passes);
        status = 1;
    } else if (w.line_at_exit != (long)FLOOD_LINES - 1) {
        fprintf(stderr, "note: %d history rows, %u passes\n",
                wsap_vtcore_history_rows(s.term), w.passes);
        fprintf(stderr, "FAIL: the exit came with line %ld on screen, expected %u\n",
                w.line_at_exit, FLOOD_LINES - 1);
        status = 1;
    } else {
        status = check_history(&s);
        if (status == 0)
            status = check_screen(&s, w.line_at_exit);
    }
    session_fini(&s);
    yasl_loop_unwatch_signals(loop);
    yasl_loop_destroy(loop);
    unlink(path);
    return status;
}
