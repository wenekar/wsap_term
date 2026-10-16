#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/app.h"
#include "yasl/yasl.h"

/* The app reloads its config without losing what it was started with. */

static char dir[64];

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(content, f) >= 0);
    assert(fclose(f) == 0);
}

/* SIGUSR1 reloads the file the app started with, and the command line
 * still wins over it. */
static void test_reload_keeps_command_line(void)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/term.conf", dir);
    write_file(path, "height = 400\n");
    char width[] = "700";
    char *argv[] = { "wsap_term", "--config", path, "--width", width, NULL };
    int status = 0;
    struct app *a = app_create(5, argv, &status);
    assert(a);
    /* The caller's arguments may not outlive app_create(). */
    strcpy(width, "999");
    assert(a->config.window.size.x == 700 && a->config.window.size.y == 400);

    write_file(path, "height = 500\nwidth = 900\n");
    assert(app_watch_signals(a));
    raise(SIGUSR1);
    uint64_t deadline = yasl_time_ns() + 2000000000ull;
    while (a->config.window.size.y != 500 && yasl_time_ns() < deadline)
        assert(yasl_loop_step(a->loop, 20) >= 0);
    if (a->config.window.size.y != 500 || a->config.window.size.x != 700) {
        fprintf(stderr, "FAIL: after SIGUSR1 the window is %dx%d; the file sets the "
                        "height to 500 and the command line the width to 700\n",
                a->config.window.size.x, a->config.window.size.y);
        exit(1);
    }
    app_destroy(a);
}

int main(void)
{
    unsetenv("WSAP_CONFIG");
    unsetenv("WSAP_WIDTH");
    unsetenv("WSAP_HEIGHT");
    unsetenv("WSAP_THEME");
    snprintf(dir, sizeof(dir), "/tmp/wsap-reload-XXXXXX");
    assert(mkdtemp(dir));
    test_reload_keeps_command_line();
    char cleanup[160];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
    assert(system(cleanup) == 0);
    puts("PASS: SIGUSR1 reloads the config file and keeps the command line");
    return 0;
}
