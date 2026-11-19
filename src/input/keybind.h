#ifndef INPUT_KEYBIND_H
#define INPUT_KEYBIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/input.h"

/* The capacity of a new keymap. The map grows past it. */
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
    /* The command scope of the binding. See `enum input_command_scope`. */
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

/* Clears keymap `km` and releases its storage. The caller owns `km`. */
void input_keymap_init(struct input_keymap *km);

/* Releases the storage of keymap `km`. A keymap that runs out of scope
 * without this call keeps its storage until the process exits. */
void input_keymap_free(struct input_keymap *km);

/* Clears all key bindings and lookup cache in keymap `km`. */
void input_keymap_clear(struct input_keymap *km);

/*
 * Replaces the bindings of scope `scope` in keymap `dst` with the bindings of
 * that scope in keymap `src`. The other scopes of `dst` stay.
 * Returns false on a bad scope or a memory shortage.
 */
bool input_keymap_copy_scope(struct input_keymap *dst,
                             const struct input_keymap *src, uint8_t scope);

/*
 * Drops the bindings of scope `scope` from keymap `km`.
 * The bindings of other scopes stay.
 * A widget owner calls this for its scope when it is destroyed.
 */
void input_keymap_clear_scope(struct input_keymap *km, uint8_t scope);

/* Populates keymap `km` with default bindings for policy `p` and `style`. */
void input_keymap_defaults(struct input_keymap *km,
                           const struct input_shortcut_policy *p,
                           enum input_keybind_style style);

/*
 * Adds, replaces, or removes the binding that `spec` names in keymap `km`.
 * A removed binding has the action `none`. The map grows as needed.
 * The spec `clear` drops the bindings of the app scope.
 * Returns true on success, or false on syntax error or memory shortage.
 */
bool input_keymap_apply_spec(struct input_keymap *km, const char *spec);

/*
 * Adds, replaces, or removes the binding that `spec` names in scope `scope`
 * of keymap `km`. A removed binding has the action `none`.
 * The spec `clear` drops the bindings of `scope` only.
 * Returns true on success, or false on syntax error or memory shortage.
 */
bool input_keymap_apply_spec_scoped(struct input_keymap *km, uint8_t scope,
                                    const char *spec);

/*
 * Adds, replaces, or removes the binding that `spec` names in scope `scope`
 * of keymap `km`. The action name resolves in registry `reg`. When `reg` has
 * no command with that name, it resolves in the built-in app commands.
 * The spec `clear` drops the bindings of `scope` only.
 * A widget passes its own registry and scope here.
 * Returns true on success, or false on syntax error or memory shortage.
 */
bool input_keymap_apply_spec_registry(struct input_keymap *km,
                                      const struct input_command_registry *reg,
                                      uint8_t scope, const char *spec);

/*
 * Looks up the command that key event `ev` runs in keymap `km`.
 * A binding may have no modifiers.
 * Returns the matching command, or INPUT_COMMAND_NONE.
 */
input_command_id input_keymap_lookup(const struct input_keymap *km,
                                        const struct input_key_event *ev);

/*
 * Looks up the command that key event `ev` runs for scope `scope` in keymap
 * `km`. The function reads `scope` first, then the app scope.
 * Returns the matching command, or INPUT_COMMAND_NONE.
 */
input_command_id input_keymap_lookup_scoped(const struct input_keymap *km,
                                               const struct input_key_event *ev,
                                               uint8_t scope);

/*
 * Formats key combination into human-readable text string in `out`.
 * Returns true on success, or false if buffer `sz` is insufficient.
 */
bool input_format_chord(unsigned mods, uint32_t codepoint, uint16_t key,
                        char *out, size_t sz);

/*
 * Formats key chord bound to shortcut `action` in keymap `km` into `out`.
 * Returns true on success, or false if unbound or buffer `sz` is too small.
 */
bool input_keymap_format_action_chord(const struct input_keymap *km,
                                input_command_id action, char *out,
                                size_t sz);

/*
 * Formats key chord bound to shortcut `action` for scope `scope` in keymap
 * `km` into `out`. A widget passes its own scope here.
 * Returns true on success, or false if unbound or buffer `sz` is too small.
 */
bool input_keymap_format_action_chord_scoped(const struct input_keymap *km,
                                input_command_id action, uint8_t scope,
                                char *out, size_t sz);

/*
 * Parses keybind style name string `s` into style enum `out`.
 * Returns true on success, or false if name is unknown.
 */
bool input_parse_keybind_style(const char *s, enum input_keybind_style *out);

/*
 * Parses key chord specification string `s` into modifier and key values.
 * Returns true on success, or false on syntax error.
 */
bool input_parse_chord(const char *s, unsigned *mods, uint32_t *codepoint,
                       uint16_t *key);

/*
 * Parses shortcut action name string `s` into shortcut enum `out`.
 * Returns true on success, or false if name is unknown.
 */
bool input_parse_action(const char *s, input_command_id *out);

/*
 * Resets the app scope of keymap `km` to the default bindings for `style`.
 * The bindings of widget scopes stay.
 */
void input_keymap_reset(struct input_keymap *km, enum input_keybind_style style);

struct app_identity;

/* Reads the input configuration of the app identity `identity`. */
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
