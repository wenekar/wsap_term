#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "input/keybind.h"
#include "yasl/yasl.h"

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f);
    int written = fputs(content, f);
    int closed = fclose(f);
    assert(written >= 0 && closed == 0);
}

static input_command_id lookup(struct window *w, uint32_t codepoint)
{
    struct input_key_event ev = { .codepoint = codepoint, .mods = INPUT_MOD_CTRL };
    return input_keymap_lookup(window_keymap(w), &ev);
}

/* SIGUSR1 loads input.conf again in every open window. Both windows then
 * have the new binding, and neither keeps the binding of the old file. */
static void test_reload_reaches_open_windows(const char *dir)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/input.conf", dir);
    write_file(path, "ctrl+k = new_tab\n");
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);

    struct test_app t;
    test_app_init(&t, "/bin/cat", (struct vector2i){ .x = 640, .y = 400 });
    struct window *one = test_terminal_window(t.app);
    struct window *two = test_terminal_window(t.app);
    assert(one && two);
    assert(lookup(one, 'k') == INPUT_COMMAND_NEW_TAB);
    assert(lookup(two, 'k') == INPUT_COMMAND_NEW_TAB);

    write_file(path, "ctrl+j = new_tab\n");
    assert(app_watch_signals(t.app));
    raise(SIGUSR1);
    uint64_t deadline = yasl_time_ns() + 2000000000ull;
    while ((lookup(one, 'j') != INPUT_COMMAND_NEW_TAB ||
            lookup(two, 'j') != INPUT_COMMAND_NEW_TAB) &&
           yasl_time_ns() < deadline)
        assert(yasl_loop_step(t.app->loop, 20) >= 0);

    struct window *windows[] = { one, two };
    for (int i = 0; i < 2; i++) {
        if (lookup(windows[i], 'j') != INPUT_COMMAND_NEW_TAB ||
            lookup(windows[i], 'k') != INPUT_COMMAND_NONE) {
            fprintf(stderr, "FAIL: after SIGUSR1 window %d has ctrl+j=%u "
                            "ctrl+k=%u; the file binds only ctrl+j\n",
                    i + 1, lookup(windows[i], 'j'), lookup(windows[i], 'k'));
            exit(1);
        }
    }
    /* A window that opens after the reload has the same app keymap. */
    struct window *three = test_terminal_window(t.app);
    assert(three);
    assert(window_keymap(three)->n == window_keymap(one)->n);
    assert(lookup(three, 'j') == INPUT_COMMAND_NEW_TAB);

    window_close(three);
    window_close(two);
    window_close(one);
    test_app_fini(&t);
    unsetenv("WSAP_INPUT_CONFIG");
}

int main(void)
{
    char dir[] = "/tmp/wsap-input-reload-XXXXXX";
    assert(mkdtemp(dir));
    test_reload_reaches_open_windows(dir);
    char cleanup[160];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
    assert(system(cleanup) == 0);
    puts("PASS: SIGUSR1 reloads input.conf in every open window");
    return 0;
}
