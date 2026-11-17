#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "input/input.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"

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

int main(void)
{
    drag_survives_second_button_test();
    drag_moves_to_other_held_button_test();
    drag_keeps_starting_button_test();
    pen_contact_test();
    return 0;
}
