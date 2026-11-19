#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "input/command.h"
#include "input/input.h"
#include "input/keybind.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/testing.h"
#include "ui/yasl/window.h"

/*
 * The key route reads the command scope of the focus chain.
 * A binding in a widget scope wins over an app binding.
 * A widget with no scope keeps the app binding.
 */

#define SCOPE_A 2
#define SCOPE_B 3

struct probe {
    struct ui_widget widget;
    /* A probe with `take` false lets the shortcut bubble to its parent. */
    bool take;
    int shortcuts;
    int last_action;
};

static struct window_host host;

static bool probe_event(struct ui_widget *widget, const struct ui_event *event);

static const struct ui_widget_ops PROBE_OPS = {
    .name = "key-route-probe",
    .event = probe_event,
};

/* A probe keeps the shortcut that it takes, so a test sees which widget the
 * route reached. */
static bool probe_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct probe *p = widget->userdata;
    if (event->kind != UI_EVENT_SHORTCUT || !p->take)
        return false;
    p->shortcuts++;
    p->last_action = event->data.shortcut.action;
    return true;
}

static void probe_init(struct probe *p, uint8_t scope, bool take)
{
    *p = (struct probe){ .take = take };
    ui_widget_init(&p->widget, &PROBE_OPS, p);
    ui_widget_set_focusable(&p->widget, true);
    p->widget.command_scope = scope;
}

struct fixture {
    struct ui_widget root;
    /* Two widgets own one scope each, and each scope binds one chord. */
    struct probe a, b;
    /* The leaf has scope zero and sits in the scope of its parent. */
    struct probe leaf;
    /* The plain widget has scope zero and no scoped ancestor. */
    struct probe plain;
    struct input_command_registry reg_a, reg_b;
    input_command_id id_a, id_b;
    struct window *window;
};

static void show(struct fixture *f)
{
    *f = (struct fixture){0};
    struct window_config config = { .size = { .x = 320, .y = 200 } };
    window_host_init(&host, NULL, &config, NULL);
    f->window = window_create(&host, &(struct window_spec){0});
    assert(f->window);
    ui_widget_init(&f->root, NULL, NULL);
    probe_init(&f->a, SCOPE_A, true);
    probe_init(&f->b, SCOPE_B, true);
    probe_init(&f->leaf, INPUT_COMMAND_SCOPE_APP, false);
    probe_init(&f->plain, INPUT_COMMAND_SCOPE_APP, true);
    /* The top half holds scope A, and the bottom half holds scope B. */
    ui_widget_set_fixed_size(&f->a.widget, (struct vector2i){ .x = 320, .y = 100 });
    ui_widget_set_fixed_size(&f->b.widget, (struct vector2i){ .x = 320, .y = 100 });
    ui_widget_set_placement(&f->a.widget, (struct ui_placement){
        .kind = UI_PLACE_AT,
    });
    ui_widget_set_placement(&f->b.widget, (struct ui_placement){
        .kind = UI_PLACE_AT, .anchor.pos = { .y = 100 },
    });
    ui_widget_set_placement(&f->plain.widget, (struct ui_placement){
        .kind = UI_PLACE_AT,
    });
    assert(ui_widget_add_child(&f->root, &f->a.widget));
    assert(ui_widget_add_child(&f->root, &f->b.widget));
    assert(ui_widget_add_child(&f->root, &f->plain.widget));
    assert(ui_widget_add_child(&f->a.widget, &f->leaf.widget));
    assert(window_ui_mount(f->window, &(struct window_ui_spec){ .root = &f->root }));
    /* A window with no native surface gets no relayout, so the test sizes the
     * screen itself. */
    ui_surface_resize(window_ui_key_surface(f->window),
                      (struct vector2i){ .x = 320, .y = 200 });

    input_command_registry_init(&f->reg_a);
    input_command_registry_init(&f->reg_b);
    f->id_a = input_command_add(&f->reg_a, SCOPE_A, "probe_a", "Probe A");
    f->id_b = input_command_add(&f->reg_b, SCOPE_B, "probe_b", "Probe B");
    assert(f->id_a != INPUT_COMMAND_NONE && f->id_b != INPUT_COMMAND_NONE);
    assert(input_keymap_apply_spec_registry(&f->window->keymap, &f->reg_a,
                                            SCOPE_A, "ctrl+j = probe_a"));
    assert(input_keymap_apply_spec_registry(&f->window->keymap, &f->reg_b,
                                            SCOPE_B, "ctrl+k = probe_b"));
    assert(input_keymap_apply_spec(&f->window->keymap, "ctrl+u = scroll_up"));
}

static void teardown(struct fixture *f)
{
    window_destroy(f->window);
    window_host_registry_remove(&host);
    ui_widget_destroy(&f->root);
    input_command_registry_free(&f->reg_a);
    input_command_registry_free(&f->reg_b);
}

static void focus(struct fixture *f, struct probe *p)
{
    ui_surface_focus(window_ui_key_surface(f->window), &p->widget);
    assert(ui_surface_focused(window_ui_key_surface(f->window)) == &p->widget);
}

/* Sends one Ctrl chord to the window, as the router receives it. */
static void press(struct fixture *f, uint32_t codepoint)
{
    struct input_event ev = {
        .kind = INPUT_KEY,
        .key = {
            .key = INPUT_KEY_NONE,
            .codepoint = codepoint,
            .base = codepoint,
            .mods = INPUT_MOD_CTRL,
            .action = INPUT_PRESS,
        },
    };
    window_ui_route(f->window, &ev);
}

/* Takes the shortcut event that the route queued. Returns false when it
 * queued none. */
static bool take_shortcut(struct fixture *f, int *action)
{
    struct input_event ev;
    if (!input_queue_take(&f->window->input, &ev))
        return false;
    assert(ev.kind == INPUT_SHORTCUT);
    *action = ev.shortcut.action;
    return true;
}

/* Sends the queued shortcut to the window, as the drain loop does. Returns
 * true when `p` took it. */
static bool deliver_shortcut(struct fixture *f, int action, struct probe *p)
{
    int before = p->shortcuts;
    struct input_event ev = {
        .kind = INPUT_SHORTCUT,
        .shortcut = { .action = action },
    };
    window_ui_route(f->window, &ev);
    return p->shortcuts == before + 1 && p->last_action == action;
}

/* Each scope binds its own chord. Each chord reaches the widget with that
 * scope, and no chord reaches the other widget. */
static void two_scopes_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;

    focus(&f, &f.a);
    press(&f, 'j');
    assert(take_shortcut(&f, &action) && action == f.id_a);
    assert(deliver_shortcut(&f, action, &f.a));
    assert(f.b.shortcuts == 0);

    press(&f, 'k');
    assert(!take_shortcut(&f, &action));

    focus(&f, &f.b);
    press(&f, 'k');
    assert(take_shortcut(&f, &action) && action == f.id_b);
    assert(deliver_shortcut(&f, action, &f.b));
    assert(f.a.shortcuts == 1);

    press(&f, 'j');
    assert(!take_shortcut(&f, &action));
    teardown(&f);
}

static void zero_scope_app_binding_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;
    focus(&f, &f.plain);
    press(&f, 'u');
    assert(take_shortcut(&f, &action) && action == INPUT_COMMAND_SCROLL_UP);
    assert(deliver_shortcut(&f, action, &f.plain));
    teardown(&f);
}

static void focus_chain_scope_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;
    focus(&f, &f.leaf);
    press(&f, 'j');
    assert(take_shortcut(&f, &action) && action == f.id_a);
    assert(deliver_shortcut(&f, action, &f.a));
    assert(f.leaf.shortcuts == 0);
    teardown(&f);
}

int main(void)
{
    two_scopes_test();
    zero_scope_app_binding_test();
    focus_chain_scope_test();
    return 0;
}
