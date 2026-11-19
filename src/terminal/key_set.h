#ifndef TERMINAL_KEY_SET_H
#define TERMINAL_KEY_SET_H

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"

/* A set of key IDs from input_key_id(). All zero is the empty set. */
struct terminal_key_set {
    uint64_t bits[(INPUT_SHORTCUT_KEY_COUNT + 63) / 64];
};

/* Returns true when set `s` holds key ID `id`. */
static inline bool terminal_key_set_has(const struct terminal_key_set *s,
                                        unsigned id)
{
    return s && id < INPUT_SHORTCUT_KEY_COUNT &&
           ((s->bits[id / 64] >> (id % 64)) & 1);
}

/* Adds key ID `id` to set `s` when `in` is true, and removes it otherwise.
 * An ID out of range changes nothing. */
static inline void terminal_key_set_put(struct terminal_key_set *s,
                                        unsigned id, bool in)
{
    if (!s || id >= INPUT_SHORTCUT_KEY_COUNT)
        return;
    uint64_t bit = UINT64_C(1) << (id % 64);
    if (in)
        s->bits[id / 64] |= bit;
    else
        s->bits[id / 64] &= ~bit;
}

/* The held keys of the terminals and where each press went. A key in
 * `declined` went to the keymap. A key in `shell` went to the encoder. The
 * repeats and the release of a held key follow its press. All zero means
 * that no key is held. */
struct terminal_held_keys {
    struct terminal_key_set declined;
    struct terminal_key_set shell;
};

#endif
