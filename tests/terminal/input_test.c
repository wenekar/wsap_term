#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "app/tabs/tabs.h"
#include "helpers/terminal_window.h"
#include "terminal/internal.h"
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

/* Sends one key in the shape that the Wayland and X11 backends send it.
 * A COMMIT key is followed by its TEXT event. A NONE key has no text event,
 * because native text input inserted nothing. */
static void native_key(struct window *w, enum yasl_key code, unsigned mods,
                       uint32_t label, uint32_t shifted,
                       enum yasl_key_text text_input, const char *value)
{
    static uint64_t text_id;
    struct yasl_event event = {
        .type = YASL_EVENT_KEY,
        .action = YASL_KEY_PRESS,
        .mods = mods,
        .key = {
            .key = code, .codepoint = label, .shifted = shifted,
            .text_input = text_input,
            .text_id = text_input == YASL_KEY_TEXT_COMMIT ? ++text_id : 0,
        },
    };
    window_native_input(w, &event);
    if (event.key.text_id)
        commit(w, value, event.key.text_id, YASL_EVENT_TEXT);
    event.action = YASL_KEY_RELEASE;
    event.key.text_id = 0;
    window_native_input(w, &event);
    drain(w);
}

/* Sends one key event with no text and drains it. A repeat sends no
 * release, as a held key does. */
static void raw_key(struct window *w, enum yasl_key code, unsigned mods,
                    uint32_t label, uint32_t shifted, unsigned action)
{
    window_native_input(w, &(struct yasl_event){
        .type = YASL_EVENT_KEY,
        .action = action,
        .mods = mods,
        .key = {
            .key = code, .codepoint = label, .shifted = shifted,
            .text_input = YASL_KEY_TEXT_NONE,
        },
    });
    drain(w);
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
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    struct session *session = terminal_session(content);
    assert(session);
    int fds[2];
    assert(pipe(fds) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&session->input_writer, session->loop, fds[1]));

    /* An unconsumed Alt press carries no native text, and no TEXT event
     * follows it. The Wayland and X11 backends both report it this way. The
     * key label must reach the encoder, which sends ESC and the letter. */
    native_key(w, YASL_KEY_A, YASL_MOD_ALT, 'a', 'A', YASL_KEY_TEXT_NONE, NULL);
    expect(fds[0], "\033a");

    /* A plain letter commits its text. The backends send the key as COMMIT
     * and then one TEXT event. The shell gets the letter once. */
    native_key(w, YASL_KEY_A, 0, 'a', 'A', YASL_KEY_TEXT_COMMIT, "a");
    expect(fds[0], "a");

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

    /* Each window owns its keymap. A chord that one window binds runs only
     * in that window. The same chord reaches the shell of the other
     * window. */
    struct window *other = test_terminal_window(app.app);
    assert(other && app_tabs_new(other));
    struct terminal *other_content = app_tabs_active(other);
    struct session *other_session = terminal_session(other_content);
    assert(other_session);
    int other_fds[2];
    assert(pipe(other_fds) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(other_fds[i], F_SETFL,
                     fcntl(other_fds[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&other_session->input_writer,
                                    other_session->loop, other_fds[1]));

    assert(input_keymap_apply_spec(&w->keymap, "alt+8=debug_overlay"));
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_PRESS);
    expect(fds[0], "");
    key(other, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_PRESS);
    expect(other_fds[0], "[");
    input_keymap_reset(&w->keymap, INPUT_KEYBIND_LEGACY);

    /* The legacy style binds no bare letter, so the shell gets the letter. */
    assert(wsap_vtcore_feed(session->term, "\033[<u", 5, 0));
    native_key(w, YASL_KEY_C, 0, 'c', 'C', YASL_KEY_TEXT_COMMIT, "c");
    expect(fds[0], "c");

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

    /* Test double-width and double-height rows in the app. */
    assert(wsap_vtcore_feed(session->term, "\033[1;1H\033#6DoubleWidth\r\n\033#3DoubleTop\r\n\033#4DoubleBottom\r\n", 53, 0));
    assert(wsap_vtcore_snapshot_update(content->snapshot, session->term));
    struct rect2i vrect = terminal_view_rect(content);
    const struct font_metrics *met = terminal_font_metrics(content);
    /* Cell on row 0 (double width): x position at 2 * cw + 2 must map to column 1 */
    struct vector2i pt = { .x = vrect.pos.x + 2 * met->cell_w + 2, .y = vrect.pos.y + 4 };
    struct vector2i c = terminal_cell_at(content, pt);
    assert(c.x == 1 && c.y == 0);
    /* Cell on row 1 (double height top half): x position at 4 * cw + 2 must map to column 2 */
    pt.y = vrect.pos.y + met->cell_h + 4;
    pt.x = vrect.pos.x + 4 * met->cell_w + 2;
    c = terminal_cell_at(content, pt);
    assert(c.x == 2 && c.y == 1);

    /* A click past the right edge of a wide row stays on its last column. */
    int wide_cols = wsap_vtcore_size(session->term).x / 2;
    pt.y = vrect.pos.y + 4;
    pt.x = vrect.pos.x + (wide_cols + 5) * 2 * met->cell_w;
    c = terminal_cell_at(content, pt);
    assert(c.x == wide_cols - 1 && c.y == 0);

    /* The two halves of a wide cell start a different selection. The exact
     * span comes from the terminal core's half-cell rules. */
    {
        struct rect2i v = terminal_view_rect(content);
        int half = met->cell_w / 2;
        char *picked[2] = { NULL, NULL };
        for (int pass = 0; pass < 2; pass++) {
            int x = v.pos.x + (pass ? half + 1 : 1);
            struct ui_event down = ui_event_pointer((struct ui_pointer_event){
                .kind = UI_POINTER_PRESS, .pos = { .x = x, .y = v.pos.y + 4 },
            }, 0);
            assert(ui_surface_dispatch(ui, &down));
            struct ui_event drag = ui_event_pointer((struct ui_pointer_event){
                .kind = UI_POINTER_MOTION,
                .pos = { .x = v.pos.x + 4 * met->cell_w + 1, .y = v.pos.y + 4 },
            }, 10);
            assert(ui_surface_dispatch(ui, &drag));
            assert(wsap_vtcore_has_selection(session->term));
            picked[pass] = wsap_vtcore_selection_text(session->term);
            assert(picked[pass]);
            struct ui_event up = ui_event_pointer((struct ui_pointer_event){
                .kind = UI_POINTER_RELEASE, .pos = drag.data.pointer.pos,
            }, 20);
            ui_surface_dispatch(ui, &up);
        }
        assert(strcmp(picked[0], picked[1]) != 0);
        wsap_vtcore_text_free(picked[0]);
        wsap_vtcore_text_free(picked[1]);
    }

    /* UI inspector lint check on scene with double-size rows */
    assert(ui_surface_build_scene(ui, scene));
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);

    /* A chord that the shell owns sends the key that the user pressed.
     * alt+u = undo sends ESC u, not ^Z. */
    assert(input_keymap_apply_spec(&w->keymap, "alt+u = undo"));
    native_key(w, YASL_KEY_U, YASL_MOD_ALT, 'u', 'U', YASL_KEY_TEXT_NONE, NULL);
    expect(fds[0], "\033u");

    /* TR_Q types `[` with alt+8. The terminal takes a key that the shell
     * owns, so the shell gets the text of the key also when the key is
     * bound to undo. */
    assert(input_keymap_apply_spec(&w->keymap, "alt+8 = undo"));
    key(w, YASL_KEY_8, YASL_MOD_ALT, '8', '(', "[", YASL_KEY_PRESS);
    expect(fds[0], "[");
    assert(input_keymap_apply_spec(&w->keymap, "alt+8 = none"));

    /* f5 = cut sends the f5 sequence, not ^X. */
    assert(input_keymap_apply_spec(&w->keymap, "f5 = cut"));
    key(w, YASL_KEY_F5, 0, 0, 0, NULL, YASL_KEY_PRESS);
    expect(fds[0], "\033[15~");

    /* Kitty keyboard mode encodes ctrl+z = undo as the kitty key. */
    assert(input_keymap_apply_spec(&w->keymap, "ctrl+z = undo"));
    assert(wsap_vtcore_feed(session->term, "\033[>1u", 5, 0));
    key(w, YASL_KEY_Z, YASL_MOD_CTRL, 'z', 'Z', NULL, YASL_KEY_PRESS);
    expect(fds[0], "\033[122;5u");
    assert(wsap_vtcore_feed(session->term, "\033[<u", 4, 0));

    /* A held undo key sends the key for the press and for each repeat. */
    raw_key(w, YASL_KEY_Z, YASL_MOD_CTRL, 'z', 'Z', YASL_KEY_PRESS);
    raw_key(w, YASL_KEY_Z, YASL_MOD_CTRL, 'z', 'Z', YASL_KEY_REPEAT);
    raw_key(w, YASL_KEY_Z, YASL_MOD_CTRL, 'z', 'Z', YASL_KEY_REPEAT);
    expect(fds[0], "\x1a\x1a\x1a");

    /* A shortcut with no key sends nothing. The debug socket `shortcut
     * undo` takes this path. */
    struct input_event keyless = {
        .kind = INPUT_SHORTCUT,
        .shortcut = { .action = INPUT_COMMAND_UNDO },
    };
    window_ui_test_route(w, &keyless);
    expect(fds[0], "");

    /* f2 = copy with no selection sends the key, not SIGINT. */
    assert(!wsap_vtcore_has_selection(session->term));
    assert(input_keymap_apply_spec(&w->keymap, "f2 = copy"));
    key(w, YASL_KEY_F2, 0, 0, 0, NULL, YASL_KEY_PRESS);
    expect(fds[0], "\033OQ");

    /* A held copy chord with a selection copies once. The press clears the
     * selection, but its repeats and its release still belong to the copy,
     * so the shell gets no ^C. */
    assert(input_keymap_apply_spec(&w->keymap, "ctrl+c = copy"));
    assert(wsap_vtcore_feed(session->term, "copy me", 7, 0));
    terminal_select_all(content);
    assert(wsap_vtcore_has_selection(session->term));
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_PRESS);
    assert(!wsap_vtcore_has_selection(session->term));
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_REPEAT);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_REPEAT);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_RELEASE);
    expect(fds[0], "");

    /* The next press, with no selection, belongs to the shell again. */
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_PRESS);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_RELEASE);
    expect(fds[0], "\x03");

    /* The kitty modes that report repeats and releases send no part of a
     * copy that the app ran. */
    assert(wsap_vtcore_feed(session->term, "\033[>3u", 5, 0));
    terminal_select_all(content);
    assert(wsap_vtcore_has_selection(session->term));
    raw_key(w, YASL_KEY_F2, 0, 0, 0, YASL_KEY_PRESS);
    raw_key(w, YASL_KEY_F2, 0, 0, 0, YASL_KEY_REPEAT);
    raw_key(w, YASL_KEY_F2, 0, 0, 0, YASL_KEY_RELEASE);
    expect(fds[0], "");
    assert(wsap_vtcore_feed(session->term, "\033[<u", 4, 0));

    /* A command scope on an ancestor of the terminal view applies to the
     * view. Its chord runs the scoped command and does not reach the
     * shell. */
    {
        struct ui_surface *keys = window_ui_key_surface(w);
        struct ui_widget *target = ui_surface_key_target(keys);
        assert(target && target->parent);
        uint8_t scope = input_command_scope_new();
        assert(scope != INPUT_COMMAND_SCOPE_APP);
        struct input_command_registry reg;
        input_command_registry_init(&reg);
        input_command_id id = input_command_add(&reg, scope, "probe_k", "Probe K");
        assert(id != INPUT_COMMAND_NONE);
        assert(input_keymap_apply_spec_registry(&w->keymap, &reg, scope,
                                                "ctrl+k = probe_k"));
        raw_key(w, YASL_KEY_K, YASL_MOD_CTRL, 'k', 'K', YASL_KEY_PRESS);
        raw_key(w, YASL_KEY_K, YASL_MOD_CTRL, 'k', 'K', YASL_KEY_RELEASE);
        expect(fds[0], "\x0b");
        target->parent->command_scope = scope;
        assert(ui_surface_command_scope(keys) == scope);
        raw_key(w, YASL_KEY_K, YASL_MOD_CTRL, 'k', 'K', YASL_KEY_PRESS);
        raw_key(w, YASL_KEY_K, YASL_MOD_CTRL, 'k', 'K', YASL_KEY_RELEASE);
        expect(fds[0], "");
        target->parent->command_scope = 0;
        assert(input_keymap_apply_spec_registry(&w->keymap, &reg, scope,
                                                "ctrl+k = none"));
        input_command_registry_free(&reg);
        input_command_scope_release(scope);
    }

    /* A repeat of the copy chord with no press in this view still belongs
     * to the copy while a selection exists. The shell gets no ^C, and the
     * selection stays. */
    terminal_select_all(content);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_REPEAT);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_REPEAT);
    expect(fds[0], "");
    assert(wsap_vtcore_has_selection(session->term));
    wsap_vtcore_select_clear(session->term);

    /* In the kitty mode that reports releases, a second app chord inside a
     * held copy does not drop the record of the copy. The shell gets no
     * release of either chord. */
    assert(input_keymap_apply_spec(&w->keymap, "f3 = font_larger"));
    assert(wsap_vtcore_feed(session->term, "\033[>2u", 5, 0));
    terminal_select_all(content);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_PRESS);
    raw_key(w, YASL_KEY_F3, 0, 0, 0, YASL_KEY_PRESS);
    raw_key(w, YASL_KEY_F3, 0, 0, 0, YASL_KEY_RELEASE);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_REPEAT);
    raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_RELEASE);
    expect(fds[0], "");
    assert(wsap_vtcore_feed(session->term, "\033[<u", 4, 0));
    assert(input_keymap_apply_spec(&w->keymap, "f3 = none"));

    /* A held tab chord switches the tab on its press. Its repeats and its
     * release reach the view of the new tab, which did not see the press.
     * They still belong to the chord, so no shell gets the key. Tab 0 is
     * the first tab, and tabs 1 and 2 report releases. */
    {
        struct terminal *tab_content[2];
        int tab_fds[2][2];
        for (int i = 0; i < 2; i++) {
            assert(app_tabs_new(w));
            tab_content[i] = app_tabs_active(w);
            struct session *s = terminal_session(tab_content[i]);
            assert(s);
            assert(pipe(tab_fds[i]) == 0);
            for (int j = 0; j < 2; j++)
                assert(fcntl(tab_fds[i][j], F_SETFL,
                             fcntl(tab_fds[i][j], F_GETFL) | O_NONBLOCK) == 0);
            assert(session_write_queue_bind(&s->input_writer, s->loop,
                                            tab_fds[i][1]));
            assert(wsap_vtcore_feed(s->term, "\033[>2u", 5, 0));
        }
        assert(input_keymap_apply_spec(&w->keymap, "ctrl+tab = next_tab"));
        assert(input_keymap_apply_spec(&w->keymap, "ctrl+shift+w = close_tab"));

        app_tabs_select(w, 0);
        drain(w);
        raw_key(w, YASL_KEY_TAB, YASL_MOD_CTRL, '\t', '\t', YASL_KEY_PRESS);
        assert(app_tabs_active(w) == tab_content[0]);
        raw_key(w, YASL_KEY_TAB, YASL_MOD_CTRL, '\t', '\t', YASL_KEY_REPEAT);
        raw_key(w, YASL_KEY_TAB, YASL_MOD_CTRL, '\t', '\t', YASL_KEY_REPEAT);
        raw_key(w, YASL_KEY_TAB, YASL_MOD_CTRL, '\t', '\t', YASL_KEY_RELEASE);
        expect(fds[0], "");
        expect(tab_fds[0][0], "");
        expect(tab_fds[1][0], "");

        /* A held close chord closes one tab. The neighbor tab gets no key. */
        app_tabs_select(w, 1);
        drain(w);
        assert(app_tabs_active(w) == tab_content[0]);
        unsigned mods = YASL_MOD_CTRL | YASL_MOD_SHIFT;
        raw_key(w, YASL_KEY_W, mods, 'w', 'W', YASL_KEY_PRESS);
        assert(app_tabs_active(w) != tab_content[0]);
        raw_key(w, YASL_KEY_W, mods, 'w', 'W', YASL_KEY_REPEAT);
        raw_key(w, YASL_KEY_W, mods, 'w', 'W', YASL_KEY_REPEAT);
        raw_key(w, YASL_KEY_W, mods, 'w', 'W', YASL_KEY_RELEASE);
        expect(fds[0], "");
        expect(tab_fds[1][0], "");

        /* A copy press in tab 0 and a tab switch while the copy is held.
         * The copy release reaches the view of the new tab. The terminals
         * of one window share the record, so the release stays with the
         * copy, and no shell gets it. */
        app_tabs_select(w, 0);
        drain(w);
        assert(app_tabs_active(w) == content);
        terminal_select_all(content);
        raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_PRESS);
        raw_key(w, YASL_KEY_TAB, YASL_MOD_CTRL, '\t', '\t', YASL_KEY_PRESS);
        raw_key(w, YASL_KEY_TAB, YASL_MOD_CTRL, '\t', '\t', YASL_KEY_RELEASE);
        assert(app_tabs_active(w) == tab_content[1]);
        raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_RELEASE);
        expect(fds[0], "");
        expect(tab_fds[1][0], "");

        /* A press decides again. The copy release went missing, so the
         * record still holds the key. A new press with no selection still
         * belongs to the shell. */
        app_tabs_select(w, 0);
        drain(w);
        terminal_select_all(content);
        raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_PRESS);
        expect(fds[0], "");
        raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_PRESS);
        raw_key(w, YASL_KEY_C, YASL_MOD_CTRL, 'c', 'C', YASL_KEY_RELEASE);
        expect(fds[0], "\x03");
        for (int i = 0; i < 2; i++) {
            close(tab_fds[i][0]);
            close(tab_fds[i][1]);
        }
    }

    window_close(other);
    window_close(w);
    test_app_fini(&app);
    close(other_fds[0]);
    close(other_fds[1]);
    close(fds[0]);
    close(fds[1]);
    return 0;
}
