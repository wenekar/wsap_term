#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "app/tabs/tabs.h"
#include "helpers/terminal_window.h"
#include "input/keybind.h"
#include "session/session.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/testing.h"
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

static bool f7_bound(struct window *w)
{
    struct input_key_event ev = { .key = INPUT_KEY_F7 };
    return input_keymap_lookup(window_keymap(w), &ev) != INPUT_COMMAND_NONE;
}

static void f7(struct window *w, unsigned action)
{
    window_native_input(w, &(struct yasl_event){
        .type = YASL_EVENT_KEY, .action = action,
        .key = { .key = YASL_KEY_F7, .text_input = YASL_KEY_TEXT_NONE },
    });
    struct input_event input;
    while (input_queue_take(&w->input, &input)) {
        window_ui_test_route(w, &input);
        input_event_release(&input);
    }
}

static void expect_bytes(int fd, const char *want, const char *what)
{
    char got[64];
    ssize_t n = read(fd, got, sizeof(got));
    size_t len = strlen(want);
    bool ok = len ? n == (ssize_t)len && !memcmp(got, want, len) : n < 0;
    if (!ok) {
        fprintf(stderr, "FAIL: %s:", what);
        for (ssize_t i = 0; i < n; i++)
            fprintf(stderr, " %02x", (unsigned char)got[i]);
        fputc('\n', stderr);
        exit(1);
    }
}

/* F7 goes down unbound, so the shell gets the press. A reload binds F7
 * while it is held. The shell still gets the release, so it sees no stuck
 * key. The next press belongs to the binding. */
static void test_reload_keeps_held_key(const char *dir)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/input.conf", dir);
    write_file(path, "ctrl+k = new_tab\n");
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);

    struct test_app t;
    test_app_init(&t, "/bin/cat", (struct vector2i){ .x = 640, .y = 400 });
    struct window *w = test_terminal_window(t.app);
    assert(w && app_tabs_new(w));
    struct session *s = terminal_session(app_tabs_active(w));
    int fds[2];
    assert(pipe(fds) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&s->input_writer, s->loop, fds[1]));
    /* The kitty mode that reports releases. */
    assert(wsap_vtcore_feed(s->term, "\033[>2u", 5, 0));
    assert(!f7_bound(w));

    f7(w, YASL_KEY_PRESS);
    expect_bytes(fds[0], "\033[18;1~", "the shell gets the F7 press");
    write_file(path, "f7 = scroll_up\n");
    assert(app_watch_signals(t.app));
    raise(SIGUSR1);
    uint64_t deadline = yasl_time_ns() + 2000000000ull;
    while (!f7_bound(w) && yasl_time_ns() < deadline)
        assert(yasl_loop_step(t.app->loop, 20) >= 0);
    assert(f7_bound(w));
    f7(w, YASL_KEY_RELEASE);
    expect_bytes(fds[0], "\033[18;1:3~",
                 "the shell gets the F7 release after a reload binds F7");

    f7(w, YASL_KEY_PRESS);
    f7(w, YASL_KEY_RELEASE);
    expect_bytes(fds[0], "", "a bound F7 sends nothing to the shell");

    window_close(w);
    test_app_fini(&t);
    close(fds[0]);
    close(fds[1]);
    unsetenv("WSAP_INPUT_CONFIG");
}

int main(void)
{
    char dir[] = "/tmp/wsap-input-reload-XXXXXX";
    assert(mkdtemp(dir));
    test_reload_reaches_open_windows(dir);
    test_reload_keeps_held_key(dir);
    char cleanup[160];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
    assert(system(cleanup) == 0);
    puts("PASS: SIGUSR1 reloads input.conf in every open window, and a "
         "held key keeps its owner");
    return 0;
}
