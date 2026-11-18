#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "input/command.h"

/* The app commands. The name is the config word. The label is the menu text.
 * The order does not matter. */
static const struct input_command app_commands[] = {
    {INPUT_SHORTCUT_FONT_LARGER,    "font_larger",     "Zoom In",            0},
    {INPUT_SHORTCUT_FONT_SMALLER,   "font_smaller",    "Zoom Out",           0},
    {INPUT_SHORTCUT_COPY,           "copy",            "Copy",               0},
    {INPUT_SHORTCUT_PASTE,          "paste",           "Paste",              0},
    {INPUT_SHORTCUT_SELECT_ALL,     "select_all",      "Select All",         0},
    {INPUT_SHORTCUT_SCROLL_UP,      "scroll_up",       "Scroll Up",          0},
    {INPUT_SHORTCUT_SCROLL_DOWN,    "scroll_down",     "Scroll Down",        0},
    {INPUT_SHORTCUT_NEW_TAB,        "new_tab",         "New Tab",            0},
    {INPUT_SHORTCUT_CLOSE_TAB,      "close_tab",       "Close Tab",          0},
    {INPUT_SHORTCUT_NEXT_TAB,       "next_tab",        "Next Tab",           0},
    {INPUT_SHORTCUT_PREV_TAB,       "prev_tab",        "Previous Tab",       0},
    {INPUT_SHORTCUT_LAST_TAB,       "last_tab",        "Last Tab",           0},
    {INPUT_SHORTCUT_TAB_1,          "tab_1",           "Tab 1",              0},
    {INPUT_SHORTCUT_TAB_2,          "tab_2",           "Tab 2",              0},
    {INPUT_SHORTCUT_TAB_3,          "tab_3",           "Tab 3",              0},
    {INPUT_SHORTCUT_TAB_4,          "tab_4",           "Tab 4",              0},
    {INPUT_SHORTCUT_TAB_5,          "tab_5",           "Tab 5",              0},
    {INPUT_SHORTCUT_TAB_6,          "tab_6",           "Tab 6",              0},
    {INPUT_SHORTCUT_TAB_7,          "tab_7",           "Tab 7",              0},
    {INPUT_SHORTCUT_TAB_8,          "tab_8",           "Tab 8",              0},
    {INPUT_SHORTCUT_KILL_LINE,      "kill_line",       "Kill Line",          0},
    {INPUT_SHORTCUT_COMMAND_PALETTE, "command_palette", "Command Palette",   0},
    {INPUT_SHORTCUT_DEBUG_OVERLAY,  "debug_overlay",   "Toggle Debug Overlay", 0},
    {INPUT_SHORTCUT_SETTINGS,       "settings",        "Settings",           0},
    {INPUT_SHORTCUT_FOCUS_TABS,     "focus_tabs",      "Focus Tabs",         0},
};

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
                                           enum input_shortcut id)
{
    for (size_t i = 0; i < reg->n; i++)
        if (reg->cmds[i].scope == scope && reg->cmds[i].id == id)
            return &reg->cmds[i];
    return NULL;
}

bool input_command_add(struct input_command_registry *reg, uint8_t scope,
                       enum input_shortcut id, const char *name,
                       const char *label)
{
    if (!reg || !name || scope >= INPUT_COMMAND_SCOPE_COUNT ||
        id == INPUT_SHORTCUT_NONE)
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

const struct input_command *input_command_find(const struct input_command_registry *reg,
                                               uint8_t scope,
                                               enum input_shortcut id)
{
    if (!reg || id == INPUT_SHORTCUT_NONE)
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

const struct input_command_registry *input_command_builtin(void)
{
    static struct input_command_registry registry;
    static bool ready;
    if (!ready) {
        input_command_registry_init(&registry);
        for (size_t i = 0; i < sizeof(app_commands) / sizeof(*app_commands); i++) {
            const struct input_command *c = &app_commands[i];
            input_command_add(&registry, INPUT_COMMAND_SCOPE_APP, c->id,
                              c->name, c->label);
        }
        ready = true;
    }
    return &registry;
}
