#define _POSIX_C_SOURCE 200809L

#include "yasl/yasl.h"
#include "ui/yasl/stats.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPIKE_NS 40000000ull
#define QUIET_LIMIT_US 20000.0

static void frame(struct frame_stats *st, uint64_t hold_ns)
{
    render_begin(st);
    uint64_t until = yasl_time_ns() + hold_ns;
    while (yasl_time_ns() < until)
        ;
    render_end_and_count_frame(st);
}

/* The line of one report, in `line`. */
static void print_line(struct frame_stats *st, char *line, size_t size)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    FILE *cap = tmpfile();
    assert(saved >= 0 && cap);
    dup2(fileno(cap), STDOUT_FILENO);
    frame_stats_print(st);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(cap);
    size_t n = fread(line, 1, size - 1, cap);
    fclose(cap);
    line[n] = '\0';
}

/* The value of `key=` in the report line. */
static unsigned long long reported(const char *line, const char *key)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *at = strstr(line, pattern);
    assert(at);
    return strtoull(at + strlen(pattern), NULL, 10);
}

static double print_render_max(struct frame_stats *st)
{
    char line[512];
    print_line(st, line, sizeof(line));
    const char *render = strstr(line, "render(us)");
    assert(render);
    const char *max = strstr(render, "max=");
    assert(max);
    return strtod(max + strlen("max="), NULL);
}

int main(void)
{
    struct frame_stats st = { .started_ns = yasl_time_ns(), .frame_min_ms = 1e18 };

    frame(&st, 1000);
    frame(&st, SPIKE_NS);
    frame(&st, 1000);
    assert(print_render_max(&st) >= SPIKE_NS / 1000.0);

    for (int i = 0; i < 5; i++)
        frame(&st, 1000);
    assert(print_render_max(&st) < QUIET_LIMIT_US);

    /* The counts stay apart: a draw can be dropped, submitted, or presented,
     * and a submitted frame is not presented yet. */
    struct frame_stats counted = { .started_ns = yasl_time_ns(), .frame_min_ms = 1e18 };
    for (int i = 0; i < 4; i++)
        frame(&counted, 1000);
    counted.frames_submitted = 3;
    counted.frames_dropped = 1;
    counted.frames_presented = 2;
    char line[512];
    print_line(&counted, line, sizeof(line));
    assert(reported(line, "frames") == 4);
    assert(reported(line, "drop") == 1);
    assert(reported(line, "submit") == 3);
    assert(reported(line, "present") == 2);

    /* A presentation that the display reports counts once, and a watch outlives
     * the window reference that created it. */
    struct present_watch *watch = present_watch_new();
    assert(watch);
    assert(present_watch_take_presented(watch) == 0);
    present_watch_note_presented(watch);
    present_watch_note_presented(watch);
    assert(present_watch_take_presented(watch) == 2);
    assert(present_watch_take_presented(watch) == 0);
    /* The frame that waits for the display holds a reference of its own. */
    struct present_watch *handler = present_watch_ref(watch);
    present_watch_unref(watch);
    present_watch_note_presented(handler);
    assert(present_watch_take_presented(handler) == 1);
    present_watch_unref(handler);

    /* No watch is not an error. */
    present_watch_note_presented(NULL);
    assert(present_watch_take_presented(NULL) == 0);
    present_watch_unref(NULL);

    return 0;
}
