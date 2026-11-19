# Build a window screen

> The examples name a host application. Use your own program in its place.


Use this guide to add a small screen to an app window.
It uses the screen API in `src/ui/` and the window binding in `src/ui/yasl/`.
The UI core does not use yasl headers.
Only app code opens windows and mounts window content.

## Create and mount the screen

This example expects an app window that is already open.
The app supplies the window frame composer when it opens the window.
The existing app composer is `app_compose_frame()` in `src/app/app.c`.
It builds the UI scene and sends it to the renderer.
Screen code does not render a frame or dispatch native input.

```c
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "ui/ui.h"
#include "ui/yasl/client.h"

enum example_action {
    EXAMPLE_APPLY = 1,
};

struct example_screen {
    struct ui_view *view;
    struct ui_input *name;
    struct ui_label *result;
};

static bool example_action(struct ui_widget *source,
                           const struct ui_action *action, void *userdata)
{
    (void)source;
    struct example_screen *screen = userdata;
    if (action->kind != UI_ACTION_ACTIVATE || action->id != EXAMPLE_APPLY)
        return false;
    if (!ui_label_set_text(screen->result, ui_input_text(screen->name)))
        fprintf(stderr, "example: cannot update the result\n");
    return true;
}

static void example_destroy(struct window *window)
{
    struct example_screen *screen = window_ui_content(window);
    ui_view_destroy(screen->view);
    free(screen);
}

bool example_mount(struct window *window)
{
    struct example_screen *screen = calloc(1, sizeof(*screen));
    if (!screen)
        return false;

    const struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN,
        .class_name = "example",
        UI_CHILDREN(
            { .kind = UI_NODE_INPUT, .key = "name",
              .bind.input = &screen->name,
              .props.input = { .accessible_name = "Name" } },
            { .kind = UI_NODE_BUTTON, .action_id = EXAMPLE_APPLY,
              .props.button = { .text = "Apply" } },
            { .kind = UI_NODE_LABEL, .key = "result", .id_name = "result",
              .bind.label = &screen->result }
        ),
        .on_action = example_action,
        .action_userdata = screen,
    };
    struct ui_view_diagnostic diagnostic;
    screen->view = ui_view_mount_ex(&root, &diagnostic);
    if (!screen->view) {
        const char *key = diagnostic.node ? diagnostic.node->key : NULL;
        fprintf(stderr, "example: %s: %s (node: %s)\n",
                ui_view_error_message(diagnostic.error),
                diagnostic.detail ? diagnostic.detail : "No details",
                key ? key : "unknown");
        free(screen);
        return false;
    }

    const struct window_ui_spec spec = {
        .name = "example",
        .root = ui_view_root(screen->view),
        .content = screen,
        .destroy = example_destroy,
    };
    if (!window_ui_mount(window, &spec)) {
        ui_view_destroy(screen->view);
        free(screen);
        return false;
    }
    return true;
}
```

The app calls `example_mount()` after `window_open()` succeeds.
If mounting fails, the app closes the window.
The app sets the window theme after mounting.
See `open_settings()` and `style_window()` in `src/app/app.c` for the complete app path.

The view copies the descriptor strings during `ui_view_mount_ex()`.
The local descriptor and its child array can then go away.
`UI_CHILDREN()` sets the child array and its count together.
Define nested descriptors that use `UI_CHILDREN()` in separate variables.
The C preprocessor cannot expand a macro inside another call to itself.
The view owns the widget storage.
The screen owns the view and the action userdata.
After a successful window mount, the window calls `example_destroy()` during teardown.
The window detaches the content root before that callback.
If mounting fails, the caller still owns the screen and destroys it.
Keep callback userdata alive until dispatch returns.
Do not destroy the view during an action callback.

The `bind` fields write typed widget handles when the mount succeeds.
The slots are unchanged if the mount fails.
The handles stay valid until the view is destroyed.
The binding field must match the descriptor kind.
Keys also identify widgets for typed lookup within a view.
They are not CSS IDs.
Use `id_name` for a CSS ID and an inspector selector.
The inspector also accepts `@key` to select a widget by its view key.
Each view key must be unique.
The typed lookup returns `NULL` if the key or widget type does not match.
Use stable handles and setters to update the mounted screen.
This preserves widget focus and edit state between updates.

`ui_view_update()` re-describes a mounted view from a new descriptor tree.
A node with a key takes over the mounted node with the same key.
A node without a key takes over the mounted node at the same child index.
The node kind must be the same.
A node that takes over a widget keeps its focus, its hover, and its edit state.
The view applies the new text, the new disabled state, and the new style identity.
A node without a match gets a new widget.
A mounted node without a match loses its widget, and the view clears its handle.
The children follow the descriptor order.
A supplied widget node keeps its widget only when the new descriptor supplies the same widget.
A different widget replaces the whole node.
An action handler can call `ui_view_update()`.
The view then frees the storage of a removed node after the dispatch ends.
`ui_view_update_ex()` reports the descriptor that caused a failure.
A failed update keeps the mounted nodes.
A matched widget can hold the properties that the update applied before it failed.

A list node holds a `ui_list`, and its class name is the list style class.
Use `ui_view_list()` to get the list handle.
A menu node holds a `ui_menu` and its layer.
The menu opens when the descriptor sets items, and it closes when they are empty.
The menu layer is the node widget.
Add the layer to the surface overlay layer to show the menu above the content.
Use `ui_view_menu()` to get the menu handle.

The view observes each widget that it owns.
When another owner destroys one of these widgets, the view clears its reference.
The typed handle becomes `NULL`.
An update can then create the node again.

The diagnostic gives an error, a detail message, and, when applicable, the descriptor that caused it.
The descriptor pointer is borrowed from the input tree.
Read it before the descriptor tree goes out of scope.
Use `ui_view_error_message()` to get a description of the error.
A dropdown descriptor adds its popup to the surface overlay layer.
The layer lets a popup extend outside a scrollview.
`ui_view_root()` returns the widget of the described root.

## Actions and state

An action starts at its source widget and bubbles to its parents.
Return `true` to stop the action.
An action has a kind, an integer ID, and fields for item or position data.
The control determines which fields apply.
See [UI library](ui-library-state.md#actions) for the action kinds.

An input emits `UI_ACTION_CHANGE` for user edits.
`ui_input_set_text()` loads a value without a change action.
It replaces the input text, clears undo history, and moves the caret to the end.
Do not set the text on each paint or frame.
The screen owns the application state and decides when to update the control.

Use `window_close()` to request that the app close a window.
Do not free the screen from its input or action callback.
Keep the callback data and widgets valid until the callback returns.

## Style and inspect

Use CSS for screen appearance.
The app owns the theme sheet and calls `window_ui_set_style()` after mounting.
It calls that function again when the theme changes.
Keep the sheet valid while the window uses it.
The UI resolves CSS values over the C layout defaults.
For example:

```css
.example { flex-direction: column; align-items: stretch; padding: 12px; gap: 8px; }
#result { color: #ffffff; }
```

The CSS implementation is a subset of browser CSS.
Sheets support `:root` variables and `var()` calls with fallback values.
A theme can set `:root` variables to change default chrome styles.

```css
:root { --accent: #3daee9; --surface: #141618; }
.panel { background: var(--surface, #1e1e2e); border-color: var(--accent); }
```

See [Flex layout](ui-library-state.md#flex-layout) for sizing and layout rules.
Unknown properties produce a diagnostic on standard error.
The inspector shows the resolved style.

Start the app with the inspector socket and open the screen:

```sh
build/bin/wsap_term --debug-socket /tmp/wsap.sock &
export WSAP_DEBUG_SOCKET=/tmp/wsap.sock
scripts/wsap-debug windows
```

Select the screen window number from `windows`.
Replace `N` with that number in these commands:

```sh
scripts/wsap-debug tree -w N --depth 4
scripts/wsap-debug widget -w N '@name'
scripts/wsap-debug lint -w N
scripts/wsap-debug screenshot -w N /tmp/example.png
```

The tree shows widget bounds, flags, and pending work.
The widget command shows resolved style and reported parts.
Run `lint` for each screen that you change.
It must report no findings.
See [UI debugging](ui-debugging.md) for headless runs, input injection, and scene inspection.

## When you need a custom widget

Use `UI_NODE_WIDGET` for a custom widget or a control that the descriptor does not cover.
Initialize the widget first and keep it detached until the view mounts.
For example:

```c
struct ui_node_desc control = {
    .kind = UI_NODE_WIDGET,
    .key = "custom",
    .props.widget = &custom->widget,
};
```

The caller owns the widget storage and keeps it alive until view destruction.
On success, the view destroys the widget and its descendants.
It does not free their enclosing storage.
On failure, the caller keeps the detached widget and destroys it.
Use `ui_view_widget(view, "custom")` to find the widget by key.
Do not copy a widget after initialization.
Keep strings passed to `ui_widget_set_style_identity()` valid while the widget uses them.
These rules differ from the copied descriptor strings of a view.
See [custom widgets and lifetime](ui-library-state.md#custom-widgets) for layout, paint, invalidation, and debug parts.
