#ifndef INPUT_COMMAND_H
#define INPUT_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/input.h"

/*
 * A command scope groups the commands of one owner.
 * Scope zero holds the app commands.
 * A widget adds its commands in a scope above zero.
 * A command in a widget scope wins over an app command on the same chord.
 */
enum input_command_scope {
    INPUT_COMMAND_SCOPE_APP = 0,
    /* One scope more than this value is invalid. */
    INPUT_COMMAND_SCOPE_COUNT = 16,
};

/*
 * A command has a stable ID, a name, and a label.
 * The ID never changes, so a saved binding stays valid.
 * The name is the word that a config line uses for the command.
 * The label is the text of a menu item or a palette row.
 */
struct input_command {
    enum input_shortcut id;
    const char *name;
    const char *label;
    uint8_t scope;
};

#define INPUT_COMMAND_INITIAL_CAP 8

/* A set of commands. A registry owns its entries. */
struct input_command_registry {
    struct input_command *cmds;
    size_t n;
    size_t cap;
};

/* Prepares empty registry `reg`. */
void input_command_registry_init(struct input_command_registry *reg);

/* Releases the storage of registry `reg`. The names and labels stay. */
void input_command_registry_free(struct input_command_registry *reg);

/*
 * Adds command `id` to scope `scope` of registry `reg` with `name` and `label`.
 * A command with the same ID in the same scope takes the new name and label.
 * Returns false on a bad scope or a memory shortage.
 */
bool input_command_add(struct input_command_registry *reg, uint8_t scope,
                       enum input_shortcut id, const char *name,
                       const char *label);

/*
 * Finds command `id` for scope `scope`.
 * The function reads `scope` first, then the app scope.
 * Returns NULL when neither scope holds the ID.
 */
const struct input_command *input_command_find(const struct input_command_registry *reg,
                                               uint8_t scope,
                                               enum input_shortcut id);

/*
 * Finds the command that `name` names in any scope of `reg`.
 * A widget scope wins over the app scope.
 * Returns NULL when no command has that name.
 */
const struct input_command *input_command_find_name(const struct input_command_registry *reg,
                                                    const char *name);

/* Returns the built-in command registry. The app and the tests read it. */
const struct input_command_registry *input_command_builtin(void);

#endif
