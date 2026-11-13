# UI library

Use `ui/ui.h` to build a screen.
The surface manages updates.
The widget tree manages composition.
Controls use one action callback type.
Rows, columns, and stacks use Clay for layout.
For a complete window example, see [Build a window screen](ui-screen-guide.md).

## Build a screen

Describe the widget tree with `ui_node_desc` values.
Mount it once with `ui_view_mount()`.
The view owns the widget storage.
It copies the descriptor strings.
The app owns the userdata for actions and events.
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
              .props.input = { .placeholder = "Search",
                               .accessible_name = "Search" } },
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
    if (!ui_surface_set_root(surface, ui_view_root(screen->view))) {
        ui_view_destroy(screen->view);
        screen->view = NULL;
        return false;
    }
    return true;
}
```

The descriptor arrays and strings only need to stay valid through `ui_view_mount()`.
Use `ui_view_mount_ex()` to get the error, details, and the descriptor that caused it.
The diagnostic node is borrowed from the descriptor tree.
Read it while the tree is valid.
Label descriptors can include a link target.
`ui_view_mount()` copies that target into the label.
Set `bind.input`, `bind.label`, or another typed binding to get a control handle.
The view writes these slots only after a successful mount.
The handles stay valid until the view is destroyed.
You can also use `ui_view_input()` or another typed lookup.
Call setters on the handle when application state changes.
This practice preserves focus and edit state in the mounted control.
Destroying the view detaches and destroys its widget tree.
Call `ui_view_destroy()` after event or action dispatch returns.

Initialize the surface before you mount the view.
Set the viewport size with `ui_surface_resize()`.
Set the surface scale with `ui_surface_set_scale()`.
Set the surface theme with `ui_surface_set_sheet()`.
Use CSS for gaps, padding, colors, and control sizes.

Send keys and pointer input through `ui_surface_dispatch()`.
The host sets `ui_key_event.edit_unit` for editing keys.
Use `ui_edit_unit_for_mods()` with the binding policy of the host.
The input widget uses this unit for arrow movement and text deletion.
Shift extends a text selection during arrow movement.
Click an input to place the caret.
Drag to select text.
Double-click to select all text.
Ctrl+A or Command+A also selects all text.
The host supplies the platform double-click interval.
The default interval is 400 ms.
Set an accessible name on each input and dropdown.
Do not use placeholder text as the name.
The macOS policy uses Option for words.
It uses Command for line edges.
The default policy uses Control for words.
Both policies preserve raw modifiers and the native key event.
Send a UTF-8 commit or paste with `ui_event_text()`.
An unhandled Tab key moves focus forward.
Shift+Tab moves focus backward.

Call `ui_surface_tick()` to advance animations.
Build the frame with `ui_surface_build_scene()`.
Render that scene through the host backend.
The host calls `ui_surface_take_frame_request()` at the start of a frame.
The host then advances animations.
Next, the host builds the scene.
Requests made during this work schedule a later frame.
The `request_frame` callback of the host must queue a frame.
It must not render inside a setter or event callback.
Nested scene builds fail.

## Actions

An action contains a kind, an action ID, and an item index.
Buttons emit `UI_ACTION_ACTIVATE`.
Inputs emit `UI_ACTION_CHANGE`.
Menus and palettes emit activation and close actions.
Menus emit highlight actions for previews.
Dropdowns emit change actions with the selected item index.

Set an action handler with `ui_widget_set_action_handler()`.
The action starts at its source control.
It then moves upward through the ancestors of the control.
Return true from the handler to stop the action.
A root handler can handle all controls on a screen.
The handler stays set when the widget moves to another surface.
Control actions do not need an event bus subscription.

Use `ui_widget_set_event_handler()` for screen-level input.
The widget type handles the event first.
The screen handler runs when the widget type does not handle the event.
The screen handler receives key releases.
Keep handler userdata valid until you destroy the widget.

A menu can span several native surfaces.
Its menu options use the same `ui_action_handler` type.
The submenu provider returns model data.
This provider remains separate from action delivery.

## Buttons

A button copies its text.
`ui_button_set_text()` returns false if memory allocation fails.
The call also returns false if text exceeds `INT_MAX` bytes.
A failed call keeps the previous text.
Pass NULL to clear the text.
Destroy the button widget when its owner closes.

## Inputs

An input copies its text and placeholder.
`ui_input_set_text()` and `ui_input_set_placeholder()` return false if memory
allocation fails or if the source is longer than `INT_MAX` bytes.
A failed call keeps the previous value.
Text input removes invalid UTF-8 bytes and ASCII control characters.
The text setter clears the edit history and moves the caret to the end.
The text setter does not emit a change action.
User edits and accessibility edits emit a change action.
Destroy the input widget when its owner closes.

## Labels

A label owns a dynamic copy of its text.
`ui_label_set_text()` does not truncate text.
The call returns false if memory allocation fails.
The call also returns false if text exceeds `INT_MAX` bytes.
A failed call keeps the previous text and selection.
Pass NULL to clear the text and free its storage.
Destroy the widget when its owner closes.

Labels wrap text at word boundaries by default.
Use `ui_label_set_wrap()` for character wrapping, clipping, or end ellipsis.
Ellipsis applies to each explicit line when line width is insufficient.
Blank lines and trailing newlines retain their height.
CRLF counts as one line break.
Measurement, painting, and pointer input use the same line breaks.
`ui_label_set_align()` selects left, center, or right alignment.
Use CSS padding to inset label content.
A single visual line stays vertically centered.
Multiple lines start at the top.
Text uses terminal cell metrics.
Labels do not provide proportional shaping or full Unicode word segmentation.

Enable selection with `ui_label_set_selectable(label, true)`.
Drag the pointer to select text across lines.
Double-click to select a word.
Triple-click to select an explicit line.
Shift-click extends the selection.
Left and Right move the selection endpoint.
Hold Shift with Left or Right to extend the selection.
Home and End move to the start and end of the text.
Ctrl+A or Command+A selects all text.
Ctrl+C or Command+C copies the selected text.
Escape clears the selection.
Selection offsets from `ui_label_selection()` are UTF-8 byte offsets.
Copying preserves original text and line breaks.
Selection does not edit label text.

Use `ui_label_set_link()` to attach a URL or target string.
Click the text to activate the link.
Alternatively, focus the label and press Enter or Space.
The label emits `UI_ACTION_ACTIVATE` with its action ID.
An application handler can intercept the action.
If no handler intercepts the action, the `open_url` service of the host opens the target.
The host determines which target formats it accepts.
`ui_label_set_disabled()` blocks activation and selection gestures.
A link can also be selectable.
Dragging to select text does not activate the link.
Plain labels do not accept keyboard focus.
Selectable labels and links participate in Tab focus traversal.

The surface host provides optional `copy_text` and `open_url` callbacks.
The UI client connects these callbacks to the system clipboard and platform URL opener.
The callbacks borrow strings only for the duration of the call.
A headless host can record these requests without launching a browser.

Style these label parts with CSS:

```css
.label-selection { background: #45475a; color: #ffffff; }
.label-link { color: #89b4fa; border-width: 1px; }
.label-link:hover, .label-link:focus { color: #b4befe; }
.label-link:disabled { color: #585b70; }
.label-focus { border-color: #89b4fa; border-width: 1px; }
```

The border width of `.label-link` sets underline thickness.
The border properties of `.label-focus` set the keyboard focus outline.
The CSS `cursor` property overrides the default cursor of the label.
A selectable label uses a text cursor.
A link uses a pointer cursor over its painted text.
The inspector reports content bounds and each visible text line.

## Composition

All controls use the same child attachment functions.
A scrollview accepts one child widget.
A second attachment fails.
The existing child widget remains unchanged.
Updating list items preserves list attachments and handlers.
Dropdowns copy their items.
They show a searchable popup.
A dropdown descriptor gets a popup host from the view.
The popup can extend outside scrollviews.
The popup follows the anchor when the layout changes.
It closes when a clipping ancestor scrolls the anchor out of sight.
Set `overlay_key` to select a stack in the view instead.
For manual composition, set the overlay parent to a container with stack layout.
`ui_dropdown_set_value()` returns false when it cannot update the label.
A failed call keeps the previous selection.
Scrollviews can show an optional draggable scroll track.
Scrollviews scroll focused descendants into view.

Child widgets paint in insertion order.
The last child widget paints on top.
Pointer input targets the topmost visible hit region.
An unhandled event moves upward from that target to its ancestors.
The event does not pass to an underlying sibling widget.
Pointer capture redirects event delivery.
Each handler receives the event once.
Clicking blank or passive content clears keyboard focus.
Clicks inside a modal window retain focus within that modal.

The UI context tracks the hover target.
It sends a leave event when the hover target changes.
A modal subtree blocks pointer input to other controls.
Focus traversal remains inside the active modal subtree.
The context restores the previous focus when the modal closes.

Widget bounds and pointer positions use surface coordinates.
Paint commands use widget-local coordinates.
Use the geometry conversion helpers in `widget.h`.

## Custom widgets

Give each operation table a name.
A paint callback draws only its own content.
The framework paints child widgets.
Set `clip_children` to clip child painting and pointer input.
Set `hit_test` when the hit region differs from widget bounds.

The measure callback of a container measures its child widgets.
The layout callback places child widgets with `ui_widget_place()`.
Without those callbacks, the framework provides a stack layout.
Use `prepare` to update content and child links before style and layout passes.
Do not modify content or layout inside paint callbacks.

A setter marks dirty state with `ui_widget_invalidate()`.
Invalidating measurement also invalidates layout and paint.
The surface resolves content, style, measurement, and layout before handling input and building scenes.

Report internal hit regions through `debug_parts`.
Check a modified screen with the [UI inspector](ui-debugging.md).
The alignment lint check must pass without findings.
Capture a screenshot to verify visual output.

## Lifetime

The caller owns widget memory.
`ui_widget_destroy()` releases widget resources.
It destroys all descendant widgets.
It does not free the enclosing struct.
Destroy the surface before you free its root widget storage.

Detaching a widget cancels focus, capture, hover, and context subscriptions.
An action or event callback can destroy a widget.
Keep memory used by dispatch valid until dispatch returns.
Do not copy a widget struct after initialization or attachment.

## Accessibility

Each initialized widget receives a stable, nonzero accessibility ID.
Virtual children use IDs from `ui_widget_accessibility_id_allocate()`.
The accessibility tree copies names, descriptions, and values.
It also stores roles, states, actions, and device-pixel bounds.
Bounds start at the top-left corner of the surface.
Text offsets count UTF-8 bytes.
The tree removes invalid UTF-8 bytes before copying strings.
It adjusts text offsets to match the copied value.
A tree rebuild reuses unchanged values from the previous tree.
The rebuild does not copy unchanged values again.
Widgets can supply help text and an invalid state flag.
Providers receive the same tree on every platform.
The provider API uses an opaque native handle and a small callback set.
It receives a snapshot only for the duration of each publish call.
It sends actions through the shared action sink.
The window queues those actions for the UI thread.
The dispatcher validates the target control before executing an action.
It rejects hidden, disabled, ignored, or unsupported targets.
The tree indexes IDs for fast lookup and duplicate detection.

AccessKit maps the tree to AT-SPI on Linux.
Its macOS adapter maps the tree to AppKit accessibility elements.
The C adapter resides in `src/ui/yasl/accessibility/`.
The UI core has no AccessKit dependency.
Keep native platform types in the surface provider.
Keep the UI tree and provider interface platform neutral.
While a modal is open, the accessibility tree exposes only the modal path and content.
Actions cannot reach controls behind the modal.
Password nodes do not copy their value or text offsets into the tree.

Each drawn frame publishes accessibility changes without checking UI damage.
A window publishes accessibility data only while its provider is active.
On Linux, AccessKit activates when `IsEnabled` is true in `org.a11y.Status`.
Set this property before an accessibility client reads the tree.
`WSAP_ACCESSIBILITY=1` prepares local snapshots before activation.
This variable does not enable the accessibility bus or change desktop settings.
When a client begins listening, the provider sends an empty action request.
The window then renders a frame.
That frame publishes the complete tree.
The terminal view updates its accessibility text at most once every 100 ms.
A timer sends the final text change after a burst of terminal output.
A terminal text update does not need to invalidate the UI scene.
Removed or detached macOS elements return empty results.
They cannot execute actions.
A provider without an action sink is read-only.
Text runs use ICU grapheme and word boundaries.
The adapter converts text selections between run character indices and UTF-8 byte offsets.
AccessKit manages native events and platform text offsets.

The terminal view exposes up to 500 history rows above the current viewport.
It also exposes visible screen text, the cursor position, and selected text ranges.
This content is read-only.
History text preserves grapheme clusters and joins soft-wrapped rows.
History retrieval reads from the terminal writer section.
Retrieval does not alter current text selections.

Run `ui-accessibility-test` to verify accessibility trees and action routing.
Run `history-text-test` to verify Unicode history handling and viewport boundaries.
Run `darwin-native-app-test` to verify AppKit output, ranges, ordering, and object lifetimes.
These tests do not replace testing with VoiceOver and Orca.
The visual bell flashes the terminal view when the bell triggers.
High contrast mode applies a high-contrast color palette.
Tab bars move focus across tabs and the New Tab button using Left and Right.
Enter or Space activates the focused item.
Menus scroll by one visible page with Page Up and Page Down.

## Scope

The library provides terminal controls and small application screens.
Text geometry uses terminal cell metrics.
The library does not provide a general proportional-text editor.
Terminal cells remain a separate render input.
The terminal engine and render backends do not depend on the screen API.

Core contracts have behavioral tests in `tests/ui/`.
The integrated screen test resides in `tests/ui/acceptance_test.c`.
Window and UI binding tests reside in `tests/ui/yasl/`.

## Clay layout

Clay calculates the bounds of boxes and default containers.
Each UI surface owns one Clay context and arena.
The build uses the pinned source in `src/ui/extern/clay/`.
The build does not download external code.

Set sizes with CSS:

```css
.panel { flex-direction: column; width: grow; height: grow; gap: 8px; }
.sidebar { width: 180px; }
.content { width: grow; min-width: 80px; max-width: 600px; }
.note { width: 50%; height: fit; }
```

`fit` sizes an element to its content.
`auto` uses the content size or the control default.
`grow` distributes available space among growing items on that axis.
A fixed size does not shrink.
A fit or grow item can shrink down to its minimum size.
Clay shrinks the largest items first.
Percent sizes calculate space after subtracting parent padding and gaps.
Use `min-width`, `min-height`, `max-width`, and `max-height` to define limits.
Use padding and gaps to space widgets.
Box layout ignores child margins.
Individual widget parts can still use margins, such as menu separators.
The layout engine does not support weighted `flex-grow` or `justify-content: space-between`.

Clay computes floating-point bounds.
`widget.layout_bounds` stores these bounds in surface space.
`widget.bounds` rounds outer edges to integers for painting and pointer input.
The inspector reports both sets of bounds.

A nested row or column shares the layout pass of its parent.
A stack uses Clay floating elements.
A default container expands each child to fill container bounds.
Anchored child widgets follow popup placement rules.
These rules preserve automatic flip and clamp behaviors.

A custom widget can retain its own measure and layout callbacks.
Clay treats that widget as a single element.
Set `width_sensitive` in widget ops when content height depends on width.
The layout pass measures this content using the width assigned by Clay.
The label widget uses this flag for wrapped text.
Measurement changes can request a subsequent frame.

Clay manages layout geometry.
The UI library manages focus, input, actions, clipping, and scene construction.
The text painter uses cell metrics from the UI font.
Layout sizes can use pixels without cell units.
Proportional text rendering is a separate planned change.
