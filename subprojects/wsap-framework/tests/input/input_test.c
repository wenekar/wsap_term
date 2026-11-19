#define _GNU_SOURCE
#include "util/text_limit.h"
#include "input/command.h"
#include "input/input.h"
#include "input/keybind.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static input_command_id shortcut(struct input_key_event ev,
                                    const struct input_shortcut_policy *p)
{
    struct input_keymap km;
    input_keymap_defaults(&km, p, INPUT_KEYBIND_LEGACY);
    input_command_id sc = input_keymap_lookup(&km, &ev);
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
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_NONE);
    input_keymap_free(&km);
}

/* A binding with no modifier runs from a bare key. */
static void zero_modifier_binding_test(void)
{
    struct input_keymap km;
    input_keymap_init(&km);
    assert(input_keymap_apply_spec(&km, "escape = command_palette"));
    struct input_key_event ev = { .key = INPUT_KEY_ESCAPE };
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_COMMAND_PALETTE);
    assert(input_keymap_apply_spec(&km, "escape = none"));
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_NONE);
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
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_COPY);
    ev.codepoint = 0x1f600;
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_PASTE);
    ev.codepoint = 0x3c1;
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_NONE);
    ev.codepoint = 0x1f601;
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_NONE);
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
    assert(input_keymap_lookup(&km, &ev) == INPUT_COMMAND_COPY);
    input_keymap_free(&km);
}


/* The built-in registry names every app command. The name and the label
 * agree with the ID in both directions. */
static void command_registry_test(void)
{
    const struct input_command_registry *reg = input_command_builtin();
    assert(reg && reg->n > 0);

    const struct input_command *copy =
        input_command_find(reg, INPUT_COMMAND_SCOPE_APP, INPUT_COMMAND_COPY);
    assert(copy && copy->id == INPUT_COMMAND_COPY);
    assert(!strcmp(copy->name, "copy"));
    assert(copy->label && copy->label[0]);

    const struct input_command *by_name = input_command_find_name(reg, "new_tab");
    assert(by_name && by_name->id == INPUT_COMMAND_NEW_TAB);
    /* The config word for a command parses back to the same ID. */
    input_command_id id;
    assert(input_parse_action("new_tab", &id) && id == INPUT_COMMAND_NEW_TAB);
    assert(input_parse_action("none", &id) && id == INPUT_COMMAND_NONE);
    assert(!input_parse_action("no_such_command", &id));

    /* A widget adds a command in its own scope. The registry gives the ID. */
    uint8_t scope = input_command_scope_new();
    assert(scope != INPUT_COMMAND_SCOPE_APP);
    struct input_command_registry own;
    input_command_registry_init(&own);
    input_command_id widget_id =
        input_command_add(&own, scope, "widget_settings", "Widget Settings");
    assert(widget_id >= INPUT_COMMAND_DYNAMIC_ID);
    const struct input_command *scoped = input_command_find(&own, scope, widget_id);
    assert(scoped && scoped->scope == scope);
    assert(!strcmp(scoped->label, "Widget Settings"));
    assert(!strcmp(input_command_label(&own, scope, widget_id), "Widget Settings"));
    assert(!strcmp(input_command_label(&own, scope, INPUT_COMMAND_COPY), ""));
    /* The app scope has no entry for the widget ID. */
    const struct input_command *app =
        input_command_find(&own, INPUT_COMMAND_SCOPE_APP, widget_id);
    assert(!app);
    /* A second command takes the next ID. */
    input_command_id other =
        input_command_add(&own, scope, "widget_other", "Widget Other");
    assert(other == widget_id + 1);
    assert(input_command_add(&own, INPUT_COMMAND_SCOPE_COUNT,
                             "x", "X") == INPUT_COMMAND_NONE);
    input_command_registry_free(&own);
    assert(!own.cmds && !own.n);
    input_command_scope_release(scope);
}

/* Every default chord gives the same command that its config word names.
 * The table reads the default keymap, so a new default binding is covered. */
static void default_chord_table_test(void)
{
    static const struct input_shortcut_policy policies[] = {
        { .accel = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
          .accel_alt = INPUT_MOD_CTRL,
          .zoom = INPUT_MOD_CTRL,
          .tab_accel = INPUT_MOD_CTRL | INPUT_MOD_SHIFT },
        { .accel = INPUT_MOD_SUPER,
          .accel_alt = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
          .zoom = INPUT_MOD_SUPER | INPUT_MOD_SHIFT,
          .tab_accel = INPUT_MOD_SUPER },
    };
    const struct input_command_registry *reg = input_command_builtin();
    unsigned checked = 0;
    for (size_t p = 0; p < sizeof(policies) / sizeof(policies[0]); p++) {
        struct input_keymap km;
        input_keymap_defaults(&km, &policies[p], INPUT_KEYBIND_LEGACY);
        assert(km.n > 0);
        for (size_t i = 0; i < km.n; i++) {
            const struct input_keybind *b = &km.binds[i];
            struct input_key_event ev = {
                .codepoint = b->codepoint, .key = b->key, .mods = b->mods,
            };
            assert(input_keymap_lookup(&km, &ev) == b->action);
            const struct input_command *cmd =
                input_command_find(reg, b->scope, b->action);
            assert(cmd && cmd->name);
            input_command_id parsed = INPUT_COMMAND_NONE;
            assert(input_parse_action(cmd->name, &parsed));
            assert(parsed == b->action);
            checked++;
        }
        input_keymap_free(&km);
    }
    assert(checked > 20);
    /* The registry names every app command. */
    for (input_command_id id = INPUT_COMMAND_FONT_LARGER;
         id < INPUT_COMMAND_APP_COUNT; id++) {
        const struct input_command *cmd =
            input_command_find(reg, INPUT_COMMAND_SCOPE_APP, id);
        assert(cmd && cmd->name && cmd->label && cmd->label[0]);
    }
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
    assert(input_keymap_lookup_scoped(&km, &ev, 2) == INPUT_COMMAND_SETTINGS);
    assert(input_keymap_lookup_scoped(&km, &ev, 0) == INPUT_COMMAND_CLOSE_TAB);
    assert(input_keymap_lookup_scoped(&km, &ev, 5) == INPUT_COMMAND_CLOSE_TAB);

    /* A widget unbinds its own chord. The app chord still runs there. */
    assert(input_keymap_apply_spec_scoped(&km, 2, "ctrl+w = none"));
    assert(input_keymap_lookup_scoped(&km, &ev, 2) == INPUT_COMMAND_CLOSE_TAB);

    /* A non-ASCII key binds in a widget scope without a modifier. */
    assert(input_keymap_apply_spec_scoped(&km, 2, "\xc3\xa9 = command_palette"));
    struct input_key_event e_acute = { .key = INPUT_KEY_NONE, .codepoint = 0xe9 };
    assert(input_keymap_lookup_scoped(&km, &e_acute, 2) == INPUT_COMMAND_COMMAND_PALETTE);
    assert(input_keymap_lookup_scoped(&km, &e_acute, 0) == INPUT_COMMAND_NONE);
    input_keymap_free(&km);
}

/* A widget command binds with the registry of its scope. Two registries in
 * different scopes never give the same ID, and a name resolves only in its
 * own registry. */
static void widget_command_binding_test(void)
{
    uint8_t two_scope = input_command_scope_new();
    uint8_t three_scope = input_command_scope_new();
    assert(two_scope && three_scope && two_scope != three_scope);
    struct input_command_registry two, three;
    input_command_registry_init(&two);
    input_command_registry_init(&three);
    input_command_id id_two =
        input_command_add(&two, two_scope, "widget_two", "Widget Two");
    input_command_id id_three =
        input_command_add(&three, three_scope, "widget_three", "Widget Three");
    assert(id_two >= INPUT_COMMAND_DYNAMIC_ID);
    assert(id_three >= INPUT_COMMAND_DYNAMIC_ID);
    assert(id_two != id_three);

    struct input_keymap km;
    input_keymap_init(&km);
    assert(input_keymap_apply_spec_registry(&km, &two, two_scope,
                                            "ctrl+j = widget_two"));
    assert(input_keymap_apply_spec_registry(&km, &three, three_scope,
                                            "ctrl+j = widget_three"));
    /* A registry does not resolve a name that another scope holds. */
    assert(!input_keymap_apply_spec_registry(&km, &three, two_scope,
                                             "ctrl+k = widget_two"));

    struct input_key_event ctrl_j = { .key = INPUT_KEY_NONE, .codepoint = 'j',
                                     .mods = INPUT_MOD_CTRL };
    assert(input_keymap_lookup_scoped(&km, &ctrl_j, two_scope) == id_two);
    assert(input_keymap_lookup_scoped(&km, &ctrl_j, three_scope) == id_three);
    assert(input_keymap_lookup_scoped(&km, &ctrl_j, 0) == INPUT_COMMAND_NONE);

    char out[64];
    assert(input_keymap_format_action_chord_scoped(&km, id_two, two_scope,
                                                   out, sizeof(out)));
    assert(!strcmp(out, "Ctrl+J"));
    /* The app scope does not read a widget binding. */
    assert(!input_keymap_format_action_chord(&km, id_two, out, sizeof(out)));
    input_keymap_free(&km);
    input_command_registry_free(&two);
    input_command_registry_free(&three);
    input_command_scope_release(two_scope);
    input_command_scope_release(three_scope);
}

/* Each owner gets its own widget scope. Two registries in two given scopes
 * never give the same ID. When every scope is taken, the call gives the app
 * scope. A released scope can be given again. */
static void scope_new_test(void)
{
    uint8_t scopes[INPUT_COMMAND_SCOPE_COUNT];
    size_t n = 0;
    for (;;) {
        uint8_t scope = input_command_scope_new();
        if (scope == INPUT_COMMAND_SCOPE_APP)
            break;
        assert(scope < INPUT_COMMAND_SCOPE_COUNT);
        for (size_t i = 0; i < n; i++)
            assert(scopes[i] != scope);
        assert(n < INPUT_COMMAND_SCOPE_COUNT - 1);
        scopes[n++] = scope;
    }
    assert(n == INPUT_COMMAND_SCOPE_COUNT - 1);

    /* Two owners add their first commands. The IDs differ. */
    struct input_command_registry a, b;
    input_command_registry_init(&a);
    input_command_registry_init(&b);
    input_command_id id_a = input_command_add(&a, scopes[0], "find", "Find");
    input_command_id id_b = input_command_add(&b, scopes[1], "save", "Save");
    assert(id_a != INPUT_COMMAND_NONE && id_b != INPUT_COMMAND_NONE);
    assert(id_a != id_b);
    input_command_registry_free(&a);
    input_command_registry_free(&b);

    /* A scope out of range gets no command. */
    struct input_command_registry c;
    input_command_registry_init(&c);
    assert(input_command_add(&c, 200, "x", "X") == INPUT_COMMAND_NONE);
    input_command_registry_free(&c);

    /* A released scope is free again. The app scope is never given. */
    input_command_scope_release(INPUT_COMMAND_SCOPE_APP);
    assert(input_command_scope_new() == INPUT_COMMAND_SCOPE_APP);
    input_command_scope_release(scopes[3]);
    assert(input_command_scope_new() == scopes[3]);
    for (size_t i = 0; i < n; i++)
        input_command_scope_release(scopes[i]);
}

/* A registry adds commands only in a scope that its owner took, and one
 * scope has one registry. A second registry in the same scope gets no ID,
 * so two registries never give the same ID. */
static void scope_owner_test(void)
{
    struct input_command_registry a, b;
    input_command_registry_init(&a);
    input_command_registry_init(&b);
    /* No owner took scope 1, so it holds no command. */
    assert(input_command_add(&a, 1, "find", "Find") == INPUT_COMMAND_NONE);

    uint8_t scope = input_command_scope_new();
    assert(scope != INPUT_COMMAND_SCOPE_APP);
    input_command_id id_a = input_command_add(&a, scope, "find", "Find");
    input_command_id id_b = input_command_add(&b, scope, "save", "Save");
    assert(id_a != INPUT_COMMAND_NONE);
    assert(id_b == INPUT_COMMAND_NONE);
    assert(input_command_add(&a, scope, "save", "Save") == id_a + 1);

    /* The owner frees its registry and makes a new one in its scope. */
    input_command_registry_free(&a);
    assert(input_command_add(&b, scope, "save", "Save") != INPUT_COMMAND_NONE);

    /* A released scope holds no new command. */
    input_command_registry_free(&b);
    input_command_scope_release(scope);
    assert(input_command_add(&b, scope, "save", "Save") == INPUT_COMMAND_NONE);
    input_command_registry_free(&b);
}

/* A widget registry binds an app command in its own scope. The binding
 * runs only in that scope. A widget command with the name of an app command
 * wins in its scope. */
static void widget_scope_binds_app_command_test(void)
{
    uint8_t scope = input_command_scope_new();
    assert(scope != INPUT_COMMAND_SCOPE_APP);
    struct input_command_registry own;
    input_command_registry_init(&own);
    input_command_id find = input_command_add(&own, scope, "find", "Find");
    input_command_id own_copy = input_command_add(&own, scope, "copy", "Copy Path");
    assert(find != INPUT_COMMAND_NONE && own_copy != INPUT_COMMAND_NONE);

    struct input_keymap km;
    input_keymap_init(&km);
    assert(input_keymap_apply_spec_registry(&km, &own, scope, "ctrl+w = close_tab"));
    assert(input_keymap_apply_spec_registry(&km, &own, scope, "ctrl+f = find"));
    assert(input_keymap_apply_spec_registry(&km, &own, scope, "ctrl+y = copy"));
    assert(!input_keymap_apply_spec_registry(&km, &own, scope, "ctrl+q = no_such"));

    struct input_key_event ctrl_w = { .codepoint = 'w', .mods = INPUT_MOD_CTRL };
    struct input_key_event ctrl_f = { .codepoint = 'f', .mods = INPUT_MOD_CTRL };
    struct input_key_event ctrl_y = { .codepoint = 'y', .mods = INPUT_MOD_CTRL };
    assert(input_keymap_lookup_scoped(&km, &ctrl_w, scope) == INPUT_COMMAND_CLOSE_TAB);
    assert(input_keymap_lookup_scoped(&km, &ctrl_w, 0) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup_scoped(&km, &ctrl_f, scope) == find);
    assert(input_keymap_lookup_scoped(&km, &ctrl_y, scope) == own_copy);
    input_keymap_free(&km);
    input_command_registry_free(&own);
    input_command_scope_release(scope);
}

/* A binding above the 'style' line survives the load. The style applies to
 * the whole file. */
static void config_style_order_test(void)
{
    char path[] = "/tmp/wsap-input-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f);
    fputs("ctrl+alt+k = new_tab\nstyle = legacy\n", f);
    assert(fclose(f) == 0);
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);
    struct input_keymap km;
    input_keymap_init(&km);
    input_config_load(&km);
    struct input_key_event bound = {
        .codepoint = 'k', .mods = INPUT_MOD_CTRL | INPUT_MOD_ALT,
    };
    assert(input_keymap_lookup(&km, &bound) == INPUT_COMMAND_NEW_TAB);
    /* The style line still selects the legacy copy and paste keys. */
    struct input_key_event ctrl_c = { .codepoint = 'c', .mods = INPUT_MOD_CTRL };
    struct input_key_event plain_c = { .codepoint = 'c' };
    assert(input_keymap_lookup(&km, &ctrl_c) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup(&km, &plain_c) == INPUT_COMMAND_NONE);
    input_keymap_free(&km);
    unlink(path);
    unsetenv("WSAP_INPUT_CONFIG");
}

/* Rewrites the input configuration at `path` with `text`. */
static void write_input_config(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    assert(fclose(f) == 0);
}

/* A reload reads the style again, so a style change needs no restart. A
 * clear line wins over a style line in either order. */
static void config_reload_style_test(void)
{
    char path[] = "/tmp/wsap-input-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    write_input_config(path, "style = modern\n");
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);

    struct input_keymap km;
    input_keymap_init(&km);
    struct input_key_event ctrl_c = { .codepoint = 'c', .mods = INPUT_MOD_CTRL };
    input_config_load(&km);
    assert(input_keymap_lookup(&km, &ctrl_c) == INPUT_COMMAND_COPY);

    /* Change the style line only. The next load uses the new style. */
    write_input_config(path, "style = legacy\n");
    input_config_load(&km);
    assert(input_keymap_lookup(&km, &ctrl_c) == INPUT_COMMAND_NONE);

    /* A clear line before a style line still clears the defaults. */
    write_input_config(path, "clear\nstyle = modern\n");
    input_config_load(&km);
    assert(input_keymap_lookup(&km, &ctrl_c) == INPUT_COMMAND_NONE);

    /* A clear line after a style line also clears the defaults. */
    write_input_config(path, "style = legacy\nclear\n");
    input_config_load(&km);
    assert(input_keymap_lookup(&km, &ctrl_c) == INPUT_COMMAND_NONE);

    input_keymap_free(&km);
    unlink(path);
    unsetenv("WSAP_INPUT_CONFIG");
}

/* A reload replaces the app bindings and keeps the widget scope bindings.
 * A stale app binding of the old file does not stay. */
static void config_reload_keeps_widget_scope_test(void)
{
    char path[] = "/tmp/wsap-input-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    write_input_config(path, "ctrl+k = new_tab\n");
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);

    uint8_t scope = input_command_scope_new();
    assert(scope != INPUT_COMMAND_SCOPE_APP);
    struct input_command_registry own;
    input_command_registry_init(&own);
    input_command_id find = input_command_add(&own, scope, "find", "Find");
    struct input_keymap km;
    input_keymap_init(&km);
    input_config_load(&km);
    assert(input_keymap_apply_spec_registry(&km, &own, scope, "ctrl+f = find"));

    struct input_key_event ctrl_f = { .codepoint = 'f', .mods = INPUT_MOD_CTRL };
    struct input_key_event ctrl_j = { .codepoint = 'j', .mods = INPUT_MOD_CTRL };
    struct input_key_event ctrl_k = { .codepoint = 'k', .mods = INPUT_MOD_CTRL };
    struct input_key_event ctrl_c = { .codepoint = 'c', .mods = INPUT_MOD_CTRL };
    assert(input_keymap_lookup(&km, &ctrl_k) == INPUT_COMMAND_NEW_TAB);

    write_input_config(path, "style = legacy\nctrl+j = new_tab\n");
    input_config_load(&km);
    assert(input_keymap_lookup_scoped(&km, &ctrl_f, scope) == find);
    assert(input_keymap_lookup(&km, &ctrl_f) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup(&km, &ctrl_j) == INPUT_COMMAND_NEW_TAB);
    assert(input_keymap_lookup(&km, &ctrl_k) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup(&km, &ctrl_c) == INPUT_COMMAND_NONE);

    /* A second load of the same file gives the same keymap. */
    size_t n = km.n;
    input_config_load(&km);
    assert(km.n == n);
    assert(input_keymap_lookup_scoped(&km, &ctrl_f, scope) == find);

    input_keymap_free(&km);
    input_command_registry_free(&own);
    input_command_scope_release(scope);
    unlink(path);
    unsetenv("WSAP_INPUT_CONFIG");
}

/* A widget owner drops the bindings of its scope, and the other scopes keep
 * theirs. A clear spec and a clear line in input.conf drop the app scope
 * only. */
static void clear_scope_test(void)
{
    struct input_keymap km;
    input_keymap_init(&km);
    assert(input_keymap_apply_spec(&km, "ctrl+w = close_tab"));
    assert(input_keymap_apply_spec_scoped(&km, 6, "ctrl+e = settings"));
    assert(input_keymap_apply_spec_scoped(&km, 7, "ctrl+e = command_palette"));
    struct input_key_event ctrl_w = { .codepoint = 'w', .mods = INPUT_MOD_CTRL };
    struct input_key_event ctrl_e = { .codepoint = 'e', .mods = INPUT_MOD_CTRL };

    input_keymap_clear_scope(&km, 6);
    assert(input_keymap_lookup_scoped(&km, &ctrl_e, 6) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup_scoped(&km, &ctrl_e, 7) == INPUT_COMMAND_COMMAND_PALETTE);
    assert(input_keymap_lookup_scoped(&km, &ctrl_w, 6) == INPUT_COMMAND_CLOSE_TAB);

    /* A clear spec in a widget scope drops that scope only. */
    assert(input_keymap_apply_spec_scoped(&km, 7, "clear"));
    assert(input_keymap_lookup_scoped(&km, &ctrl_e, 7) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup(&km, &ctrl_w) == INPUT_COMMAND_CLOSE_TAB);

    /* A clear spec in the app scope keeps the widget scopes. */
    assert(input_keymap_apply_spec_scoped(&km, 7, "ctrl+e = command_palette"));
    assert(input_keymap_apply_spec(&km, "clear"));
    assert(input_keymap_lookup(&km, &ctrl_w) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup_scoped(&km, &ctrl_e, 7) == INPUT_COMMAND_COMMAND_PALETTE);

    /* A clear line in input.conf drops the defaults and keeps scope 7. */
    char path[] = "/tmp/wsap-input-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    write_input_config(path, "clear\nctrl+k = new_tab\n");
    assert(setenv("WSAP_INPUT_CONFIG", path, 1) == 0);
    input_config_init(NULL);
    input_config_load(&km);
    struct input_key_event ctrl_k = { .codepoint = 'k', .mods = INPUT_MOD_CTRL };
    struct input_key_event ctrl_shift_c = {
        .codepoint = 'c', .mods = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
    };
    assert(input_keymap_lookup(&km, &ctrl_shift_c) == INPUT_COMMAND_NONE);
    assert(input_keymap_lookup(&km, &ctrl_k) == INPUT_COMMAND_NEW_TAB);
    assert(input_keymap_lookup_scoped(&km, &ctrl_e, 7) == INPUT_COMMAND_COMMAND_PALETTE);

    input_keymap_free(&km);
    unlink(path);
    unsetenv("WSAP_INPUT_CONFIG");
}

int main(void)
{
    command_registry_test();
    default_chord_table_test();
    scoped_binding_wins_test();
    widget_command_binding_test();
    scope_new_test();
    scope_owner_test();
    widget_scope_binds_app_command_test();
    keymap_match_test();
    zero_modifier_binding_test();
    wide_codepoint_binding_test();
    keymap_grows_test();
    config_style_order_test();
    config_reload_style_test();
    config_reload_keeps_widget_scope_test();
    clear_scope_test();
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

    assert(SC(&lin, .codepoint = 'c', .mods = ctrl_shift) == INPUT_COMMAND_COPY);
    assert(SC(&mac, .codepoint = 'c', .mods = INPUT_MOD_SUPER) == INPUT_COMMAND_COPY);
    assert(SC(&mac, .codepoint = 'c', .mods = ctrl_shift) == INPUT_COMMAND_COPY);

    assert(SC(&lin, .codepoint = 'c', .mods = INPUT_MOD_CTRL) == INPUT_COMMAND_NONE);
    assert(SC(&lin, .codepoint = 'c', .mods = INPUT_MOD_SUPER) == INPUT_COMMAND_NONE);

    assert(SC(&lin, .codepoint = 'v', .mods = ctrl_shift | INPUT_MOD_CAPS) ==
           INPUT_COMMAND_PASTE);
    assert(SC(&lin, .key = INPUT_KEY_INSERT, .mods = INPUT_MOD_SHIFT) ==
           INPUT_COMMAND_PASTE);
    assert(SC(&lin, .key = INPUT_KEY_PAGE_UP, .mods = INPUT_MOD_SHIFT) ==
           INPUT_COMMAND_SCROLL_UP);
    assert(SC(&lin, .key = INPUT_KEY_PAGE_DOWN, .mods = INPUT_MOD_SHIFT) ==
           INPUT_COMMAND_SCROLL_DOWN);

#ifdef __APPLE__
    assert(SC(&mac, .codepoint = '=', .mods = INPUT_MOD_SUPER | INPUT_MOD_SHIFT) ==
           INPUT_COMMAND_FONT_LARGER);
#else
    assert(SC(&lin, .codepoint = '=', .mods = INPUT_MOD_CTRL) ==
           INPUT_COMMAND_FONT_LARGER);
#endif
    assert(SC(&lin, .keypad = INPUT_KEY_KP_ADD, .mods = ctrl_shift) ==
           INPUT_COMMAND_FONT_LARGER);
#ifdef __APPLE__
    assert(SC(&mac, .codepoint = '-', .mods = INPUT_MOD_SUPER | INPUT_MOD_SHIFT) ==
           INPUT_COMMAND_FONT_SMALLER);
#else
    assert(SC(&lin, .codepoint = '-', .mods = INPUT_MOD_CTRL) ==
           INPUT_COMMAND_FONT_SMALLER);
    assert(SC(&lin, .codepoint = '=', .shifted = '+', .base = '=',
              .mods = ctrl_shift) == INPUT_COMMAND_NONE);
#endif
#ifdef __APPLE__
    assert(SC(&mac, .codepoint = '4', .shifted = '+', .base = '4',
              .mods = INPUT_MOD_SUPER) == INPUT_COMMAND_TAB_4);
#else
    assert(SC(&lin, .codepoint = '4', .shifted = '+', .base = '4',
              .mods = ctrl_shift) == INPUT_COMMAND_TAB_4);
#endif

#ifdef __APPLE__
    assert(SC(&mac, .key = INPUT_KEY_BACKSPACE, .mods = INPUT_MOD_SUPER) ==
           INPUT_COMMAND_KILL_LINE);
    assert(SC(&mac, .codepoint = 'p',
              .mods = INPUT_MOD_SUPER | INPUT_MOD_SHIFT) ==
           INPUT_COMMAND_COMMAND_PALETTE);
#else
    assert(SC(&lin, .codepoint = 'p', .mods = ctrl_shift) ==
           INPUT_COMMAND_COMMAND_PALETTE);
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
        assert(input_keymap_lookup(&modern, &ctrl_c) == INPUT_COMMAND_COPY);
        assert(input_keymap_lookup(&modern, &ctrl_v) == INPUT_COMMAND_PASTE);
        assert(input_keymap_lookup(&legacy, &ctrl_c) == INPUT_COMMAND_NONE);
        assert(input_keymap_lookup(&legacy, &ctrl_v) == INPUT_COMMAND_NONE);
        assert(input_keymap_lookup(&modern, &ctrl_shift_c) == INPUT_COMMAND_COPY);
        assert(input_keymap_lookup(&legacy, &ctrl_shift_c) == INPUT_COMMAND_COPY);
        assert(input_keymap_lookup(&modern, &ctrl_t) == INPUT_COMMAND_NONE);
        assert(input_keymap_lookup(&legacy, &ctrl_t) == INPUT_COMMAND_NONE);
        assert(input_keymap_lookup(&modern, &ctrl_shift_t) == INPUT_COMMAND_NEW_TAB);
        assert(input_keymap_lookup(&legacy, &ctrl_shift_t) == INPUT_COMMAND_NEW_TAB);
        /* A zero modifier set binds nothing. The bare letters must reach the
         * shell in both styles. */
        struct input_key_event plain_c = { .codepoint = 'c' };
        struct input_key_event plain_v = { .codepoint = 'v' };
        assert(input_keymap_lookup(&modern, &plain_c) == INPUT_COMMAND_NONE);
        assert(input_keymap_lookup(&legacy, &plain_c) == INPUT_COMMAND_NONE);
        assert(input_keymap_lookup(&legacy, &plain_v) == INPUT_COMMAND_NONE);
        input_keymap_free(&modern);
        input_keymap_free(&legacy);
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
        assert(input_keymap_format_action_chord(&km, INPUT_COMMAND_COPY, sc, sizeof(sc)));
        assert(!strcmp(sc, "Ctrl+C") || !strcmp(sc, "Cmd+C"));

        assert(input_keymap_format_action_chord(&km, INPUT_COMMAND_PASTE, sc, sizeof(sc)));
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
        assert(input_queue_push_drop_files(&queue, uris, sizeof(uris) - 1,
                                           (struct vector2i){ .x = 3, .y = 4 })
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
