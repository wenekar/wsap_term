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

static double print_render_max(struct frame_stats *st)
{
    char line[512] = {0};
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    FILE *cap = tmpfile();
    assert(saved >= 0 && cap);
    dup2(fileno(cap), STDOUT_FILENO);
    frame_stats_print(st, 0);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(cap);
    size_t n = fread(line, 1, sizeof(line) - 1, cap);
    fclose(cap);
    line[n] = '\0';

    const char *render = strstr(line, "render(us)");
    assert(render);
    const char *max = strstr(render, "max=");
    assert(max);
    return strtod(max + strlen("max="), NULL);
}

int main(void)
{
    struct frame_stats st = {0};

    frame(&st, 1000);
    frame(&st, SPIKE_NS);
    frame(&st, 1000);
    assert(print_render_max(&st) >= SPIKE_NS / 1000.0);

    for (int i = 0; i < 5; i++)
        frame(&st, 1000);
    assert(print_render_max(&st) < QUIET_LIMIT_US);

    return 0;
}
