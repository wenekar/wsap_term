#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "input/command.h"
#include "input/input.h"
#include "input/keybind.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/testing.h"
#include "ui/yasl/window.h"

/*
 * The key route reads the command scope of the focus chain.
 * A binding in a widget scope wins over an app binding.
 * A widget with no scope keeps the app binding.
 */

/* The two scopes of the fixture. Each fixture takes them and releases them. */
static uint8_t SCOPE_A, SCOPE_B;

struct probe {
    struct ui_widget widget;
    /* A probe with `take` false lets the shortcut bubble to its parent. */
    bool take;
    /* A probe with `take_keys` true handles each key and keeps its text. */
    bool take_keys;
    int shortcuts;
    int last_action;
    int texts;
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
    if (event->kind == UI_EVENT_TEXT) {
        p->texts++;
        return true;
    }
    if (event->kind == UI_EVENT_KEY && p->take_keys) {
        ui_surface_keep_key_text(ui_widget_surface(widget));
        return true;
    }
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
    SCOPE_A = input_command_scope_new();
    SCOPE_B = input_command_scope_new();
    assert(SCOPE_A && SCOPE_B);
    /* The host borrows the config, so it must outlive the host. */
    static const struct window_config config = { .size = { .x = 320, .y = 200 } };
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
    free(host.windows);
    input_keymap_free(&host.keymap);
    ui_widget_destroy(&f->root);
    input_command_registry_free(&f->reg_a);
    input_command_registry_free(&f->reg_b);
    input_command_scope_release(SCOPE_A);
    input_command_scope_release(SCOPE_B);
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

/* A widget with a scope out of range counts as a widget with no scope, so
 * the app chords still work in its subtree. */
static void bad_scope_keeps_app_bindings_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;
    f.plain.widget.command_scope = 200;
    focus(&f, &f.plain);
    press(&f, 'u');
    assert(take_shortcut(&f, &action) && action == INPUT_COMMAND_SCROLL_UP);
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

/* Sends an Alt key that commits `text`, as a layout that types a symbol with
 * Alt does, and routes the queued events. */
static void alt_text_key(struct fixture *f, enum yasl_key code, uint32_t label,
                         const char *text)
{
    static uint64_t text_id;
    struct yasl_event key = {
        .type = YASL_EVENT_KEY,
        .action = YASL_KEY_PRESS,
        .mods = YASL_MOD_ALT,
        .key = {
            .key = code, .codepoint = label,
            .text_input = YASL_KEY_TEXT_COMMIT, .text_id = ++text_id,
            .consumed_mods = YASL_MOD_ALT,
        },
    };
    window_native_input(f->window, &key);
    window_native_input(f->window, &(struct yasl_event){
        .type = YASL_EVENT_TEXT, .text = text, .length = strlen(text),
        .ime = { .key_id = key.key.text_id },
    });
    key.action = YASL_KEY_RELEASE;
    key.key.text_id = 0;
    window_native_input(f->window, &key);
    struct input_event ev;
    while (input_queue_take(&f->window->input, &ev)) {
        window_ui_test_route(f->window, &ev);
        input_event_release(&ev);
    }
}

/* With no focused widget, the keys go to the default focus. The scope of
 * the default focus applies. */
static void default_focus_scope_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;
    struct ui_surface *keys = window_ui_key_surface(f.window);
    ui_surface_focus(keys, NULL);
    ui_surface_set_default_focus(keys, &f.a.widget);
    press(&f, 'j');
    assert(take_shortcut(&f, &action) && action == f.id_a);
    assert(deliver_shortcut(&f, action, &f.a));
    ui_surface_set_default_focus(keys, NULL);
    teardown(&f);
}

/* A modal with a scope and no focusable child gets the keys. Its scope
 * applies. */
static void modal_scope_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;
    struct probe modal;
    probe_init(&modal, SCOPE_B, true);
    ui_widget_set_focusable(&modal.widget, false);
    focus(&f, &f.plain);
    assert(ui_widget_add_child(&f.root, &modal.widget));
    ui_widget_set_modal(&modal.widget, true);
    press(&f, 'k');
    assert(take_shortcut(&f, &action) && action == f.id_b);
    assert(deliver_shortcut(&f, action, &modal));
    ui_widget_set_modal(&modal.widget, false);
    ui_widget_destroy(&modal.widget);
    teardown(&f);
}

/* A scoped chord that types text runs its command. Its text does not also
 * reach the widget. */
static void scoped_text_chord_test(void)
{
    struct fixture f;
    show(&f);
    input_command_id id = input_command_add(&f.reg_a, SCOPE_A, "probe_a8", "A8");
    assert(input_keymap_apply_spec_registry(&f.window->keymap, &f.reg_a, SCOPE_A,
                                            "alt+8 = probe_a8"));
    focus(&f, &f.a);
    alt_text_key(&f, YASL_KEY_8, '8', "[");
    assert(f.a.shortcuts == 1 && f.a.last_action == (int)id);
    assert(f.a.texts == 0);
    teardown(&f);
}

/* A widget that takes a bound key gets the text of the key. The keymap
 * does not run the command. */
static void taken_bound_key_keeps_text_test(void)
{
    struct fixture f;
    show(&f);
    assert(input_keymap_apply_spec(&f.window->keymap, "alt+9 = scroll_down"));
    f.plain.take_keys = true;
    focus(&f, &f.plain);
    alt_text_key(&f, YASL_KEY_9, '9', "]");
    assert(f.plain.texts == 1);
    assert(f.plain.shortcuts == 0);

    /* A widget that does not take the key leaves it to the keymap. The
     * command runs, and the text is dropped. */
    f.plain.take_keys = false;
    alt_text_key(&f, YASL_KEY_9, '9', "]");
    assert(f.plain.texts == 1);
    assert(f.plain.shortcuts == 1 &&
           f.plain.last_action == INPUT_COMMAND_SCROLL_DOWN);
    teardown(&f);
}

/* A widget follows the scope workflow of command.h with the public calls
 * only. It takes a scope, adds a command, and binds a chord in the window.
 * The chord runs the command, also after an input.conf reload. After the
 * widget clears its scope, the chord runs nothing. */
static void public_scope_workflow_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;
    uint8_t scope = input_command_scope_new();
    assert(scope != INPUT_COMMAND_SCOPE_APP);
    struct input_command_registry reg;
    input_command_registry_init(&reg);
    input_command_id id = input_command_add(&reg, scope, "probe_new", "New");
    assert(id != INPUT_COMMAND_NONE);
    f.plain.widget.command_scope = scope;

    assert(window_bind_scope(f.window, &reg, scope, "ctrl+k = probe_new"));
    assert(!window_bind_scope(f.window, &reg, INPUT_COMMAND_SCOPE_APP,
                              "ctrl+k = probe_new"));
    assert(!window_bind_scope(f.window, &reg, INPUT_COMMAND_SCOPE_COUNT,
                              "ctrl+k = probe_new"));
    assert(!window_bind_scope(f.window, &reg, scope, "ctrl+k = no_such"));
    assert(!window_bind_scope(f.window, &reg, scope, "ctrl+"));

    focus(&f, &f.plain);
    press(&f, 'k');
    assert(take_shortcut(&f, &action) && action == id);
    assert(deliver_shortcut(&f, action, &f.plain));

    /* A reload with a clear line replaces the app bindings only. */
    char path[] = "/tmp/wsap-key-route-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    fputs("clear\nctrl+y = scroll_down\n", file);
    assert(fclose(file) == 0);
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);
    window_host_reload_input(&host);
    press(&f, 'y');
    assert(take_shortcut(&f, &action) && action == INPUT_COMMAND_SCROLL_DOWN);
    press(&f, 'k');
    assert(take_shortcut(&f, &action) && action == id);
    assert(deliver_shortcut(&f, action, &f.plain));

    /* The owner clears its scope. The chord runs nothing, and the app
     * binding stays. */
    window_clear_scope(f.window, scope);
    press(&f, 'k');
    assert(!take_shortcut(&f, &action));
    press(&f, 'y');
    assert(take_shortcut(&f, &action) && action == INPUT_COMMAND_SCROLL_DOWN);

    input_command_registry_free(&reg);
    input_command_scope_release(scope);
    unlink(path);
    assert(setenv("WSAP_INPUT_CONFIG", "NONE", 1) == 0);
    input_config_init(NULL);
    teardown(&f);
}

/* The release path drops the chords of a scope in every window. A later
 * owner of the same scope gets none of the old chords. */
static void release_scope_test(void)
{
    struct fixture f;
    show(&f);
    int action = 0;
    struct window *two = window_create(&host, &(struct window_spec){0});
    assert(two);
    uint8_t scope = input_command_scope_new();
    assert(scope != INPUT_COMMAND_SCOPE_APP);
    struct input_command_registry reg;
    input_command_registry_init(&reg);
    input_command_id old = input_command_add(&reg, scope, "probe_old", "Old");
    assert(old != INPUT_COMMAND_NONE);
    assert(window_bind_scope(f.window, &reg, scope, "ctrl+k = probe_old"));
    assert(window_bind_scope(two, &reg, scope, "ctrl+k = probe_old"));
    f.plain.widget.command_scope = scope;
    focus(&f, &f.plain);
    press(&f, 'k');
    assert(take_shortcut(&f, &action) && action == old);

    input_command_registry_free(&reg);
    window_host_release_scope(&host, scope);
    struct input_key_event ctrl_k = { .codepoint = 'k', .mods = INPUT_MOD_CTRL };
    assert(input_keymap_lookup_scoped(window_keymap(two), &ctrl_k, scope) ==
           INPUT_COMMAND_NONE);

    /* A new owner takes the scope and adds a command. The old chord does
     * not run it. */
    uint8_t again = input_command_scope_new();
    assert(again != INPUT_COMMAND_SCOPE_APP);
    struct input_command_registry next;
    input_command_registry_init(&next);
    assert(input_command_add(&next, again, "probe_next", "Next") !=
           INPUT_COMMAND_NONE);
    f.plain.widget.command_scope = again;
    press(&f, 'k');
    assert(!take_shortcut(&f, &action));

    input_command_registry_free(&next);
    window_host_release_scope(&host, again);
    window_destroy(two);
    teardown(&f);
}

int main(void)
{
    two_scopes_test();
    zero_scope_app_binding_test();
    focus_chain_scope_test();
    bad_scope_keeps_app_bindings_test();
    default_focus_scope_test();
    modal_scope_test();
    scoped_text_chord_test();
    taken_bound_key_keeps_text_test();
    public_scope_workflow_test();
    release_scope_test();
    return 0;
}
