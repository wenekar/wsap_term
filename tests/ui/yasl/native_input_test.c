#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "input/input.h"
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

int main(void)
{
    drag_survives_second_button_test();
    drag_moves_to_other_held_button_test();
    drag_keeps_starting_button_test();
    pen_contact_test();
    caps_lock_key_text_test();
    return 0;
}
