#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/surface.h"
#include "ui/ui.h"
#include "ui/widget_impl.h"

/* A widget that counts what it receives. */
struct probe {
    int keys, texts, shortcuts, drops, captures;
    bool handle_key;
    bool keep_text;
    /* Invalidate the widget when a key arrives. */
    bool invalidate_key;
    bool handle_capture;
    /* Answer for text, shortcut, drop, and pointer events. */
    bool handle_other;
    int shortcut_action;
    enum ui_edit_command shortcut_edit;
    struct ui_widget widget;
};

static bool probe_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct probe *p = widget->userdata;
    switch (event->kind) {
    case UI_EVENT_KEY:
        p->keys++;
        if (p->keep_text)
            ui_surface_keep_key_text(ui_widget_surface(widget));
        if (p->invalidate_key)
            ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
        return p->handle_key;
    case UI_EVENT_TEXT:
        p->texts++;
        return p->handle_other;
    case UI_EVENT_SHORTCUT:
        p->shortcuts++;
        p->shortcut_action = event->data.shortcut.action;
        p->shortcut_edit = event->data.shortcut.edit;
        return p->handle_other;
    case UI_EVENT_DROP_FILES:
        p->drops++;
        return p->handle_other;
    case UI_EVENT_POINTER:
        return p->handle_other;
    default:
        return false;
    }
}

static bool probe_capture(struct ui_widget *widget,
                          const struct ui_event *event, void *userdata)
{
    struct probe *p = userdata;
    (void)widget;
    if (event->kind != UI_EVENT_KEY)
        return false;
    p->captures++;
    return p->handle_capture;
}

static const struct ui_widget_ops PROBE_OPS = {
    .name = "probe",
    .event = probe_event,
};

static void probe_init(struct probe *p)
{
    *p = (struct probe){0};
    ui_widget_init(&p->widget, &PROBE_OPS, p);
}

/* Makes root the surface root, sized 200 by 100. */
static void set_root_200x100(struct ui_surface *s, struct ui_widget *root)
{
    ui_surface_set_root(s, root);
    ui_surface_resize(s, (struct vector2i){ .x = 200, .y = 100 });
}

static void key_press(struct ui_surface *s)
{
    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = 'k',
    }, 0);
    ui_surface_dispatch(s, &key);
}

/* With nothing focused, keys go to the default focus. */
static void default_focus_test(void)
{
    struct probe root, child;
    probe_init(&root);
    probe_init(&child);
    root.handle_key = true;
    child.handle_key = true;
    assert(ui_widget_add_child(&root.widget, &child.widget));

    struct ui_surface s = {0};
    set_root_200x100(&s, &root.widget);
    ui_surface_prepare(&s);
    assert(!ui_surface_focused(&s));

    ui_surface_set_default_focus(&s, &child.widget);
    key_press(&s);
    assert(child.keys == 1 && root.keys == 0);

    /* With no default focus, the root takes the key. */
    ui_surface_set_default_focus(&s, NULL);
    key_press(&s);
    assert(child.keys == 1 && root.keys == 1);

    /* A hidden default focus falls back to the root. */
    ui_surface_set_default_focus(&s, &child.widget);
    ui_widget_set_hidden(&child.widget, true);
    key_press(&s);
    assert(child.keys == 1 && root.keys == 2);

    ui_surface_destroy(&s);
    ui_widget_destroy(&root.widget);
}

/* A focused widget still takes keys ahead of the default focus. */
static void focus_beats_default_test(void)
{
    struct probe root, first, second;
    probe_init(&root);
    probe_init(&first);
    probe_init(&second);
    first.handle_key = true;
    second.handle_key = true;
    assert(ui_widget_add_child(&root.widget, &first.widget));
    assert(ui_widget_add_child(&root.widget, &second.widget));

    struct ui_surface s = {0};
    set_root_200x100(&s, &root.widget);
    ui_surface_set_default_focus(&s, &first.widget);
    ui_surface_focus(&s, &second.widget);
    key_press(&s);
    assert(second.keys == 1 && first.keys == 0);

    ui_surface_destroy(&s);
    ui_widget_destroy(&root.widget);
}

/* A capture on an ancestor runs before the focused label handles the key. */
static void capture_before_label_test(void)
{
    struct probe root;
    probe_init(&root);
    struct ui_label label;
    ui_label_init(&label);
    ui_label_set_text(&label, "Hello");
    ui_label_set_selectable(&label, true);
    assert(ui_widget_add_child(&root.widget, ui_label_widget(&label)));

    struct ui_surface s = {0};
    set_root_200x100(&s, &root.widget);
    ui_surface_prepare(&s);
    ui_surface_focus(&s, ui_label_widget(&label));

    struct ui_event select_all = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = 'a', .mods = UI_MOD_CTRL,
    }, 0);
    ui_widget_set_key_capture(&root.widget, probe_capture, &root);
    root.handle_capture = true;
    assert(ui_surface_dispatch(&s, &select_all));
    size_t lo = 0, hi = 0;
    assert(root.captures == 1);
    assert(!ui_label_selection(&label, &lo, &hi));

    /* A capture that declines lets the label see the key. */
    root.handle_capture = false;
    assert(ui_surface_dispatch(&s, &select_all));
    assert(root.captures == 2);
    assert(ui_label_selection(&label, &lo, &hi) && lo == 0 && hi == 5);

    ui_surface_destroy(&s);
    ui_widget_destroy(&root.widget);
}

/* With a modal open, the capture path starts at the modal. The root's
 * capture does not run, even though the root holds the modal. */
static void capture_starts_at_modal_test(void)
{
    struct probe root, modal, child;
    probe_init(&root);
    probe_init(&modal);
    probe_init(&child);
    child.handle_key = true;
    root.handle_capture = true;
    assert(ui_widget_add_child(&root.widget, &modal.widget));
    assert(ui_widget_add_child(&modal.widget, &child.widget));
    ui_widget_set_focusable(&child.widget, true);
    ui_widget_set_key_capture(&root.widget, probe_capture, &root);
    ui_widget_set_modal(&modal.widget, true);

    struct ui_surface s = {0};
    set_root_200x100(&s, &root.widget);
    ui_surface_prepare(&s);
    assert(ui_surface_has_modal(&s));

    key_press(&s);
    assert(root.captures == 0);
    assert(child.keys == 1);

    ui_surface_destroy(&s);
    ui_widget_destroy(&root.widget);
}

/* A shortcut or a file drop bubbles from the focused widget to an ancestor. */
static void shortcut_bubbles_test(void)
{
    struct probe root, leaf;
    probe_init(&root);
    probe_init(&leaf);
    assert(ui_widget_add_child(&root.widget, &leaf.widget));
    root.handle_other = true;

    struct ui_surface s = {0};
    set_root_200x100(&s, &root.widget);
    ui_surface_focus(&s, &leaf.widget);

    struct ui_event copy = ui_event_shortcut(42, UI_EDIT_COPY, 0);
    assert(ui_surface_dispatch(&s, &copy));
    assert(leaf.shortcuts == 1 && root.shortcuts == 1);
    assert(root.shortcut_action == 42 && root.shortcut_edit == UI_EDIT_COPY);

    const char uris[] = "file:///tmp/a.txt";
    struct ui_event drop = ui_event_drop_files(uris, sizeof(uris) - 1, 0);
    assert(ui_surface_dispatch(&s, &drop));
    assert(leaf.drops == 1 && root.drops == 1);

    ui_surface_destroy(&s);
    ui_widget_destroy(&root.widget);
}

/* A key handler that keeps its text lets the text reach the UI. The flag
 * clears at the next dispatch. */
static void keep_key_text_test(void)
{
    struct probe root;
    probe_init(&root);
    root.handle_key = true;
    root.keep_text = true;

    struct ui_surface s = {0};
    set_root_200x100(&s, &root.widget);
    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = 'k', .text_deferred = true,
    }, 0);
    assert(ui_surface_dispatch(&s, &key));
    assert(ui_surface_key_text_kept(&s));

    root.keep_text = false;
    assert(ui_surface_dispatch(&s, &key));
    assert(!ui_surface_key_text_kept(&s));

    ui_surface_destroy(&s);
    ui_widget_destroy(&root.widget);
}

/* A handled key draws only when a widget invalidates. A handled pointer
 * event still requests the frame. */
static void frame_requests_test(void)
{
    struct probe root;
    probe_init(&root);
    root.handle_key = true;
    root.handle_other = true;

    struct ui_surface s = {0};
    set_root_200x100(&s, &root.widget);
    ui_surface_prepare(&s);

    ui_surface_take_frame_request(&s);
    key_press(&s);
    assert(root.keys == 1 && !ui_surface_frame_pending(&s));

    root.invalidate_key = true;
    key_press(&s);
    assert(ui_surface_frame_pending(&s));
    ui_surface_take_frame_request(&s);
    root.invalidate_key = false;

    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 10, .y = 10 },
    }, 0);
    assert(ui_surface_dispatch(&s, &press));
    assert(ui_surface_frame_pending(&s));

    ui_surface_destroy(&s);
    ui_widget_destroy(&root.widget);
}

int main(void)
{
    default_focus_test();
    focus_beats_default_test();
    capture_before_label_test();
    capture_starts_at_modal_test();
    shortcut_bubbles_test();
    keep_key_text_test();
    frame_requests_test();
    puts("PASS: default focus, shortcut events, and key capture");
    return 0;
}
