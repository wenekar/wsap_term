#ifndef INPUT_KEYBIND_H
#define INPUT_KEYBIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/input.h"

/* The initial capacity of a keymap in bindings. The map grows when full. */
#define INPUT_KEYMAP_INITIAL_CAP 8

enum input_keybind_style {
    INPUT_KEYBIND_LEGACY = 0,
    INPUT_KEYBIND_MODERN,
};

struct input_keybind {
    unsigned mods;
    uint32_t codepoint;
    uint16_t key;
    uint16_t action;
    /* A value of `enum input_command_scope`. */
    uint8_t scope;
};

struct input_keymap {
    uint64_t *hash_chord;
    uint16_t *hash_action;
    size_t hash_cap;
    struct input_keybind *binds;
    size_t n;
    size_t cap;
    bool hash_valid;
};

/* Prepares keymap `km` with no bindings. Pass a keymap that holds no storage:
 * the call does not free it. */
void input_keymap_init(struct input_keymap *km);

/* Releases the storage of keymap `km` and leaves it empty. The owner must call
 * this before the keymap goes out of scope, or the storage leaks. */
void input_keymap_free(struct input_keymap *km);

/* Removes all bindings from keymap `km` and keeps its storage. */
void input_keymap_clear(struct input_keymap *km);

/*
 * Replaces the bindings of scope `scope` in keymap `dst` with the bindings of
 * that scope in keymap `src`. The other scopes of `dst` stay. A call with
 * `dst` equal to `src` changes nothing and returns true.
 * Returns false on a bad scope or a memory shortage. On a memory shortage,
 * `dst` has lost its earlier bindings for `scope` and keeps the ones that the
 * call added before the failure.
 */
bool input_keymap_copy_scope(struct input_keymap *dst,
                             const struct input_keymap *src, uint8_t scope);

/*
 * Drops the bindings of scope `scope` from keymap `km`.
 * The bindings of other scopes stay.
 * An owner of a keymap calls this for its scope before it releases the scope.
 */
void input_keymap_clear_scope(struct input_keymap *km, uint8_t scope);

/*
 * Fills keymap `km` with the default app bindings for policy `p` and `style`.
 * The style selects the copy and paste chords on platforms other than macOS.
 * Pass a keymap that holds no storage: the call does not free existing
 * storage.
 */
void input_keymap_defaults(struct input_keymap *km,
                           const struct input_shortcut_policy *p,
                           enum input_keybind_style style);

/*
 * Applies binding spec `spec`, such as `ctrl+k = new_tab`, to the app scope of
 * keymap `km`. A spec with an action adds or replaces a binding, and a spec
 * with the action `none` removes one. A chord with no modifier binds the bare
 * key. The spec `clear` drops the bindings of the app scope. The map grows as
 * needed.
 * Returns true on success, or false on a malformed spec, an unknown action
 * name, or a memory shortage.
 */
bool input_keymap_apply_spec(struct input_keymap *km, const char *spec);

/*
 * Applies binding spec `spec` in scope `scope` of keymap `km`. The action name
 * resolves in the built-in app commands. A spec with an action adds or
 * replaces a binding, and a spec with the action `none` removes one. The spec
 * `clear` drops the bindings of `scope` only.
 * Returns true on success, or false on a malformed spec, an unknown action
 * name, or a memory shortage.
 */
bool input_keymap_apply_spec_scoped(struct input_keymap *km, uint8_t scope,
                                    const char *spec);

/*
 * Applies binding spec `spec` in scope `scope` of keymap `km`. The action name
 * resolves in `scope` of registry `reg`, then in the app scope of `reg`, and
 * finally in the built-in app commands, so a widget scope can bind an app
 * command. A spec with the action `none` removes a binding. The spec `clear`
 * drops the bindings of `scope` only. A widget passes its own registry and
 * scope here.
 * Returns true on success, or false on a malformed spec, an unknown action
 * name, or a memory shortage.
 */
bool input_keymap_apply_spec_registry(struct input_keymap *km,
                                      const struct input_command_registry *reg,
                                      uint8_t scope, const char *spec);

/*
 * Looks up the command that key event `ev` runs in the app scope of keymap
 * `km`. The lookup tries the key, the keypad key, the codepoint, the shifted
 * character, and the base character of `ev`, in that order. It ignores the
 * Caps Lock and Num Lock modifiers.
 * Returns the matching command, or `INPUT_COMMAND_NONE`.
 */
input_command_id input_keymap_lookup(const struct input_keymap *km,
                                        const struct input_key_event *ev);

/*
 * Looks up the command that key event `ev` runs for scope `scope` in keymap
 * `km`. The function reads the bindings of `scope` first, then the app scope.
 * A widget passes its own scope here. The lookup matches the event fields as
 * `input_keymap_lookup()` does.
 * Returns the matching command, or `INPUT_COMMAND_NONE`.
 */
input_command_id input_keymap_lookup_scoped(const struct input_keymap *km,
                                               const struct input_key_event *ev,
                                               uint8_t scope);

/*
 * Writes the text of the chord `mods`, `codepoint`, and `key` to `out`, for
 * example `Ctrl+Shift+C`.
 * Returns true on success, or false when the chord has no printable name or
 * `sz` cannot hold the text.
 */
bool input_format_chord(unsigned mods, uint32_t codepoint, uint16_t key,
                        char *out, size_t sz);

/*
 * Writes the chord of the first app-scope binding of command `action` in
 * keymap `km` that can be formatted to `out`.
 * Returns true on success, or false when the app scope has no formattable
 * binding for `action` or `sz` cannot hold the text.
 */
bool input_keymap_format_action_chord(const struct input_keymap *km,
                                input_command_id action, char *out,
                                size_t sz);

/*
 * Writes the chord of the first binding of command `action` in scope `scope`
 * of keymap `km` that can be formatted to `out`. The call does not read the app
 * scope, so a widget asks for its own scope here.
 * Returns true on success, or false when `scope` has no formattable binding for
 * `action` or `sz` cannot hold the text.
 */
bool input_keymap_format_action_chord_scoped(const struct input_keymap *km,
                                input_command_id action, uint8_t scope,
                                char *out, size_t sz);

/*
 * Parses keybind style name `s`, `legacy` or `modern`, into `out`. The name
 * ignores case.
 * Returns true on success, or false when the name is unknown.
 */
bool input_parse_keybind_style(const char *s, enum input_keybind_style *out);

/*
 * Parses chord spec `s`, such as `ctrl+shift+c` or `enter`, into `mods` and
 * one of `codepoint` or `key`. The other output is zero, and names ignore
 * case.
 * Returns true on success, or false on a syntax error.
 */
bool input_parse_chord(const char *s, unsigned *mods, uint32_t *codepoint,
                       uint16_t *key);

/*
 * Parses command name `s` into command ID `out`. The name resolves in the
 * built-in app commands, and the name `none` gives `INPUT_COMMAND_NONE`. The
 * name ignores case.
 * Returns true on success, or false when the name is unknown.
 */
bool input_parse_action(const char *s, input_command_id *out);

/*
 * Resets the app scope of keymap `km` to the default bindings for `style`.
 * The defaults come from `input_platform_shortcuts`. The bindings of other
 * scopes stay.
 */
void input_keymap_reset(struct input_keymap *km, enum input_keybind_style style);

struct app_identity;

/*
 * Selects the input configuration file for app identity `identity` and reads
 * its keybind style. The last valid `style` line wins. The file is the path in
 * `WSAP_INPUT_CONFIG`, or `input.conf` in the configuration directory of
 * `identity`. A NULL `identity` has no default path. The value `NONE` selects
 * no file.
 * When the selected file does not exist, the call attempts to write a
 * commented default file.
 * Call it before the first `input_config_load()`, from one thread.
 */
void input_config_init(const struct app_identity *identity);

/*
 * Loads the input configuration into keymap `km`.
 * The load replaces the bindings of the app scope. The bindings of widget
 * scopes stay.
 * A window host calls this once for all its windows: before its first window
 * and at each reload. Each window then copies the same result.
 */
void input_config_load(struct input_keymap *km);

#endif
