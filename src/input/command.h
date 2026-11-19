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
    INPUT_COMMAND_COMMAND_PALETTE,
    INPUT_COMMAND_DEBUG_OVERLAY,
    INPUT_COMMAND_SETTINGS,
    INPUT_COMMAND_FOCUS_TABS,
    /* New commands go after this line, so no existing ID moves. */
    INPUT_COMMAND_UNDO,
    INPUT_COMMAND_REDO,
    INPUT_COMMAND_CUT,
    INPUT_COMMAND_APP_COUNT,
};

/* A saved binding stores the number, so an ID must not move. */
_Static_assert(INPUT_COMMAND_FONT_LARGER == 1, "font_larger ID moved");
_Static_assert(INPUT_COMMAND_FONT_SMALLER == 2, "font_smaller ID moved");
_Static_assert(INPUT_COMMAND_COPY == 3, "copy ID moved");
_Static_assert(INPUT_COMMAND_PASTE == 4, "paste ID moved");
_Static_assert(INPUT_COMMAND_SELECT_ALL == 5, "select_all ID moved");
_Static_assert(INPUT_COMMAND_SCROLL_UP == 6, "scroll_up ID moved");
_Static_assert(INPUT_COMMAND_SCROLL_DOWN == 7, "scroll_down ID moved");
_Static_assert(INPUT_COMMAND_NEW_TAB == 8, "new_tab ID moved");
_Static_assert(INPUT_COMMAND_CLOSE_TAB == 9, "close_tab ID moved");
_Static_assert(INPUT_COMMAND_NEXT_TAB == 10, "next_tab ID moved");
_Static_assert(INPUT_COMMAND_PREV_TAB == 11, "prev_tab ID moved");
_Static_assert(INPUT_COMMAND_LAST_TAB == 12, "last_tab ID moved");
_Static_assert(INPUT_COMMAND_TAB_1 == 13, "tab_1 ID moved");
_Static_assert(INPUT_COMMAND_TAB_2 == 14, "tab_2 ID moved");
_Static_assert(INPUT_COMMAND_TAB_3 == 15, "tab_3 ID moved");
_Static_assert(INPUT_COMMAND_TAB_4 == 16, "tab_4 ID moved");
_Static_assert(INPUT_COMMAND_TAB_5 == 17, "tab_5 ID moved");
_Static_assert(INPUT_COMMAND_TAB_6 == 18, "tab_6 ID moved");
_Static_assert(INPUT_COMMAND_TAB_7 == 19, "tab_7 ID moved");
_Static_assert(INPUT_COMMAND_TAB_8 == 20, "tab_8 ID moved");
_Static_assert(INPUT_COMMAND_KILL_LINE == 21, "kill_line ID moved");
_Static_assert(INPUT_COMMAND_COMMAND_PALETTE == 22, "command_palette ID moved");
_Static_assert(INPUT_COMMAND_DEBUG_OVERLAY == 23, "debug_overlay ID moved");
_Static_assert(INPUT_COMMAND_SETTINGS == 24, "settings ID moved");
_Static_assert(INPUT_COMMAND_FOCUS_TABS == 25, "focus_tabs ID moved");
_Static_assert(INPUT_COMMAND_UNDO == 26, "undo ID moved");
_Static_assert(INPUT_COMMAND_REDO == 27, "redo ID moved");
_Static_assert(INPUT_COMMAND_CUT == 28, "cut ID moved");
_Static_assert(INPUT_COMMAND_APP_COUNT == 29, "app ID count moved");

/* The first ID that `input_command_add()` gives to a new command.
 * Each scope gets a band of `INPUT_COMMAND_SCOPE_ID_SPAN` IDs from this
 * value, so two registries in different scopes never give the same ID.
 * It is above every app ID, so a widget command never takes an app ID. */
#define INPUT_COMMAND_DYNAMIC_ID 256
#define INPUT_COMMAND_SCOPE_ID_SPAN 4096

/*
 * A command scope groups the commands of one owner.
 * Scope zero holds the app commands.
 * A widget adds its commands in a scope above zero.
 * A command in a widget scope wins over an app command on the same chord.
 *
 * Each widget scope has one registry and one owner.
 * Two registries in the same scope can give the same ID, so do not share a
 * scope. Get a free scope from `input_command_scope_new()`.
 *
 * To add a command in a widget scope:
 * 1. Call `input_command_scope_new()`, and keep the scope that it returns.
 * 2. Call `input_command_add()` with the scope, the name, and the label, and
 *    keep the ID that it returns.
 * 3. Set the scope in the `command_scope` field of the widget.
 * 4. When the command has a chord, bind it in each window with
 *    `window_bind_scope()` from `ui/yasl/client.h`. Pass the registry of the
 *    widget. For a keymap of your own, use
 *    `input_keymap_apply_spec_registry()`. A menu item needs no chord. An
 *    app command, such as `close_tab`, can bind in the widget scope too.
 * 5. Put the ID in the action of the widget, and read the label of a menu
 *    item with `input_command_label()`.
 * When the owner is destroyed, it calls `window_clear_scope()` for each
 * window that it bound, or `input_keymap_clear_scope()` for a keymap of its
 * own. Then it frees its registry and calls `input_command_scope_release()`.
 * The UI core does not do this, because it owns no keymap.
 * A registry in one scope never gives an ID that a registry in another scope
 * gives.
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
 * Gives a widget scope that no other owner holds.
 * The caller owns the scope until it calls `input_command_scope_release()`.
 * Call it from one thread only.
 * Returns a scope from 1 to `INPUT_COMMAND_SCOPE_COUNT - 1`, or
 * `INPUT_COMMAND_SCOPE_APP` when no scope is free.
 */
uint8_t input_command_scope_new(void);

/*
 * Gives back widget scope `scope`, so a later call to
 * `input_command_scope_new()` can give it again.
 * Call it only for a scope that you own, after you free its registry.
 * The function ignores the app scope and a scope that is out of range.
 */
void input_command_scope_release(uint8_t scope);

/*
 * Adds a command to scope `scope` of registry `reg` with `name` and `label`.
 * The scope must be from 1 to `INPUT_COMMAND_SCOPE_COUNT - 1`.
 * The registry gives the command the first free ID in the band of `scope`,
 * at or above `INPUT_COMMAND_DYNAMIC_ID`.
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
 * Finds the command that `name` names in scope `scope` of `reg`.
 * The function reads `scope` first, then the app scope.
 * Returns NULL when no command has that name.
 */
const struct input_command *input_command_find_name_scoped(const struct input_command_registry *reg,
                                                           uint8_t scope,
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
