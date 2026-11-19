#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "input/keybind.h"
#include "session/session.h"
#include "terminal/internal.h"
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

/* Routes the queued input of `w`. Returns the number of shortcut events for
 * command `id`. */
static int drain(struct window *w, input_command_id id)
{
    int runs = 0;
    struct input_event input;
    while (input_queue_take(&w->input, &input)) {
        if (input.kind == INPUT_SHORTCUT && input.shortcut.action == id)
            runs++;
        window_ui_test_route(w, &input);
        input_event_release(&input);
    }
    return runs;
}

/* Sends one key event. A non-NULL `text` is the committed text of the key,
 * as a layout that types a symbol with Alt gives it. */
static int send_key(struct window *w, enum yasl_key code, unsigned mods,
                    uint32_t label, const char *text, unsigned action,
                    input_command_id id)
{
    static uint64_t text_id;
    struct yasl_event event = {
        .type = YASL_EVENT_KEY, .action = action, .mods = mods,
        .key = {
            .key = code, .codepoint = label, .shifted = label,
            .text_input = text ? YASL_KEY_TEXT_COMMIT : YASL_KEY_TEXT_NONE,
            .text_id = text ? ++text_id : 0,
            .consumed_mods = text ? mods & YASL_MOD_ALT : 0,
        },
    };
    window_native_input(w, &event);
    if (text)
        window_native_input(w, &(struct yasl_event){
            .type = YASL_EVENT_TEXT, .text = text, .length = strlen(text),
            .ime = { .key_id = event.key.text_id },
        });
    return drain(w, id);
}

/* Sends SIGUSR1 and runs the loop until window `w` binds ctrl+`cp` to
 * command `id`, or two seconds pass. */
static void reload_until(struct app *a, struct window *w, uint32_t cp,
                         input_command_id id)
{
    raise(SIGUSR1);
    uint64_t deadline = yasl_time_ns() + 2000000000ull;
    while (lookup(w, cp) != id && yasl_time_ns() < deadline)
        assert(yasl_loop_step(a->loop, 20) >= 0);
    assert(lookup(w, cp) == id);
}

/* Asserts that the shell got no byte. */
static void expect_nothing(int fd)
{
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        fprintf(stderr, "FAIL: the shell got %zd bytes:", n);
        for (ssize_t i = 0; i < n; i++)
            fprintf(stderr, " %02x", (unsigned char)buf[i]);
        fputc('\n', stderr);
        exit(1);
    }
    assert(n == -1 && errno == EAGAIN);
}

/* A reload while a bound chord is down does not change the route of that
 * key. Its repeats keep running the command, and the shell gets no text and
 * no release. */
static void test_held_chord_across_reload(const char *dir)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/held.conf", dir);
    write_file(path, "alt+8 = scroll_down\nf6 = scroll_up\nctrl+k = new_tab\n");
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);

    struct test_app t;
    test_app_init(&t, "/bin/cat", (struct vector2i){ .x = 640, .y = 400 });
    assert(app_watch_signals(t.app));
    struct window *w = test_terminal_window(t.app);
    assert(w && app_tabs_new(w));
    struct session *session = terminal_session(app_tabs_active(w));
    int fds[2];
    assert(pipe(fds) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&session->input_writer, session->loop, fds[1]));

    /* The text chord alt+8 types '[' on a TR-Q layout. */
    assert(send_key(w, YASL_KEY_8, YASL_MOD_ALT, '8', "[", YASL_KEY_PRESS,
                    INPUT_COMMAND_SCROLL_DOWN) == 1);
    write_file(path, "alt+8 = scroll_down\nf6 = scroll_up\nctrl+j = new_tab\n");
    reload_until(t.app, w, 'j', INPUT_COMMAND_NEW_TAB);
    assert(send_key(w, YASL_KEY_8, YASL_MOD_ALT, '8', "[", YASL_KEY_REPEAT,
                    INPUT_COMMAND_SCROLL_DOWN) == 1);
    assert(send_key(w, YASL_KEY_8, YASL_MOD_ALT, '8', "[", YASL_KEY_REPEAT,
                    INPUT_COMMAND_SCROLL_DOWN) == 1);
    send_key(w, YASL_KEY_8, YASL_MOD_ALT, '8', NULL, YASL_KEY_RELEASE, 0);
    expect_nothing(fds[0]);

    /* In kitty mode 2 the shell gets key releases. F6 is bound at its
     * press, and the reload unbinds it. The shell gets no repeat and no
     * release of the key. */
    assert(wsap_vtcore_feed(session->term, "\033[>2u", 5, 0));
    assert(send_key(w, YASL_KEY_F6, 0, 0, NULL, YASL_KEY_PRESS,
                    INPUT_COMMAND_SCROLL_UP) == 1);
    write_file(path, "ctrl+k = new_tab\n");
    reload_until(t.app, w, 'k', INPUT_COMMAND_NEW_TAB);
    send_key(w, YASL_KEY_F6, 0, 0, NULL, YASL_KEY_REPEAT, 0);
    send_key(w, YASL_KEY_F6, 0, 0, NULL, YASL_KEY_RELEASE, 0);
    expect_nothing(fds[0]);

    window_close(w);
    test_app_fini(&t);
    close(fds[0]);
    unsetenv("WSAP_INPUT_CONFIG");
}

int main(void)
{
    char dir[] = "/tmp/wsap-input-reload-XXXXXX";
    assert(mkdtemp(dir));
    test_reload_reaches_open_windows(dir);
    test_held_chord_across_reload(dir);
    char cleanup[160];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
    assert(system(cleanup) == 0);
    puts("PASS: SIGUSR1 reloads input.conf in every open window");
    return 0;
}
