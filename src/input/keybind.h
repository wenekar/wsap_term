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

void input_keymap_clear(struct input_keymap *km);
void input_keymap_defaults(struct input_keymap *km,
                           const struct input_shortcut_policy *p,
                           enum input_keybind_style style);
bool input_keymap_apply_spec(struct input_keymap *km, const char *spec);
enum input_shortcut input_keymap_lookup(const struct input_keymap *km,
                                        const struct input_key_event *ev);
bool input_format_chord(unsigned mods, uint32_t codepoint, uint16_t key,
                        char *out, size_t sz);
bool input_keymap_format_action_chord(const struct input_keymap *km,
                                enum input_shortcut action, char *out,
                                size_t sz);
bool input_parse_keybind_style(const char *s, enum input_keybind_style *out);
bool input_parse_chord(const char *s, unsigned *mods, uint32_t *codepoint,
                       uint16_t *key);
bool input_parse_action(const char *s, enum input_shortcut *out);

struct input_keymap *input_keymap_active(void);
void input_keymap_reset_active(enum input_keybind_style style);

void input_config_init(void);

#endif
