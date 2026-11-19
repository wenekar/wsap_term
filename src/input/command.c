#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "input/command.h"

/* The app commands. The name is the config word. The label is the menu text.
 * The order does not matter. */
static const struct input_command app_commands[] = {
    {INPUT_COMMAND_FONT_LARGER,    "font_larger",     "Zoom In",            0},
    {INPUT_COMMAND_FONT_SMALLER,   "font_smaller",    "Zoom Out",           0},
    {INPUT_COMMAND_COPY,           "copy",            "Copy",               0},
    {INPUT_COMMAND_PASTE,          "paste",           "Paste",              0},
    {INPUT_COMMAND_SELECT_ALL,     "select_all",      "Select All",         0},
    {INPUT_COMMAND_SCROLL_UP,      "scroll_up",       "Scroll Up",          0},
    {INPUT_COMMAND_SCROLL_DOWN,    "scroll_down",     "Scroll Down",        0},
    {INPUT_COMMAND_NEW_TAB,        "new_tab",         "New Tab",            0},
    {INPUT_COMMAND_CLOSE_TAB,      "close_tab",       "Close Tab",          0},
    {INPUT_COMMAND_NEXT_TAB,       "next_tab",        "Next Tab",           0},
    {INPUT_COMMAND_PREV_TAB,       "prev_tab",        "Previous Tab",       0},
    {INPUT_COMMAND_LAST_TAB,       "last_tab",        "Last Tab",           0},
    {INPUT_COMMAND_TAB_1,          "tab_1",           "Tab 1",              0},
    {INPUT_COMMAND_TAB_2,          "tab_2",           "Tab 2",              0},
    {INPUT_COMMAND_TAB_3,          "tab_3",           "Tab 3",              0},
    {INPUT_COMMAND_TAB_4,          "tab_4",           "Tab 4",              0},
    {INPUT_COMMAND_TAB_5,          "tab_5",           "Tab 5",              0},
    {INPUT_COMMAND_TAB_6,          "tab_6",           "Tab 6",              0},
    {INPUT_COMMAND_TAB_7,          "tab_7",           "Tab 7",              0},
    {INPUT_COMMAND_TAB_8,          "tab_8",           "Tab 8",              0},
    {INPUT_COMMAND_KILL_LINE,      "kill_line",       "Kill Line",          0},
    {INPUT_COMMAND_UNDO,           "undo",            "Undo",               0},
    {INPUT_COMMAND_REDO,           "redo",            "Redo",               0},
    {INPUT_COMMAND_CUT,            "cut",             "Cut",                0},
    {INPUT_COMMAND_COMMAND_PALETTE, "command_palette", "Command Palette",   0},
    {INPUT_COMMAND_DEBUG_OVERLAY,  "debug_overlay",   "Toggle Debug Overlay", 0},
    {INPUT_COMMAND_SETTINGS,       "settings",        "Settings",           0},
    {INPUT_COMMAND_FOCUS_TABS,     "focus_tabs",      "Focus Tabs",         0},
};
_Static_assert(sizeof app_commands / sizeof app_commands[0] ==
                   INPUT_COMMAND_APP_COUNT - 1,
               "each app command ID needs one row in app_commands");

void input_command_registry_init(struct input_command_registry *reg)
{
    if (reg)
        *reg = (struct input_command_registry){0};
}

void input_command_registry_free(struct input_command_registry *reg)
{
    if (!reg)
        return;
    free(reg->cmds);
    *reg = (struct input_command_registry){0};
}

static struct input_command *registry_slot(struct input_command_registry *reg,
                                           uint8_t scope,
                                           input_command_id id)
{
    for (size_t i = 0; i < reg->n; i++)
        if (reg->cmds[i].scope == scope && reg->cmds[i].id == id)
            return &reg->cmds[i];
    return NULL;
}

/* Adds command `id` to scope `scope` of `reg`. The builtin registry keeps the
 * stable app IDs through this function. */
static bool registry_add_at(struct input_command_registry *reg, uint8_t scope,
                            input_command_id id, const char *name,
                            const char *label)
{
    if (!reg || !name || scope >= INPUT_COMMAND_SCOPE_COUNT ||
        id == INPUT_COMMAND_NONE)
        return false;
    struct input_command *slot = registry_slot(reg, scope, id);
    if (slot) {
        slot->name = name;
        slot->label = label;
        return true;
    }
    if (reg->n == reg->cap) {
        size_t cap = reg->cap ? reg->cap * 2 : INPUT_COMMAND_INITIAL_CAP;
        struct input_command *cmds = realloc(reg->cmds, cap * sizeof *cmds);
        if (!cmds)
            return false;
        reg->cmds = cmds;
        reg->cap = cap;
    }
    reg->cmds[reg->n++] = (struct input_command){
        .id = id, .name = name, .label = label, .scope = scope,
    };
    return true;
}

/* Returns the first ID in the band of `scope` that `reg` does not hold, or
 * `INPUT_COMMAND_NONE` when the band is full. */
static input_command_id registry_free_id(const struct input_command_registry *reg,
                                         uint8_t scope)
{
    unsigned base = INPUT_COMMAND_DYNAMIC_ID +
                    (unsigned)(scope - 1) * INPUT_COMMAND_SCOPE_ID_SPAN;
    unsigned end = base + INPUT_COMMAND_SCOPE_ID_SPAN;
    if (end > (unsigned)UINT16_MAX + 1)
        end = (unsigned)UINT16_MAX + 1;
    for (unsigned id = base; id < end; id++) {
        bool used = false;
        for (size_t i = 0; i < reg->n; i++) {
            if (reg->cmds[i].id == (input_command_id)id) {
                used = true;
                break;
            }
        }
        if (!used)
            return (input_command_id)id;
    }
    return INPUT_COMMAND_NONE;
}

/* The widget scopes that `input_command_scope_new()` gave out. Bit `n` is
 * scope `n`. Bit 0 is the app scope, which no owner gets. */
static uint32_t scopes_used = 1u;
_Static_assert(INPUT_COMMAND_SCOPE_COUNT <= 32,
               "the scope set needs one bit per scope");

uint8_t input_command_scope_new(void)
{
    for (unsigned scope = 1; scope < INPUT_COMMAND_SCOPE_COUNT; scope++) {
        if (!(scopes_used & (1u << scope))) {
            scopes_used |= 1u << scope;
            return (uint8_t)scope;
        }
    }
    return INPUT_COMMAND_SCOPE_APP;
}

void input_command_scope_release(uint8_t scope)
{
    if (scope != INPUT_COMMAND_SCOPE_APP && scope < INPUT_COMMAND_SCOPE_COUNT)
        scopes_used &= ~(1u << scope);
}

input_command_id input_command_add(struct input_command_registry *reg,
                                  uint8_t scope, const char *name,
                                  const char *label)
{
    if (!reg || !name || !name[0] || scope == INPUT_COMMAND_SCOPE_APP ||
        scope >= INPUT_COMMAND_SCOPE_COUNT)
        return INPUT_COMMAND_NONE;
    input_command_id id = registry_free_id(reg, scope);
    if (id == INPUT_COMMAND_NONE || !registry_add_at(reg, scope, id, name, label))
        return INPUT_COMMAND_NONE;
    return id;
}

const struct input_command *input_command_find(const struct input_command_registry *reg,
                                               uint8_t scope,
                                               input_command_id id)
{
    if (!reg || id == INPUT_COMMAND_NONE)
        return NULL;
    if (scope && scope < INPUT_COMMAND_SCOPE_COUNT) {
        const struct input_command *scoped =
            registry_slot((struct input_command_registry *)reg, scope, id);
        if (scoped)
            return scoped;
    }
    return registry_slot((struct input_command_registry *)reg,
                         INPUT_COMMAND_SCOPE_APP, id);
}

const struct input_command *input_command_find_name(const struct input_command_registry *reg,
                                                    const char *name)
{
    if (!reg || !name || !name[0])
        return NULL;
    const struct input_command *match = NULL;
    for (size_t i = 0; i < reg->n; i++) {
        const struct input_command *cmd = &reg->cmds[i];
        if (strcasecmp(cmd->name, name))
            continue;
        /* A widget scope wins over the app scope. */
        if (!match || (match->scope == INPUT_COMMAND_SCOPE_APP &&
                       cmd->scope != INPUT_COMMAND_SCOPE_APP))
            match = cmd;
    }
    return match;
}

const struct input_command *input_command_find_name_scoped(const struct input_command_registry *reg,
                                                           uint8_t scope,
                                                           const char *name)
{
    if (!reg || !name || !name[0] || scope >= INPUT_COMMAND_SCOPE_COUNT)
        return NULL;
    const struct input_command *app = NULL;
    for (size_t i = 0; i < reg->n; i++) {
        const struct input_command *cmd = &reg->cmds[i];
        if (strcasecmp(cmd->name, name))
            continue;
        if (cmd->scope == scope)
            return cmd;
        if (cmd->scope == INPUT_COMMAND_SCOPE_APP)
            app = cmd;
    }
    return app;
}

const char *input_command_label(const struct input_command_registry *reg,
                               uint8_t scope, input_command_id id)
{
    const struct input_command *cmd = input_command_find(reg, scope, id);
    return cmd && cmd->label ? cmd->label : "";
}

const struct input_command_registry *input_command_builtin(void)
{
    static struct input_command_registry registry;
    static bool ready;
    if (!ready) {
        input_command_registry_init(&registry);
        for (size_t i = 0; i < sizeof(app_commands) / sizeof(*app_commands); i++) {
            const struct input_command *c = &app_commands[i];
            registry_add_at(&registry, INPUT_COMMAND_SCOPE_APP, c->id,
                            c->name, c->label);
        }
        ready = true;
    }
    return &registry;
}
