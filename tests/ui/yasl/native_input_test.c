#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "input/input.h"
#include "ui/yasl/drop.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/ui_input.h"

/* The window is zeroed for each case. It has no surface and no popups, so
 * native input only queues events. */
static struct window w;

static void fresh_window(void)
{
    memset(&w, 0, sizeof w);
}

static struct yasl_event yasl_pointer(unsigned action, unsigned button, unsigned buttons)
{
    struct yasl_event e = { .type = YASL_EVENT_POINTER, .action = action };
    e.pointer.button = button;
    e.pointer.buttons = buttons;
    return e;
}

static struct yasl_event yasl_pen(unsigned action)
{
    struct yasl_event e = { .type = YASL_EVENT_PEN, .action = action };
    e.pen.tool = YASL_PEN_PEN;
    return e;
}

/* Sends one native event. Returns the single mouse event it queued. */
static struct input_mouse_event feed(const struct yasl_event *e)
{
    window_native_input(&w, e);
    struct input_event out;
    bool queued = input_queue_take(&w.input, &out);
    assert(queued);
    assert(out.kind == INPUT_MOUSE);
    return out.mouse;
}

static const unsigned primary = YASL_BUTTON_BIT(YASL_BUTTON_PRIMARY);
static const unsigned secondary = YASL_BUTTON_BIT(YASL_BUTTON_SECONDARY);

/* A left drag keeps the left button held after a right press and release. */
static void drag_survives_second_button_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_PRIMARY, primary);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_SECONDARY, primary | secondary);
    m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_SECONDARY, primary);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, primary);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_PRIMARY, 0);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, 0);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_NONE);
}

/* When the drag button goes up, the other button still held takes the drag. */
static void drag_moves_to_other_held_button_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_PRIMARY, primary);
    feed(&e);
    e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_SECONDARY, primary | secondary);
    feed(&e);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_PRIMARY, secondary);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, secondary);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_SECONDARY, 0);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, 0);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_NONE);
}

/* A drag keeps the button that started it while another button is held. */
static void drag_keeps_starting_button_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_SECONDARY, secondary);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_PRIMARY, primary | secondary);
    m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, primary | secondary);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_SECONDARY);
}

/* Pen contact reports the primary button until the pen lifts. */
static void pen_contact_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pen(YASL_PEN_DOWN);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_PRIMARY);
    assert(m.device == POINTER_DEVICE_PEN);

    e = yasl_pen(YASL_PEN_MOVE);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pen(YASL_PEN_UP);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pen(YASL_PEN_MOVE);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_NONE);
}

/* Sends one key press that carries no native text. Returns the queued key. */
static struct input_key_event feed_key(enum yasl_key key, uint32_t codepoint,
                                       uint32_t shifted, unsigned mods)
{
    struct yasl_event e = { .type = YASL_EVENT_KEY, .action = YASL_KEY_PRESS, .mods = mods };
    e.key.key = key;
    e.key.codepoint = codepoint;
    e.key.shifted = shifted;
    e.key.text_input = YASL_KEY_TEXT_UNSPECIFIED;
    window_native_input(&w, &e);
    struct input_event out;
    bool queued = input_queue_take(&w.input, &out);
    assert(queued);
    assert(out.kind == INPUT_KEY);
    return out.key;
}

/* A key that the global map binds still reaches the focused widget. Step 1
 * queues the key only, so the widget sees the key before the app command. */
static void bound_key_reaches_widget_test(void)
{
    fresh_window();
    struct yasl_event e = { .type = YASL_EVENT_KEY, .action = YASL_KEY_PRESS,
                            .mods = YASL_MOD_CTRL | YASL_MOD_SHIFT };
    e.key.key = YASL_KEY_T;
    e.key.codepoint = 't';
    e.key.text_input = YASL_KEY_TEXT_UNSPECIFIED;
    window_native_input(&w, &e);

    struct input_event out;
    assert(input_queue_take(&w.input, &out));
    assert(out.kind == INPUT_KEY);
    assert(out.key.key == INPUT_KEY_NONE && out.key.codepoint == 't');
    assert(out.key.mods == (INPUT_MOD_CTRL | INPUT_MOD_SHIFT));
    assert(!input_queue_take(&w.input, &out));
}

/* ALT changes the text of a key, but the text still arrives as a TEXT event.
 * The key event defers its own label so the widget does not insert it twice. */
static void alt_key_text_deferred_test(void)
{
    fresh_window();
    struct input_key_event k = feed_key(YASL_KEY_A, 'a', 0xE5, YASL_MOD_ALT);
    assert(k.text_deferred);
    assert(ui_key_from_input(&k).text_deferred);
}

/* Keypad Clear keeps its identity. */
static void keypad_clear_test(void)
{
    fresh_window();
    struct input_key_event k = feed_key(YASL_KEY_KP_CLEAR, 0, 0, 0);
    assert(k.key == INPUT_KEY_KP_BEGIN);
    assert(k.keypad == INPUT_KEY_KP_BEGIN);
}


/* Caps Lock changes letter keys only. Shift cancels it on letters. */
static void caps_lock_key_text_test(void)
{
    fresh_window();
    struct input_key_event k = feed_key(YASL_KEY_1, '1', '!', YASL_MOD_CAPS);
    assert(k.text == '1');
    assert(ui_key_from_input(&k).text == '1');

    k = feed_key(YASL_KEY_A, 'a', 'A', YASL_MOD_CAPS);
    assert(k.text == 'A');
    assert(ui_key_from_input(&k).text == 'A');

    k = feed_key(YASL_KEY_A, 'a', 'A', YASL_MOD_CAPS | YASL_MOD_SHIFT);
    assert(k.text == 'a');
    assert(ui_key_from_input(&k).text == 'a');

    /* s-cedilla on a Turkish layout sits on the US ';' key. */
    k = feed_key(YASL_KEY_SEMICOLON, 0x15F, 0x15E, YASL_MOD_CAPS);
    assert(k.text == 0x15E);
    assert(ui_key_from_input(&k).text == 0x15E);

    k = feed_key(YASL_KEY_SEMICOLON, 0x15F, 0x15E, YASL_MOD_CAPS | YASL_MOD_SHIFT);
    assert(k.text == 0x15F);
    assert(ui_key_from_input(&k).text == 0x15F);

    /* e-acute on an AZERTY layout sits on the '2' key. Its shifted label is ASCII. */
    k = feed_key(YASL_KEY_2, 0xE9, '2', YASL_MOD_CAPS);
    assert(k.text == 0xE9);
    assert(ui_key_from_input(&k).text == 0xE9);
}

/* Builds a DROP RELEASE event with `length` bytes of `text` and type `mime`. */
static struct yasl_event drop_release(const char *mime, const char *text, size_t length)
{
    struct yasl_event e = {
        .type = YASL_EVENT_DROP,
        .action = YASL_DROP_RELEASE,
        .text = text,
        .length = length,
    };
    snprintf(e.drop.mime, sizeof e.drop.mime, "%s", mime);
    return e;
}

/* Takes the next queued event. It must be a paste of `text`. */
static void expect_paste(const char *text)
{
    struct input_event out;
    assert(input_queue_take(&w.input, &out));
    assert(out.kind == INPUT_PASTE);
    assert(out.paste.len == strlen(text));
    assert(memcmp(out.paste.data, text, out.paste.len) == 0);
    input_event_release(&out);
}

/* An image drop writes no file and queues nothing. The payload stays in
 * memory for a MIME drop event. */
static void image_drop_writes_no_file_test(void)
{
    static const char image_bytes[] = { 'P', 'N', 'G', 0, 1, 0, 2, 'x' };
    char runtime[PATH_MAX];
    snprintf(runtime, PATH_MAX, "/tmp/wsap-drop-XXXXXX");
    assert(mkdtemp(runtime));
    assert(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);

    fresh_window();
    struct yasl_event e = drop_release("image/png", image_bytes, sizeof image_bytes);
    window_native_input(&w, &e);
    struct input_event out;
    assert(!input_queue_take(&w.input, &out));
    assert(drop_save_image("image/png", image_bytes, sizeof image_bytes) == NULL);
    drop_cleanup();

    char drops[PATH_MAX + 32];
    snprintf(drops, sizeof drops, "%s/wsap_term", runtime);
    assert(access(drops, F_OK) != 0);
    assert(rmdir(runtime) == 0);
}

/* Text drops paste like clipboard text, for each text type. */
static void text_drop_pastes_test(void)
{
    static const char text[] = "caf\xc3\xa9 \xe2\x9c\x93\n";
    static const char *const types[] = {
        "text/plain;charset=utf-8", "UTF8_STRING", "text/plain",
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        fresh_window();
        struct yasl_event e = drop_release(types[i], text, sizeof text - 1);
        window_native_input(&w, &e);
        expect_paste(text);
    }
}

/* A uri list with no local file pastes its URLs. Comment lines and blank
 * lines are skipped, and the URLs are joined with a space. */
static void web_uri_list_pastes_test(void)
{
    fresh_window();
    static const char link[] = "https://example.com/a.png\r\n";
    struct yasl_event e = drop_release("text/uri-list", link, sizeof link - 1);
    window_native_input(&w, &e);
    expect_paste("https://example.com/a.png");

    fresh_window();
    static const char list[] = "# copied from a browser\r\n"
                               "https://example.com/a.png\r\n"
                               "\r\n"
                               "https://example.com/b.png\r\n";
    e = drop_release("text/uri-list", list, sizeof list - 1);
    window_native_input(&w, &e);
    expect_paste("https://example.com/a.png https://example.com/b.png");
}

/* A uri list with a local file queues the list as a file drop, as before. */
static void local_uri_list_drops_files_test(void)
{
    fresh_window();
    static const char list[] = "file:///tmp/x\r\n";
    struct yasl_event e = drop_release("text/uri-list", list, sizeof list - 1);
    window_native_input(&w, &e);
    struct input_event out;
    assert(input_queue_take(&w.input, &out));
    assert(out.kind == INPUT_DROP_FILES);
    assert(out.drop.len == sizeof list - 1);
    assert(memcmp(out.drop.data, list, out.drop.len) == 0);
    input_event_release(&out);
}

/* A drop of another data type queues nothing. An empty image queues nothing. */
static void other_drops_queue_nothing_test(void)
{
    static const char json[] = "{}";

    fresh_window();
    struct yasl_event e = drop_release("application/json", json, sizeof json - 1);
    window_native_input(&w, &e);
    e = drop_release("image/png", json, 0);
    window_native_input(&w, &e);
    struct input_event out;
    assert(!input_queue_take(&w.input, &out));
}

int main(void)
{
    drag_survives_second_button_test();
    drag_moves_to_other_held_button_test();
    drag_keeps_starting_button_test();
    pen_contact_test();
    caps_lock_key_text_test();
    bound_key_reaches_widget_test();
    alt_key_text_deferred_test();
    keypad_clear_test();
    image_drop_writes_no_file_test();
    text_drop_pastes_test();
    web_uri_list_pastes_test();
    local_uri_list_drops_files_test();
    other_drops_queue_nothing_test();
    return 0;
}
