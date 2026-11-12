#ifndef INPUT_KEYBIND_H
#define INPUT_KEYBIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/input.h"

#define INPUT_MAX_KEYBINDS 128
#define INPUT_KEYMAP_HASH_CAP 256

enum input_keybind_style {
    INPUT_KEYBIND_LEGACY = 0,
    INPUT_KEYBIND_MODERN,
};

struct input_keybind {
    unsigned mods;
    uint32_t codepoint;
    uint16_t key;
    uint16_t action;
};

struct input_keymap_slot {
    uint32_t chord;
    uint16_t action;
};

struct input_keymap {
    struct input_keybind binds[INPUT_MAX_KEYBINDS];
    int n;
    struct input_keymap_slot hash[INPUT_KEYMAP_HASH_CAP];
    bool hash_valid;
};

/* Clears all key bindings and lookup cache in keymap `km`. */
void input_keymap_clear(struct input_keymap *km);

/* Populates keymap `km` with default bindings for policy `p` and `style`. */
void input_keymap_defaults(struct input_keymap *km,
                           const struct input_shortcut_policy *p,
                           enum input_keybind_style style);

/*
 * Parses and applies binding specification string `spec` to keymap `km`.
 * Returns true on success, or false on syntax error.
 */
bool input_keymap_apply_spec(struct input_keymap *km, const char *spec);

/*
 * Looks up shortcut action matching key event `ev` in keymap `km`.
 * Returns the matching shortcut action, or INPUT_SHORTCUT_NONE.
 */
enum input_shortcut input_keymap_lookup(const struct input_keymap *km,
                                        const struct input_key_event *ev);

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

/* Resets the globally active keymap to default bindings for `style`. */
void input_keymap_reset_active(enum input_keybind_style style);

/* Initializes global input configuration and default key bindings. */
void input_config_init(void);

#endif
