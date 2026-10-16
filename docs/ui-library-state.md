# UI library

Use `ui/ui.h` to build a screen.
The surface manages updates.
The widget tree manages composition.
Controls use one action callback type.
Rows, columns, and stacks use Clay for layout.

## Build a screen

Describe the widget tree with `ui_node_desc` values.
Mount it once with `ui_view_mount()`.
The view owns the widget storage and copies the descriptor strings.
Action and event userdata stay owned by the app.
Keep the view alive while the surface uses its widgets.

```c
#include "ui/ui.h"

struct settings_screen {
    struct ui_view *view;
};

static bool action(struct ui_widget *source,
                   const struct ui_action *event, void *userdata)
{
    (void)source;
    struct settings_screen *screen = userdata;
    if (event->kind == UI_ACTION_ACTIVATE && event->id == 1) {
        /* Apply the settings here. */
        return true;
    }
    return false;
}

static bool settings_init(struct settings_screen *screen,
                          struct ui_surface *surface)
{
    const struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN,
        .key = "settings",
        .props.layout = { .align = UI_BOX_ALIGN_STRETCH, .gap = 8 },
        .children = UI_NODES(
            { .kind = UI_NODE_INPUT, .key = "search",
              .props.input = { .placeholder = "Search" } },
            { .kind = UI_NODE_BUTTON, .key = "apply", .action_id = 1,
              .props.button = { .text = "Apply" } }
        ),
        .child_count = 2,
        .on_action = action,
        .action_userdata = screen,
    };
    screen->view = ui_view_mount(&root, NULL);
    if (!screen->view)
        return false;
    ui_surface_set_root(surface, ui_view_root(screen->view));
    return true;
}
```

The descriptor arrays and strings only need to live through `ui_view_mount()`.
Label descriptors can include a link target.
The mount copies that target into the label.
Use `ui_view_input()` or another typed lookup to keep a control handle.
Call its setters when app state changes.
This keeps focus and edit state in the mounted control.
Destroying the view detaches and destroys its tree.
Call `ui_view_destroy()` after event or action dispatch returns.

Initialize the surface before mounting the view.
Set its viewport and scale with `ui_surface_resize()` and
`ui_surface_set_scale()`.
Set its theme with `ui_surface_set_sheet()`.
Use CSS for gaps, padding, colors, and control sizes.

Send keys and pointer input through `ui_surface_dispatch()`.
The host sets `ui_key_event.edit_unit` for editing keys.
Use `ui_edit_unit_for_mods()` with the host's binding policy.
The input widget uses this unit for arrow movement and deletion.
Shift extends a selection during movement.
Click an input to place the caret.
Drag to select text.
Double-click to select all text.
Ctrl+A or Command+A also selects all text.
The host supplies the platform double-click interval.
The default interval is 400 ms.
Set an accessible name on each input and dropdown.
Do not use placeholder text as the name.
The macOS policy uses Option for words and Command for line edges.
The default policy uses Control for words.
The policies preserve the raw modifiers and native key event.
Send a UTF-8 commit or paste with `ui_event_text()`.
An unhandled Tab key moves focus.
Shift+Tab moves focus back.

Call `ui_surface_tick()` to advance animations.
Build the frame with `ui_surface_build_scene()`.
Render that scene through the host backend.
The host calls `ui_surface_take_frame_request()` at the start of a frame.
It then advances animations and builds the scene.
Requests raised during that work schedule a later frame.
The host's `request_frame` callback must queue a frame.
It must not render inside a setter or event callback.
Nested scene builds fail.

## Actions

An action has a kind, an action ID, and an item index.
Buttons emit `UI_ACTION_ACTIVATE`.
Inputs emit `UI_ACTION_CHANGE`.
Menus and palettes also emit activation and close actions.
Menus emit highlight actions for previews.
Dropdowns emit change actions with the selected item index.

Set a handler with `ui_widget_set_action_handler()`.
The action starts at its source control.
It then moves through that control's ancestors.
Return true to stop it.
A root handler can handle all controls on a screen.
The handler stays set when the widget moves to another surface.
No bus subscription is needed for control actions.

Use `ui_widget_set_event_handler()` for screen level input.
The widget type handles the event first.
The screen handler runs when the type does not handle it.
The screen handler receives key releases.
Keep its userdata alive until the widget is destroyed.

A menu can span several native surfaces.
Its options use the same `ui_action_handler` type.
The submenu provider returns model data.
It is separate from action delivery.

## Labels

A label owns a dynamic copy of its text.
`ui_label_set_text()` does not truncate it.
The call returns false if allocation fails or the text exceeds `INT_MAX` bytes.
A failed call keeps the old text and selection.
Pass NULL to clear the text and release its storage.
Destroy the widget when its owner closes.

Labels wrap at words by default.
Use `ui_label_set_wrap()` for character wrapping, clipping, or end ellipsis.
Ellipsis applies to each explicit line when its width is too small.
Blank lines and trailing newlines keep their height.
CRLF acts as one line break.
Measurement, painting, and pointer input use the same line breaks.
`ui_label_set_align()` selects left, center, or right alignment.
Use CSS padding to inset the content.
A single visual line stays vertically centered.
Multiple lines start at the top.
Text uses terminal cell metrics.
Labels do not provide proportional shaping or full Unicode word segmentation.

Enable selection with `ui_label_set_selectable(label, true)`.
Drag to select text across lines.
Double-click to select a word.
Triple-click to select an explicit line.
Shift-click extends the selection.
Left and Right move the selection endpoint.
Shift extends it with those keys.
Home and End move to the start and end of the text.
Ctrl+A or Command+A selects all.
Ctrl+C or Command+C copies the selection.
Escape clears it.
Selection offsets from `ui_label_selection()` are UTF-8 byte offsets.
Copy keeps the original text, including line breaks.
Selection does not edit the text.

Use `ui_label_set_link()` to attach a copied URL or target.
Click the text, or focus it and press Enter or Space.
The label emits `UI_ACTION_ACTIVATE` with its action ID.
A handler can take the action to implement an application link.
Otherwise the surface host's `open_url` service opens the target.
The host decides which targets it accepts.
`ui_label_set_disabled()` blocks activation and selection gestures.
A link can also be selectable.
Dragging a selection does not activate it.
Plain labels do not take keyboard focus.
Selectable labels and links participate in Tab traversal.

The surface host supplies optional `copy_text` and `open_url` callbacks.
The window's UI client connects them to the clipboard and platform URL opener.
The callbacks borrow their strings only for the duration of the call.
A headless host can record these requests without opening a browser.

Style these label parts with CSS:

```css
.label-selection { background: #45475a; color: #ffffff; }
.label-link { color: #89b4fa; border-width: 1px; }
.label-link:hover, .label-link:focus { color: #b4befe; }
.label-link:disabled { color: #585b70; }
.label-focus { border-color: #89b4fa; border-width: 1px; }
```

The link part's border width sets underline thickness.
The focus part sets the keyboard focus outline.
CSS `cursor` overrides the label's default cursor.
A selectable label uses a text cursor.
A link uses a pointer over its painted text.
The inspector reports content bounds and each visible text line.

## Composition

All controls use the same child attachment calls.
A scrollview accepts one child.
A second attachment fails without changing the existing child.
Updating list items preserves the list's attachment and handlers.
Dropdowns copy their items and show a searchable popup.
Set their overlay parent to a container with stack layout.
Scrollviews can show an optional draggable scroll track.
They bring focused descendants into view.

Children paint in insertion order.
The last child is on top.
Pointer input targets the top visible hit region.
An unhandled event moves from that target to its ancestors.
It does not pass through to a covered sibling.
Pointer capture changes the target.
Each handler receives the event once.
Clicking blank or passive content clears keyboard focus.
Clicks inside a modal keep focus within that modal.

The context tracks the hover target.
It sends leave when that target changes.
A modal subtree blocks pointer input to other controls.
Focus traversal stays inside the active modal subtree.
The context restores the prior focus when that modal closes.

Widget bounds and pointer positions use surface coordinates.
Paint commands use widget-local coordinates.
Use the geometry conversion helpers in `widget.h`.

## Custom widgets

Give each operation table a name.
A paint callback draws only its own content.
The framework paints its children.
Set `clip_children` to clip child paint and pointer targeting.
Set `hit_test` when the hit region differs from the widget bounds.

A container's measure callback measures its children.
Its layout callback places them with `ui_widget_place()`.
Without those callbacks, the framework supplies a stack layout.
Use `prepare` to update content and child links before style and layout.
Keep paint callbacks free of content and layout changes.

A setter marks the required work with `ui_widget_invalidate()`.
Measurement invalidation also marks layout and paint.
The surface prepares content, style, measurement, and layout before input
and scene construction.

Report internal hit regions through `debug_parts`.
Check a changed screen with the [UI inspector](ui-debugging.md).
Its alignment lint must pass.
Capture a screenshot to check the result.

## Lifetime

The caller owns widget storage.
`ui_widget_destroy()` releases widget resources and destroys descendants.
It does not free the enclosing struct.
Destroy the surface before releasing its external root storage.

Detach cancels focus, capture, hover, and context subscriptions.
An action or event callback can destroy a widget.
Keep storage used by dispatch alive until dispatch returns.
Do not copy a widget after initialization or attachment.

## Accessibility

Each initialized widget has a stable, nonzero accessibility ID.
Virtual children use IDs from `ui_widget_accessibility_id_allocate()`.
The accessibility tree copies names, descriptions, and values.
It also stores roles, states, actions, and device-pixel bounds.
Bounds start at the surface's top-left corner.
Text offsets count UTF-8 bytes.
The tree removes invalid UTF-8 bytes before it copies strings.
It updates text offsets to match the copied value.
Widgets can supply help text and an invalid state.
Providers receive the same tree on every platform.
The provider API uses an opaque native handle and a small set of callbacks.
It receives a snapshot only for the duration of each publish call.
It sends actions through the shared action sink.
The window queues those actions for the UI thread.
The dispatcher checks the current control before it runs an action.
It rejects hidden, disabled, ignored, and unsupported targets.
The tree indexes IDs for lookup and duplicate checks.

Linux maps the tree to AT-SPI.
macOS maps it to AppKit accessibility elements.
A Windows UI Automation provider can use the same tree and action sink later.
Keep native platform types in the surface provider.
Keep the UI tree and provider contract platform neutral.
While a modal is open, the tree contains its path and content.
Actions cannot reach controls behind the modal.
Password nodes do not copy their value or text offsets into the tree.

Each drawn frame publishes accessibility changes without a UI damage check.
A terminal text change does not need to change the UI scene.
Removed or detached macOS elements return empty results.
They cannot run actions.
A provider with no action sink is read-only.

The terminal view exposes up to 500 history rows before the current viewport.
It also exposes visible text, its caret, and its selected text range.
The value is read-only.
History text keeps grapheme clusters and joins soft-wrapped rows.
History capture uses the terminal writer section.
It does not change the selection.

Run `ui-accessibility-test` to check trees and action routing.
Run `history-text-test` to check Unicode history and viewport limits.
Run `darwin-native-app-test` to check AppKit output, ranges, order, and lifetime.
These tests do not replace checks with VoiceOver and Orca.
Visual bell flashes the terminal view when the bell rings.
High contrast mode applies a high-contrast palette.
Tab bars move focus across tabs and the New Tab button with Left and Right.
Enter or Space activates the focused item.
Menus move by one visible page with Page Up and Page Down.

## Scope

The library supplies terminal controls and small application screens.
Text geometry uses terminal cell metrics.
It does not provide a general proportional-text editor.
Terminal cells remain a separate render input.
The terminal engine and render backends do not depend on the screen API.

The main contracts have behavioral tests in `tests/ui/`.
The integrated screen is in `tests/ui/acceptance_test.c`.
Window and UI binding tests are in `tests/ui/yasl/`.

## Clay layout

Clay computes the bounds of boxes and default containers.
Each UI surface owns one Clay context and arena.
The build uses the pinned source in `src/ui/extern/clay/`.
The build does not download it.

Set sizes with CSS:

```css
.panel { flex-direction: column; width: grow; height: grow; gap: 8px; }
.sidebar { width: 180px; }
.content { width: grow; min-width: 80px; max-width: 600px; }
.note { width: 50%; height: fit; }
```

`fit` uses the content size.
`auto` also uses the content size, or the control's default.
`grow` shares available space with other grow items on that axis.
A fixed size does not shrink.
A fit or grow size can shrink to its minimum size.
Clay reduces the largest items first.
Percent sizes use the parent's space after padding and gaps.
Use `min-width`, `min-height`, `max-width`, and `max-height` to set limits.
Use padding and gaps for spacing between widgets.
Box layout does not apply child margins.
Widget parts can still use margins, such as menu separators.
Weighted `flex-grow` and `justify-content: space-between` are removed.

Clay computes float bounds.
`widget.layout_bounds` keeps these bounds in surface space.
`widget.bounds` rounds their edges for painting and pointer input.
The inspector reports both bounds.

A nested row or column uses the same layout pass as its parent.
A stack uses Clay floating elements.
A default container fills its bounds with each child.
Anchored children use the popup placement rules.
These rules keep flip and clamp behavior.

A custom widget can keep its measure and layout callbacks.
Clay treats that widget as one element.
Set `width_sensitive` in its ops if its content height depends on width.
The layout pass measures this content at the width that Clay assigns.
The label uses this flag for wrapped text.
Measurement changes can request a later frame.

Clay owns layout geometry.
The UI library owns focus, input, actions, clipping, and scene construction.
The current text painter still uses the UI font's cell metrics.
Layout sizes can use pixels without cell units.
Proportional text painting is a separate change.
