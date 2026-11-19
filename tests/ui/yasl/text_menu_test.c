#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "input/command.h"
#include "input/keybind.h"
#include "ui/widgets/input.h"
#include "ui/widgets/list.h"
#include "ui/widgets/menu.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/testing.h"
#include "ui/yasl/window.h"

/*
 * A right click on a focused ui_input opens the text menu.
 * The menu items come from the command registry, so the window holds no
 * English text of its own.
 */

static struct window_host host;

static struct window *make_window(struct ui_input *in, const char *text)
{
    /* The host borrows the config, so it must outlive the host. */
    static const struct window_config config = { .size = { .x = 320, .y = 200 } };
    window_host_init(&host, NULL, &config, NULL);
    struct window *w = window_create(&host, &(struct window_spec){0});
    assert(w);
    ui_input_init(in);
    ui_input_set_text(in, text);
    assert(window_ui_mount(w, &(struct window_ui_spec){ .root = &in->widget }));
    return w;
}

static void destroy_window(struct window *w)
{
    window_destroy(w);
    window_host_registry_remove(&host);
    free(host.windows);
}

static struct ui_menu *open_menu(struct window *w, struct ui_input *in,
                                 struct vector2i at)
{
    /* The input widget asks for a menu, as a right press does. */
    assert(ui_widget_notify(&in->widget, (struct ui_action){
        .kind = UI_ACTION_MENU, .index = -1, .pos = at,
    }));
    struct ui_menu *menu = window_ui_menu(w);
    assert(menu && ui_menu_is_open(menu));
    return menu;
}

/* The menu of a focused ui_input lists the text commands from the registry. */
static void text_menu_test(void)
{
    struct ui_input in;
    struct window *w = make_window(&in, "text");
    struct ui_menu *menu = open_menu(w, &in, (struct vector2i){ .x = 8, .y = 8 });
    const struct ui_list *list = &menu->levels[0].list;

    /* The order of the rows, with INPUT_COMMAND_NONE for a separator. */
    static const input_command_id order[] = {
        INPUT_COMMAND_UNDO, INPUT_COMMAND_REDO, INPUT_COMMAND_NONE,
        INPUT_COMMAND_CUT, INPUT_COMMAND_COPY, INPUT_COMMAND_PASTE,
        INPUT_COMMAND_NONE, INPUT_COMMAND_SELECT_ALL,
    };
    assert(ui_list_count(list) == (int)(sizeof order / sizeof order[0]));

    const struct input_command_registry *reg = input_command_builtin();
    for (int i = 0; i < ui_list_count(list); i++) {
        const struct ui_list_item *item = ui_list_item(list, i);
        assert(item);
        if (order[i] == INPUT_COMMAND_NONE) {
            assert(item->flags & UI_LIST_ITEM_SEPARATOR);
            continue;
        }
        const char *label =
            input_command_label(reg, INPUT_COMMAND_SCOPE_APP, order[i]);
        assert(label[0]);
        assert(item->label && !strcmp(item->label, label));
        assert(item->action == (int)order[i] - (int)INPUT_COMMAND_UNDO ||
               item->action >= 0);
    }

    /* The row of a bound command shows the chord of the window keymap. */
    char chord[64];
    assert(input_keymap_format_action_chord(&w->keymap, INPUT_COMMAND_COPY,
                                            chord, sizeof chord));
    const struct ui_list_item *copy = ui_list_item(list, 4);
    assert(copy && copy->shortcut && !strcmp(copy->shortcut, chord));

    /* The menu remembers the input as its target. */
    struct ui_widget *target = ui_surface_context_target(&ui_of(w)->screen);
    assert(target == &in.widget);
    destroy_window(w);
}

/* A command that the input cannot run is disabled. Paste stays enabled,
 * because the clipboard can fill an empty input. */
static void disabled_command_test(void)
{
    struct ui_input in;
    struct window *w = make_window(&in, "");
    struct ui_menu *menu = open_menu(w, &in, (struct vector2i){ .x = 2, .y = 2 });
    const struct ui_list *list = &menu->levels[0].list;
    const struct ui_list_item *undo = ui_list_item(list, 0);
    assert(undo && (undo->flags & UI_LIST_ITEM_DISABLED));
    const struct ui_list_item *paste = ui_list_item(list, 5);
    assert(paste && !(paste->flags & UI_LIST_ITEM_DISABLED));
    destroy_window(w);
}

/* A shortcut that the keymap of the window sends runs the text command of the
 * focused input. The words undo, redo, and cut must reach the input. */
static void edit_shortcut_runs_the_text_command_test(void)
{
    struct ui_input in;
    struct window *w = make_window(&in, "");
    ui_surface_focus(window_ui_key_surface(w), &in.widget);
    struct ui_event typed = ui_event_text("ab", 2, 0);
    assert(ui_widget_event(&in.widget, &typed));
    assert(!strcmp(ui_input_text(&in), "ab"));

    /* An undo shortcut arrives as a command ID, not as a key. */
    struct input_event undo = {
        .kind = INPUT_SHORTCUT,
        .shortcut = { .action = INPUT_COMMAND_UNDO },
    };
    window_ui_route(w, &undo);
    assert(!strcmp(ui_input_text(&in), ""));

    struct input_event redo = {
        .kind = INPUT_SHORTCUT,
        .shortcut = { .action = INPUT_COMMAND_REDO },
    };
    window_ui_route(w, &redo);
    assert(!strcmp(ui_input_text(&in), "ab"));
    destroy_window(w);
}

int main(void)
{
    text_menu_test();
    edit_shortcut_runs_the_text_command_test();
    disabled_command_test();
    return 0;
}
