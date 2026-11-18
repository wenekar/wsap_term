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

/* Populates keymap `km` with default bindings for policy `p` and `style`. */
void input_keymap_defaults(struct input_keymap *km,
                           const struct input_shortcut_policy *p,
                           enum input_keybind_style style);

/*
 * Adds, replaces, or removes the binding that `spec` names in keymap `km`.
 * A removed binding has the action `none`. The map grows as needed.
 * Returns true on success, or false on syntax error or memory shortage.
 */
bool input_keymap_apply_spec(struct input_keymap *km, const char *spec);

/*
 * Adds, replaces, or removes the binding that `spec` names in scope `scope`
 * of keymap `km`. A removed binding has the action `none`.
 * Returns true on success, or false on syntax error or memory shortage.
 */
bool input_keymap_apply_spec_scoped(struct input_keymap *km, uint8_t scope,
                                    const char *spec);

/*
 * Looks up the command that key event `ev` runs in keymap `km`.
 * A binding may have no modifiers.
 * Returns the matching command, or INPUT_SHORTCUT_NONE.
 */
enum input_shortcut input_keymap_lookup(const struct input_keymap *km,
                                        const struct input_key_event *ev);

/*
 * Looks up the command that key event `ev` runs for scope `scope` in keymap
 * `km`. The function reads `scope` first, then the app scope.
 * Returns the matching command, or INPUT_SHORTCUT_NONE.
 */
enum input_shortcut input_keymap_lookup_scoped(const struct input_keymap *km,
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
                                enum input_shortcut action, char *out,
                                size_t sz);

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
bool input_parse_action(const char *s, enum input_shortcut *out);

/* Returns a pointer to the globally active keymap. */
struct input_keymap *input_keymap_active(void);

/* Resets keymap `km` to the default bindings for `style`. */
void input_keymap_reset(struct input_keymap *km, enum input_keybind_style style);

/* Resets the globally active keymap to default bindings for `style`. */
void input_keymap_reset_active(enum input_keybind_style style);

/* Initializes global input configuration and default key bindings. */
struct app_identity;

/* Reads the input configuration of the app identity `identity`. */
void input_config_init(const struct app_identity *identity);

/*
 * Loads the input configuration into keymap `km`.
 * A window calls this once in its init, so each window owns its bindings.
 */
void input_config_load(struct input_keymap *km);

#endif
