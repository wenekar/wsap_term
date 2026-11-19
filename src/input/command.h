#ifndef INPUT_COMMAND_H
#define INPUT_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A command ID. Zero runs no command.
 * A key binding, a menu item, and an action event hold this value.
 * The IDs of the app commands are stable, so a saved binding stays valid.
 * A widget command gets its ID from `input_command_add()`.
 */
typedef uint16_t input_command_id;

/*
 * The stable IDs of the built-in app commands.
 * The name of a command in the registry is the config word, and the label is
 * the menu text.
 */
enum input_command_app_id {
    INPUT_COMMAND_NONE = 0,

    INPUT_COMMAND_FONT_LARGER,
    INPUT_COMMAND_FONT_SMALLER,
    INPUT_COMMAND_COPY,
    INPUT_COMMAND_PASTE,
    INPUT_COMMAND_SELECT_ALL,
    INPUT_COMMAND_SCROLL_UP,
    INPUT_COMMAND_SCROLL_DOWN,
    INPUT_COMMAND_NEW_TAB,
    INPUT_COMMAND_CLOSE_TAB,
    INPUT_COMMAND_NEXT_TAB,
    INPUT_COMMAND_PREV_TAB,
    INPUT_COMMAND_LAST_TAB,

    INPUT_COMMAND_TAB_1,
    INPUT_COMMAND_TAB_2,
    INPUT_COMMAND_TAB_3,
    INPUT_COMMAND_TAB_4,
    INPUT_COMMAND_TAB_5,
    INPUT_COMMAND_TAB_6,
    INPUT_COMMAND_TAB_7,
    INPUT_COMMAND_TAB_8,
    INPUT_COMMAND_KILL_LINE,
    INPUT_COMMAND_UNDO,
    INPUT_COMMAND_REDO,
    INPUT_COMMAND_CUT,
    INPUT_COMMAND_COMMAND_PALETTE,
    INPUT_COMMAND_DEBUG_OVERLAY,
    INPUT_COMMAND_SETTINGS,
    INPUT_COMMAND_FOCUS_TABS,
    INPUT_COMMAND_APP_COUNT,
};

/* The first ID that `input_command_add()` gives to a new command.
 * It is above every app ID, so a widget command never takes an app ID. */
#define INPUT_COMMAND_DYNAMIC_ID 256

/*
 * A command scope groups the commands of one owner.
 * Scope zero holds the app commands.
 * A widget adds its commands in a scope above zero.
 * A command in a widget scope wins over an app command on the same chord.
 *
 * To add a command in a widget scope:
 * 1. Call `input_command_add()` with the scope, the name, and the label, and
 *    keep the ID that it returns.
 * 2. Bind the ID with `input_keymap_apply_spec_scoped()` when the command has
 *    a chord. A menu item needs no chord.
 * 3. Put the ID in the action of the widget, and read the label of a menu
 *    item with `input_command_label()`.
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
    input_command_id id;
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
 * Adds a command to scope `scope` of registry `reg` with `name` and `label`.
 * The registry gives the command the first free ID at or above
 * `INPUT_COMMAND_DYNAMIC_ID`.
 * A widget calls this for a command of its own scope.
 * The name and the label must stay valid while the registry lives.
 * Returns the ID, or `INPUT_COMMAND_NONE` on a bad scope or a memory
 * shortage.
 */
input_command_id input_command_add(struct input_command_registry *reg,
                                  uint8_t scope, const char *name,
                                  const char *label);

/*
 * Finds command `id` for scope `scope`.
 * The function reads `scope` first, then the app scope.
 * Returns NULL when neither scope holds the ID.
 */
const struct input_command *input_command_find(const struct input_command_registry *reg,
                                               uint8_t scope,
                                               input_command_id id);

/*
 * Finds the command that `name` names in any scope of `reg`.
 * A widget scope wins over the app scope.
 * Returns NULL when no command has that name.
 */
const struct input_command *input_command_find_name(const struct input_command_registry *reg,
                                                    const char *name);

/*
 * Returns the label of command `id` for scope `scope`, or an empty string
 * when no scope holds the ID.
 * A menu or a palette item uses this text.
 */
const char *input_command_label(const struct input_command_registry *reg,
                               uint8_t scope, input_command_id id);

/* Returns the built-in command registry. The app and the tests read it. */
const struct input_command_registry *input_command_builtin(void);

#endif
