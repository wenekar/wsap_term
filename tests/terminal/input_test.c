#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "input/keybind.h"
#include "session/session.h"
#include "ui/debug.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/testing.h"

static void commit(struct window *w, const char *value, uint64_t key_id,
                    enum yasl_event_type type)
{
    window_native_input(w, &(struct yasl_event){
        .type = type,
        .text = value,
        .length = strlen(value),
        .ime = { .key_id = key_id },
    });
}

static void text(struct window *w, const char *value)
{
    commit(w, value, 0, YASL_EVENT_TEXT_EDIT);
}

static void drain(struct window *w)
{
    struct input_event input;
    while (input_queue_take(&w->input, &input)) {
        window_ui_test_route(w, &input);
        input_event_release(&input);
    }
}

static void key_text(struct window *w, enum yasl_key code, unsigned mods,
                uint32_t label, uint32_t shifted, const char *value,
                unsigned action, enum yasl_event_type type)
{
    static uint64_t text_id;
    struct yasl_event event = {
        .type = YASL_EVENT_KEY,
        .action = action,
        .mods = mods,
        .key = {
            .key = code, .codepoint = label, .shifted = shifted,
            .text_input = value ? YASL_KEY_TEXT_COMMIT : YASL_KEY_TEXT_NONE,
            .text_id = value ? ++text_id : 0,
            .consumed_mods = value ? mods & (YASL_MOD_ALT | YASL_MOD_CTRL | YASL_MOD_SUPER) : 0,
        },
    };
    window_native_input(w, &event);
    if (value)
        commit(w, value, event.key.text_id, type);
    event.action = YASL_KEY_RELEASE;
    event.key.text_id = 0;
    window_native_input(w, &event);
    drain(w);
}

static void key(struct window *w, enum yasl_key code, unsigned mods,
                uint32_t label, uint32_t shifted, const char *value, unsigned action)
{
    key_text(w, code, mods, label, shifted, value, action, YASL_EVENT_TEXT_EDIT);
}

static void composing_key(struct window *w, enum yasl_key code, const char *value)
{
    struct yasl_event event = {
        .type = YASL_EVENT_KEY, .action = YASL_KEY_PRESS,
        .key = { .key = code, .codepoint = 'e', .text_input = YASL_KEY_TEXT_COMPOSITION },
    };
    window_native_input(w, &event);
    if (value) text(w, value);
    event.action = YASL_KEY_RELEASE;
    window_native_input(w, &event);
    drain(w);
}

static void expect(int fd, const char *value)
{
    char received[128];
    ssize_t length = read(fd, received, sizeof(received));
    if (!value[0]) {
        assert(length == -1 && errno == EAGAIN);
        return;
    }
    if (length != (ssize_t)strlen(value) || memcmp(received, value, (size_t)length)) {
        fprintf(stderr, "Expected terminal input: %s\nReceived bytes:", value);
        for (ssize_t i = 0; i < length; i++)
            fprintf(stderr, " %02x", (unsigned char)received[i]);
        fputc('\n', stderr);
        assert(false);
    }
    assert(read(fd, received, sizeof(received)) == -1 && errno == EAGAIN);
}

int main(void)
{
    struct test_app app;
    test_app_init(&app, "/bin/cat", (struct vector2i){ .x = 800, .y = 480 });
    struct window *w = test_terminal_window(&app.app->host, &app.app->config.terminal);
    assert(w && terminal_new_tab(w));
    struct session *session = terminal_active_session(w);
    assert(session);
    int fds[2];
    assert(pipe(fds) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&session->input_writer, session->loop, fds[1]));

    key_text(w, YASL_KEY_A, 0, 'a', 'A', "a", YASL_KEY_PRESS, YASL_EVENT_TEXT);
    key(w, YASL_KEY_A, YASL_MOD_SHIFT, 'a', 'A', "A", YASL_KEY_PRESS);
    key(w, YASL_KEY_A, YASL_MOD_CTRL, 'a', 'A', NULL, YASL_KEY_PRESS);
    expect(fds[0], "aA\001");

    /* TR_Q uses Option to type these symbols. Send each symbol once. */
    text(w, "before");
    key(w, YASL_KEY_ISO_BACKSLASH, YASL_MOD_ALT, '<', '>', "|", YASL_KEY_PRESS);
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_PRESS);
    key(w, YASL_KEY_9, YASL_MOD_ALT, '9', ')', "]", YASL_KEY_PRESS);
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_REPEAT);
    expect(fds[0], "before|[][");

    /* A dead key waits for the next key to commit its text. */
    composing_key(w, YASL_KEY_E, NULL);
    composing_key(w, YASL_KEY_LEFT, NULL);
    expect(fds[0], "");
    composing_key(w, YASL_KEY_E, "é");
    expect(fds[0], "é");

    key(w, YASL_KEY_A, YASL_MOD_CAPS, 'a', 'A', "ä😀", YASL_KEY_PRESS);
    expect(fds[0], "ä😀");

    key(w, YASL_KEY_LEFT, YASL_MOD_ALT, 0, 0, NULL, YASL_KEY_PRESS);
    key(w, YASL_KEY_BACKSPACE, YASL_MOD_ALT, 0, 0, NULL, YASL_KEY_PRESS);
    key(w, YASL_KEY_A, YASL_MOD_ALT | YASL_MOD_CTRL, 'a', 'A', NULL, YASL_KEY_PRESS);
    expect(fds[0], "\033[1;3D\033\177\033\001");

    assert(wsap_vtcore_feed(session->term, "\033[>1u", 5, 0));
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_PRESS);
    expect(fds[0], "[");
    assert(wsap_vtcore_feed(session->term, "\033[<u\033[>24u", 10, 0));
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_PRESS);
    expect(fds[0], "\033[56;1;91u");
    key(w, YASL_KEY_A, 0, 'a', 'A', "ä😀", YASL_KEY_PRESS);
    expect(fds[0], "\033[97;1;97:776:128512u");
    text(w, "漢字");
    drain(w);
    expect(fds[0], "\033[0;1;28450:23383u");
    assert(wsap_vtcore_feed(session->term, "\033[<u\033[>31u", 10, 0));
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_REPEAT);
    expect(fds[0], "\033[56;1:2;91u\033[56;1:3u");
    composing_key(w, YASL_KEY_LEFT, "é");
    expect(fds[0], "\033[0;1;233u");

    assert(input_keymap_apply_spec(input_keymap_active(), "alt+8=debug_overlay"));
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_PRESS);
    expect(fds[0], "");
    input_keymap_reset_active(INPUT_KEYBIND_LEGACY);

    struct ui_surface *ui = window_ui_surface(w);
    ui_surface_set_scale(ui, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(ui, (struct vector2i){ .x = 800, .y = 480 });
    struct ui_scene *scene = window_ui_scene_scratch(w);
    assert(ui_surface_build_scene(ui, scene));
    struct ui_debug_view view = {
        .root = ui_surface_root(ui), .surface = ui, .scene = scene,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);

    window_close(w);
    test_app_fini(&app);
    close(fds[0]);
    close(fds[1]);
    return 0;
}
