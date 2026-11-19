#include "util/text_limit.h"
#include "input/command.h"
#include "input/input.h"
#include "input/keybind.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum input_shortcut shortcut(struct input_key_event ev,
                                    const struct input_shortcut_policy *p)
{
    struct input_keymap km;
    input_keymap_defaults(&km, p, INPUT_KEYBIND_LEGACY);
    enum input_shortcut sc = input_keymap_lookup(&km, &ev);
    input_keymap_free(&km);
    return sc;
}
#define SC(p, ...) shortcut((struct input_key_event){ __VA_ARGS__ }, p)

/* A bound key matches without the queue. The widget path uses the test to
 * tell an app command from terminal input. */
static void keymap_match_test(void)
{
    static const struct input_shortcut_policy lin = {
        .accel = INPUT_MOD_CTRL, .zoom = INPUT_MOD_CTRL,
        .accel_alt = INPUT_MOD_CTRL | INPUT_MOD_SHIFT, .tab_accel = INPUT_MOD_CTRL,
    };
    struct input_keymap km;
    input_keymap_defaults(&km, &lin, INPUT_KEYBIND_MODERN);
    struct input_key_event ev = {0};
    ev.key = INPUT_KEY_NONE;
    ev.codepoint = 'c';
    ev.mods = INPUT_MOD_CTRL;
    assert(input_keymap_match(&km, &ev));
    ev.mods = INPUT_MOD_ALT;
    assert(!input_keymap_match(&km, &ev));
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_NONE);
    input_keymap_free(&km);
}

/* A binding with no modifier runs from a bare key. */
static void zero_modifier_binding_test(void)
{
    struct input_keymap km;
    input_keymap_init(&km);
    assert(input_keymap_apply_spec(&km, "escape = command_palette"));
    struct input_key_event ev = { .key = INPUT_KEY_ESCAPE };
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_COMMAND_PALETTE);
    assert(input_keymap_apply_spec(&km, "escape = none"));
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_NONE);
    input_keymap_free(&km);
}

/* A key token and a codepoint do not share a bit. A codepoint above 0x7fff
 * stays distinct, and a codepoint above 0xffff is not truncated. */
static void wide_codepoint_binding_test(void)
{
    struct input_keymap km;
    input_keymap_init(&km);
    assert(input_keymap_apply_spec(&km, "ctrl+\xcf\x80 = copy"));
    assert(input_keymap_apply_spec(&km, "ctrl+\xf0\x9f\x98\x80 = paste"));
    struct input_key_event ev = { .key = INPUT_KEY_NONE, .mods = INPUT_MOD_CTRL };
    ev.codepoint = 0x3c0;
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_COPY);
    ev.codepoint = 0x1f600;
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_PASTE);
    ev.codepoint = 0x3c1;
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_NONE);
    ev.codepoint = 0x1f601;
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_NONE);
    input_keymap_free(&km);
}

/* The map grows past the old 128-binding limit. */
static void keymap_grows_test(void)
{
    struct input_keymap km;
    input_keymap_init(&km);
    static const unsigned mods[] = {
        INPUT_MOD_CTRL, INPUT_MOD_ALT, INPUT_MOD_CTRL | INPUT_MOD_ALT,
    };
    static const char *const names[] = { "ctrl+", "alt+", "ctrl+alt+" };
    unsigned added = 0;
    for (size_t m = 0; m < 3; m++) {
        for (char c = '!'; c <= '~'; c++) {
            if (c == '+')
                continue;
            char spec[64];
            snprintf(spec, sizeof spec, "%s%c = copy", names[m], c);
            assert(input_keymap_apply_spec(&km, spec));
            added++;
        }
    }
    assert(added > 128);
    assert(km.n > 128);
    struct input_key_event ev = { .codepoint = '~', .mods = mods[2] };
    assert(input_keymap_lookup(&km, &ev) == INPUT_SHORTCUT_COPY);
    input_keymap_free(&km);
}


/* The built-in registry names every app command. The name and the label
 * agree with the ID in both directions. */
static void command_registry_test(void)
{
    const struct input_command_registry *reg = input_command_builtin();
    assert(reg && reg->n > 0);

    const struct input_command *copy =
        input_command_find(reg, INPUT_COMMAND_SCOPE_APP, INPUT_SHORTCUT_COPY);
    assert(copy && copy->id == INPUT_SHORTCUT_COPY);
    assert(!strcmp(copy->name, "copy"));
    assert(copy->label && copy->label[0]);

    const struct input_command *by_name = input_command_find_name(reg, "new_tab");
    assert(by_name && by_name->id == INPUT_SHORTCUT_NEW_TAB);
    /* The config word for a command parses back to the same ID. */
    enum input_shortcut id;
    assert(input_parse_action("new_tab", &id) && id == INPUT_SHORTCUT_NEW_TAB);
    assert(input_parse_action("none", &id) && id == INPUT_SHORTCUT_NONE);
    assert(!input_parse_action("no_such_command", &id));

    /* A widget adds a command in its own scope. */
    struct input_command_registry own;
    input_command_registry_init(&own);
    assert(input_command_add(&own, 3, INPUT_SHORTCUT_SETTINGS,
                             "widget_settings", "Widget Settings"));
    const struct input_command *scoped =
        input_command_find(&own, 3, INPUT_SHORTCUT_SETTINGS);
    assert(scoped && scoped->scope == 3);
    assert(!strcmp(scoped->label, "Widget Settings"));
    /* The app scope has the same ID and keeps its own entry. */
    const struct input_command *app =
        input_command_find(&own, INPUT_COMMAND_SCOPE_APP, INPUT_SHORTCUT_SETTINGS);
    assert(!app);
    assert(!input_command_add(&own, INPUT_COMMAND_SCOPE_COUNT,
                              INPUT_SHORTCUT_COPY, "x", "X"));
    input_command_registry_free(&own);
    assert(!own.cmds && !own.n);
}

/* A command that a widget binds wins over the app command on the same
 * chord. A scope without a binding reads the app binding. */
static void scoped_binding_wins_test(void)
{
    struct input_keymap km;
    input_keymap_init(&km);
    assert(input_keymap_apply_spec(&km, "ctrl+w = close_tab"));
    assert(input_keymap_apply_spec_scoped(&km, 2, "ctrl+w = settings"));

    struct input_key_event ev = { .key = INPUT_KEY_NONE, .codepoint = 'w',
                                 .mods = INPUT_MOD_CTRL };
    assert(input_keymap_lookup_scoped(&km, &ev, 2) == INPUT_SHORTCUT_SETTINGS);
    assert(input_keymap_lookup_scoped(&km, &ev, 0) == INPUT_SHORTCUT_CLOSE_TAB);
    assert(input_keymap_lookup_scoped(&km, &ev, 5) == INPUT_SHORTCUT_CLOSE_TAB);

    /* A widget unbinds its own chord. The app chord still runs there. */
    assert(input_keymap_apply_spec_scoped(&km, 2, "ctrl+w = none"));
    assert(input_keymap_lookup_scoped(&km, &ev, 2) == INPUT_SHORTCUT_CLOSE_TAB);

    /* A non-ASCII key binds in a widget scope without a modifier. */
    assert(input_keymap_apply_spec_scoped(&km, 2, "\xc3\xa9 = command_palette"));
    struct input_key_event e_acute = { .key = INPUT_KEY_NONE, .codepoint = 0xe9 };
    assert(input_keymap_lookup_scoped(&km, &e_acute, 2) == INPUT_SHORTCUT_COMMAND_PALETTE);
    assert(input_keymap_lookup_scoped(&km, &e_acute, 0) == INPUT_SHORTCUT_NONE);
    input_keymap_free(&km);
}

int main(void)
{
    command_registry_test();
    scoped_binding_wins_test();
    keymap_match_test();
    zero_modifier_binding_test();
    wide_codepoint_binding_test();
    keymap_grows_test();
    static const struct input_shortcut_policy lin = {
        .accel = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
        .zoom = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
    };
    static const struct input_shortcut_policy mac = {
        .accel = INPUT_MOD_SUPER,
        .accel_alt = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
        .zoom = INPUT_MOD_SUPER | INPUT_MOD_SHIFT,
    };
    unsigned ctrl_shift = INPUT_MOD_CTRL | INPUT_MOD_SHIFT;

    assert(SC(&lin, .codepoint = 'c', .mods = ctrl_shift) == INPUT_SHORTCUT_COPY);
    assert(SC(&mac, .codepoint = 'c', .mods = INPUT_MOD_SUPER) == INPUT_SHORTCUT_COPY);
    assert(SC(&mac, .codepoint = 'c', .mods = ctrl_shift) == INPUT_SHORTCUT_COPY);

    assert(SC(&lin, .codepoint = 'c', .mods = INPUT_MOD_CTRL) == INPUT_SHORTCUT_NONE);
    assert(SC(&lin, .codepoint = 'c', .mods = INPUT_MOD_SUPER) == INPUT_SHORTCUT_NONE);

    assert(SC(&lin, .codepoint = 'v', .mods = ctrl_shift | INPUT_MOD_CAPS) ==
           INPUT_SHORTCUT_PASTE);
    assert(SC(&lin, .key = INPUT_KEY_INSERT, .mods = INPUT_MOD_SHIFT) ==
           INPUT_SHORTCUT_PASTE);
    assert(SC(&lin, .key = INPUT_KEY_PAGE_UP, .mods = INPUT_MOD_SHIFT) ==
           INPUT_SHORTCUT_SCROLL_UP);
    assert(SC(&lin, .key = INPUT_KEY_PAGE_DOWN, .mods = INPUT_MOD_SHIFT) ==
           INPUT_SHORTCUT_SCROLL_DOWN);

#ifdef __APPLE__
    assert(SC(&mac, .codepoint = '=', .mods = INPUT_MOD_SUPER | INPUT_MOD_SHIFT) ==
           INPUT_SHORTCUT_FONT_LARGER);
#else
    assert(SC(&lin, .codepoint = '=', .mods = INPUT_MOD_CTRL) ==
           INPUT_SHORTCUT_FONT_LARGER);
#endif
    assert(SC(&lin, .keypad = INPUT_KEY_KP_ADD, .mods = ctrl_shift) ==
           INPUT_SHORTCUT_FONT_LARGER);
#ifdef __APPLE__
    assert(SC(&mac, .codepoint = '-', .mods = INPUT_MOD_SUPER | INPUT_MOD_SHIFT) ==
           INPUT_SHORTCUT_FONT_SMALLER);
#else
    assert(SC(&lin, .codepoint = '-', .mods = INPUT_MOD_CTRL) ==
           INPUT_SHORTCUT_FONT_SMALLER);
    assert(SC(&lin, .codepoint = '=', .shifted = '+', .base = '=',
              .mods = ctrl_shift) == INPUT_SHORTCUT_NONE);
#endif
#ifdef __APPLE__
    assert(SC(&mac, .codepoint = '4', .shifted = '+', .base = '4',
              .mods = INPUT_MOD_SUPER) == INPUT_SHORTCUT_TAB_4);
#else
    assert(SC(&lin, .codepoint = '4', .shifted = '+', .base = '4',
              .mods = ctrl_shift) == INPUT_SHORTCUT_TAB_4);
#endif

#ifdef __APPLE__
    assert(SC(&mac, .key = INPUT_KEY_BACKSPACE, .mods = INPUT_MOD_SUPER) ==
           INPUT_SHORTCUT_KILL_LINE);
    assert(SC(&mac, .codepoint = 'p',
              .mods = INPUT_MOD_SUPER | INPUT_MOD_SHIFT) ==
           INPUT_SHORTCUT_COMMAND_PALETTE);
#else
    assert(SC(&lin, .codepoint = 'p', .mods = ctrl_shift) ==
           INPUT_SHORTCUT_COMMAND_PALETTE);
#endif

    {
        enum input_keybind_style style = INPUT_KEYBIND_MODERN;
        assert(input_parse_keybind_style("Legacy", &style) && style == INPUT_KEYBIND_LEGACY);
        assert(input_parse_keybind_style("modern", &style) && style == INPUT_KEYBIND_MODERN);
        assert(!input_parse_keybind_style("classic", &style) && style == INPUT_KEYBIND_MODERN);
    }

#ifndef __APPLE__
    {
        struct input_keymap modern, legacy;
        input_keymap_defaults(&modern, &lin, INPUT_KEYBIND_MODERN);
        input_keymap_defaults(&legacy, &lin, INPUT_KEYBIND_LEGACY);
        struct input_key_event ctrl_c = { .codepoint = 'c', .mods = INPUT_MOD_CTRL };
        struct input_key_event ctrl_v = { .codepoint = 'v', .mods = INPUT_MOD_CTRL };
        struct input_key_event ctrl_shift_c = { .codepoint = 'c', .mods = ctrl_shift };
        struct input_key_event ctrl_t = { .codepoint = 't', .mods = INPUT_MOD_CTRL };
        struct input_key_event ctrl_shift_t = { .codepoint = 't', .mods = ctrl_shift };
        assert(input_keymap_lookup(&modern, &ctrl_c) == INPUT_SHORTCUT_COPY);
        assert(input_keymap_lookup(&modern, &ctrl_v) == INPUT_SHORTCUT_PASTE);
        assert(input_keymap_lookup(&legacy, &ctrl_c) == INPUT_SHORTCUT_NONE);
        assert(input_keymap_lookup(&legacy, &ctrl_v) == INPUT_SHORTCUT_NONE);
        assert(input_keymap_lookup(&modern, &ctrl_shift_c) == INPUT_SHORTCUT_COPY);
        assert(input_keymap_lookup(&legacy, &ctrl_shift_c) == INPUT_SHORTCUT_COPY);
        assert(input_keymap_lookup(&modern, &ctrl_t) == INPUT_SHORTCUT_NONE);
        assert(input_keymap_lookup(&legacy, &ctrl_t) == INPUT_SHORTCUT_NONE);
        assert(input_keymap_lookup(&modern, &ctrl_shift_t) == INPUT_SHORTCUT_NEW_TAB);
        assert(input_keymap_lookup(&legacy, &ctrl_shift_t) == INPUT_SHORTCUT_NEW_TAB);
        /* A zero modifier set binds nothing. The bare letters must reach the
         * shell in both styles. */
        struct input_key_event plain_c = { .codepoint = 'c' };
        struct input_key_event plain_v = { .codepoint = 'v' };
        assert(input_keymap_lookup(&modern, &plain_c) == INPUT_SHORTCUT_NONE);
        assert(input_keymap_lookup(&legacy, &plain_c) == INPUT_SHORTCUT_NONE);
        assert(input_keymap_lookup(&legacy, &plain_v) == INPUT_SHORTCUT_NONE);
    }
#endif

    {
        struct input_queue q = {0};
        char big[9000];
        memset(big, 'x', sizeof(big));
        for (size_t i = 0; i < sizeof(big); i += 97) big[i] = 'a' + (i % 26);

        assert(input_queue_push_paste(&q, big, sizeof(big)) == sizeof(big));
        size_t got = 0;
        struct input_event pev;
        while (input_queue_take(&q, &pev)) {
            assert(pev.kind == INPUT_PASTE);
            assert(pev.paste.len == sizeof(big));
            assert(!memcmp(pev.paste.data, big + got, pev.paste.len));
            got += pev.paste.len;
            input_event_release(&pev);
        }
        assert(got == sizeof(big));

        for (int i = 0; i < INPUT_QUEUE_SIZE - 1; i++)
            input_queue_add(&q, &(struct input_event){
                .kind = INPUT_KEY,
                .key = { .key = INPUT_KEY_RETURN, .action = INPUT_PRESS },
            });
        assert(input_queue_push_paste(&q, big, sizeof(big)) == 0);

        struct input_event dev;
        int pasted = 0;
        while (input_queue_take(&q, &dev)) {
            if (dev.kind == INPUT_PASTE) pasted++;
            input_event_release(&dev);
        }
        assert(pasted == 0);
        assert(q.head == q.tail);
    }

    {
        struct input_queue q = {0};
        struct input_event down = {
            .kind = INPUT_MOUSE,
            .mouse = { .kind = PTR_SCROLL, .button = POINTER_WHEEL_DOWN, .scroll_lines = -1 },
        };
        struct input_event right = {
            .kind = INPUT_MOUSE,
            .mouse = { .kind = PTR_SCROLL, .button = POINTER_WHEEL_RIGHT, .scroll_lines = 1 },
        };
        assert(input_queue_add(&q, &down) == INPUT_PUSH_OK);
        assert(input_queue_add(&q, &down) == INPUT_PUSH_OK);
        assert(input_queue_add(&q, &right) == INPUT_PUSH_OK);
        assert(input_queue_add(&q, &down) == INPUT_PUSH_OK);
        struct input_event ev;
        assert(input_queue_take(&q, &ev));
        assert(ev.mouse.button == POINTER_WHEEL_DOWN && ev.mouse.scroll_lines == -2);
        assert(input_queue_take(&q, &ev));
        assert(ev.mouse.button == POINTER_WHEEL_RIGHT && ev.mouse.scroll_lines == 1);
        assert(input_queue_take(&q, &ev));
        assert(ev.mouse.button == POINTER_WHEEL_DOWN && ev.mouse.scroll_lines == -1);
        assert(!input_queue_take(&q, &ev));
    }

    {

        struct input_queue q = {0};
        struct input_event key_ev = {
            .kind = INPUT_KEY,
            .key = { .key = INPUT_KEY_RETURN, .action = INPUT_PRESS },
        };
        struct input_event mot_ev = {
            .kind = INPUT_MOUSE,
            .mouse = { .kind = PTR_MOTION, .button = 3 },
        };

        for (int i = 0; i < INPUT_QUEUE_SIZE - 1; i++)
            assert(input_queue_add(&q, &key_ev) == INPUT_PUSH_OK);
        assert(input_queue_add(&q, &key_ev) == INPUT_PUSH_DROPPED);
        assert(input_queue_add(&q, &mot_ev) == INPUT_PUSH_DROPPED);

        input_queue_reset(&q);
        mot_ev.mouse.pos.x = 42;
        assert(input_queue_add(&q, &mot_ev) == INPUT_PUSH_OK);
        for (int i = 0; i < INPUT_QUEUE_SIZE - 2; i++)
            assert(input_queue_add(&q, &key_ev) == INPUT_PUSH_OK);
        assert(input_queue_add(&q, &key_ev) == INPUT_PUSH_EVICTED_MOTION);
        struct input_event first;
        assert(input_queue_take(&q, &first));
        assert(first.kind == INPUT_KEY);

        input_queue_reset(&q);
        char paste[5000];
        memset(paste, 'p', sizeof(paste));
        assert(input_queue_push_paste(&q, paste, sizeof(paste)) == sizeof(paste));
        input_queue_reset(&q);
        assert(q.head == q.tail);
    }

    {
        struct input_queue q = {0};
        char *text = malloc(INPUT_TEXT_MAX);
        assert(text);
        memset(text, 'p', INPUT_TEXT_MAX);
        assert(input_queue_push_paste(&q, text, SIZE_MAX) == 0);
        assert(input_queue_push_paste(&q, text, INPUT_TEXT_MAX) == INPUT_TEXT_MAX);
        assert(input_queue_push_paste(&q, "x", 1) == 0);
        struct input_event ev;
        assert(input_queue_take(&q, &ev));
        assert(ev.paste.len == INPUT_TEXT_MAX);
        assert(!memcmp(ev.paste.data, text, INPUT_TEXT_MAX));
        input_event_release(&ev);
        assert(input_queue_push_paste(&q, "x", 1) == 1);
        input_queue_reset(&q);
        assert(input_queue_push_paste(&q, text, INPUT_TEXT_MAX) == INPUT_TEXT_MAX);
        input_queue_reset(&q);
        free(text);
    }

    {
        char sc[64];
        assert(input_format_chord(INPUT_MOD_CTRL, 'c', INPUT_KEY_NONE, sc, sizeof(sc)));
        assert(!strcmp(sc, "Ctrl+C"));

        assert(input_format_chord(INPUT_MOD_CTRL | INPUT_MOD_SHIFT, 'c', INPUT_KEY_NONE, sc, sizeof(sc)));
        assert(!strcmp(sc, "Ctrl+Shift+C"));

        assert(input_format_chord(INPUT_MOD_SHIFT, 0, INPUT_KEY_INSERT, sc, sizeof(sc)));
        assert(!strcmp(sc, "Shift+Insert"));

        assert(input_format_chord(INPUT_MOD_HYPER, 'k', INPUT_KEY_NONE, sc, sizeof(sc)));
        assert(!strcmp(sc, "Hyper+K"));

        struct input_keymap km;
        input_keymap_init(&km);
        input_keymap_defaults(&km, &input_platform_shortcuts, INPUT_KEYBIND_MODERN);
        assert(input_keymap_format_action_chord(&km, INPUT_SHORTCUT_COPY, sc, sizeof(sc)));
        assert(!strcmp(sc, "Ctrl+C") || !strcmp(sc, "Cmd+C"));

        assert(input_keymap_format_action_chord(&km, INPUT_SHORTCUT_PASTE, sc, sizeof(sc)));
        assert(!strcmp(sc, "Ctrl+V") || !strcmp(sc, "Cmd+V"));
        input_keymap_free(&km);
    }

    {
        struct input_keyboard_state keyboard = {0};
        struct input_key_event key = { .key = INPUT_KEY_LEFT, .action = INPUT_PRESS };
        input_keyboard_update(&keyboard, &key);
        assert(input_keyboard_key_down(&keyboard, INPUT_KEY_LEFT));
        key.action = INPUT_RELEASE;
        input_keyboard_update(&keyboard, &key);
        assert(!input_keyboard_key_down(&keyboard, INPUT_KEY_LEFT));
        key.action = INPUT_PRESS;
        input_keyboard_update(&keyboard, &key);
        input_keyboard_reset(&keyboard);
        assert(!input_keyboard_key_down(&keyboard, INPUT_KEY_LEFT));
    }
    {
        struct input_queue queue = {0};
        const char uris[] = "file:///tmp/a%20b.txt\r\n";
        assert(input_queue_push_drop_files(&queue, uris, sizeof(uris) - 1)
               == sizeof(uris) - 1);
        struct input_event event;
        assert(input_queue_take(&queue, &event));
        assert(event.kind == INPUT_DROP_FILES);
        assert(event.drop.len == sizeof(uris) - 1);
        assert(!memcmp(event.drop.data, uris, event.drop.len));
        input_event_release(&event);
        assert(!input_queue_take(&queue, &event));
        char text[100];
        memset(text, 'x', sizeof(text));
        input_queue_push_text(&queue, text, sizeof(text), INPUT_TEXT_KEY);
        size_t n = 0;
        while (input_queue_take(&queue, &event)) {
            assert(event.kind == INPUT_TEXT && event.text.source == INPUT_TEXT_KEY);
            assert(!memcmp(event.text.utf8, text + n, event.text.len));
            n += event.text.len;
        }
        assert(n == sizeof(text));
    }
    {
        struct input_queue queue = {0};
        struct input_event key = { .kind = INPUT_KEY,
            .key = { .key = INPUT_KEY_RETURN, .action = INPUT_PRESS } };
        for (int i = 0; i < INPUT_QUEUE_SIZE - 1; i++)
            assert(input_queue_add(&queue, &key) == INPUT_PUSH_OK);
        char text[100];
        memset(text, 'x', sizeof(text));
        assert(input_queue_push_text(&queue, text, sizeof(text), INPUT_TEXT_COMMIT) == 0);
        struct input_event event;
        assert(input_queue_take(&queue, &event));
        assert(input_queue_push_text(&queue, text, sizeof(text), INPUT_TEXT_COMMIT) == 48);
        size_t received = 0;
        while (input_queue_take(&queue, &event)) {
            if (event.kind == INPUT_TEXT) received += event.text.len;
            input_event_release(&event);
        }
        assert(received == 48);
    }
    puts("PASS: input queues, keyboard state, and shortcuts");
    return 0;
}
