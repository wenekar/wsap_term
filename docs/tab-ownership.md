# Tab ownership contract

This contract separates app tabs from terminal content.
It applies to the tab ownership refactor.

## Owners

| Owner | State and work |
| --- | --- |
| `src/app/tabs/` | Tab order, identity, selection, close state, and window transfers. It owns the tab bar, previews, and command palette. It owns each terminal-content handle. |
| `src/terminal/` | One session and its terminal widgets, snapshot, font, zoom, selection, scroll state, and terminal input. |
| `src/ui/widgets/tabbar.*` | Tab-bar drawing, animation, focus, and hit testing. It sends actions to the app. |
| `src/ui/yasl/` | Native windows, surfaces, input routing, scale, and frame scheduling. It has no tab policy. |
| `src/session/` | The term core, PTY, input queue, and PTY size notification. |
| `src/app/app.c` | Window creation and frame composition. It alone calls the renderer. |

A terminal-content handle has no tab array or active-tab index.
It has no tab-bar, palette, preview, or window-transfer state.
The app must use public terminal headers.
The terminal library must not include an app header.
Do not relax the include graph to make the move compile.

## Terminal-content interface

`src/terminal/terminal.h` declares an opaque `struct terminal`.
The public interface acts on this handle, not on a window lookup.
Keep terminal configuration and frame-source data in this layer.
Keep the theme-picker host interface if the terminal menu needs it.

The core interface has these operations:

```c
struct terminal *terminal_create(struct window *window,
                                  const struct terminal_spec *spec);
void terminal_destroy(struct terminal *terminal);
struct ui_widget *terminal_widget(struct terminal *terminal);
void terminal_set_window(struct terminal *terminal, struct window *window);
struct session *terminal_session(struct terminal *terminal);
const char *terminal_title(const struct terminal *terminal);
void terminal_commit_geometry(struct terminal *terminal);
void terminal_prepare(struct terminal *terminal, uint64_t now_ms);
bool terminal_frame_source(struct terminal *terminal, uint64_t now_ms,
                            struct terminal_frame_source *out);
```

The create spec supplies the initial content size and font size.
It also supplies the session config, theme, cwd, and host callbacks.
Use the initial content size to start the PTY at the target cell grid.
A zero content size can use a documented default before native configure.
Do not subtract tab-bar height inside the terminal library.

The host reports title changes and process exit to the app.
Pass the terminal handle with each report.
The app can then find the tab by identity, not by a saved array index.
Terminal commands can request app actions through a host callback.
The callback must not expose the app tab collection to terminal content.

The window pointer is borrowed.
It supplies native services, such as clipboard and redraw.
A null window is valid while content is in transfer.
A window bind does not move ownership of the terminal handle.
A bind must update scale and native service targets before input resumes.

The session and widget accessors return borrowed pointers.
The app keeps the terminal handle alive while it uses these pointers.
The frame source stays valid until the next prepare or destruction.

## App tab interface

`src/app/tabs/tabs.h` declares the app tab interface.
`src/app/tabs/internal.h` holds its private state.
The directory has its own `meson.build`.
`src/app/meson.build` calls `subdir('tabs')`.

The app interface supplies these operations:

- Mount a tabbed app root in a window.
- Create, close, select, and reorder tabs.
- Get the active terminal-content handle.
- Get the live tab count and active tab index.
- Take a terminal-content handle from a window.
- Attach a terminal-content handle to a window.
- Detach a tab into a new app window.
- Handle tab-bar actions, preview requests, and tab shortcuts.
- Prepare app state before UI layout and terminal snapshot capture.

Use an `app_tabs_` prefix for this interface.
Remove tab ownership operations from the terminal interface.
Update callers and tests instead of keeping a second owner as a wrapper.
The app root owns the window UI mount.
Settings windows keep their existing separate mount.

An attach or open operation takes ownership only on success.
On failure, the caller still owns the handle.
A take operation transfers ownership to the caller.
A failed transfer must restore the source or destroy the handle once.
No path can leave two owners or no owner.
Keep one explicit owner for content during a native drag.

## Tab state and close

The app is the only authority for live tabs.
A tab has a stable identity across reorder and transfer.
A terminal pointer can serve as that identity while it is alive.
Use a separate token if a closing display entry outlives the content.

Close changes logical state before the next frame.
Select the next live tab in the same app operation.
Do not select, send input to, preview, or transfer a closing tab.
Count all closing tabs when the app derives the live count.
Closing the last live tab closes the window.

The app may keep an owned label as a display entry for a close animation.
That entry is not a live terminal tab.
The app must retire session content at a safe loop boundary.
Do not free content while one of its callbacks is on the stack.
Widget animation completion must not decide when a session is destroyed.
An app-owned presentation deadline retires the display entry.
The app schedules a frame for this deadline even if the widget is idle.

Publish the full label and selection state after each model change.
Do not wait for root prepare to repair an incomplete tab mutation.
Widget array capacity and label count are not app state.
Remove the stale-count window between widget removal and label update.

## Geometry commit

There is one source of terminal geometry: the laid-out content widget.
The app derives its rectangle from native bounds, CSS, and tab presentation.
Use whole `rect2i` and `vector2i` values across interfaces.

Reserve the full CSS tab-bar height while at least two tab entries exist.
A closing display entry keeps its reservation until its presentation ends.
Logical close and selection still take effect at once.
Release the bar space after the app retires the last closing entry.
Commit the resulting grid change once, after UI layout.
The content origin follows the visible tab-bar height.
The content slides up as the bar collapses.
Keep the reserved content size until the close presentation ends.
Then expand the content once at its final origin.
A drag preview can reveal as an overlay.
Animation progress can move the content origin.
It must not change the content size or cell grid.
If needed, add a tab-bar query for the full CSS height.
That query must not depend on animation progress or widget tab count.

A tab mutation, configure, restyle, scale change, or zoom invalidates layout.
These paths must not each resize the session directly.
After UI layout, `terminal_commit_geometry()` derives the cell grid from
its content widget and font metrics.
This is the terminal layer's only session-resize call site.
Creation can use the same grid calculation for the initial session size.
An unchanged grid is a no-op.
Do not use a separate cached tab-bar height to suppress a geometry commit.

Inactive content can keep its last grid until it becomes active.
Before selection is rendered or receives input, apply its current geometry.
Do not carry a pending grid computed for a different window or font.
The session owns PTY notification throttling.
Keep the latest pending size and the final trailing notification.

## Frame order

For the main surface, the app runs these stages in order:

1. Apply pending app tab changes and safe content retirement.
2. Tick and lay out the UI.
3. Commit active terminal geometry.
4. Capture the active terminal snapshot.
5. Build the UI scene and compose the frame.
6. Present and schedule the next frame.

Read the active handle again after an operation that can close a window.
Never use a snapshot from before a geometry change in the same frame.
If scene preparation changes the content bounds, skip the frame and retry.
Keep text-blink scheduling correct when snapshot capture moves after layout.
Do not add a second snapshot update to hide a wrong frame order.

## Migration and acceptance

Move app behavior under `src/app/tabs/`.
Do not create a new sibling library for the tab controller.
Move tests only when their behavior now belongs to the app subsystem.
Keep test names flat and unchanged after a file move.
Do not test private collection layout or a fixed internal call order.

Keep these behaviors:

- Tab creation inherits cwd and zoom as before.
- Reorder, native drag, detach, and attach preserve the session.
- Theme reload, settings, clipboard, IME, accessibility, and key encoding work.
- The tab preview and command palette still work.
- Process exit closes the correct tab after its last output.
- The app renderer and native window keep their current ownership.

Add behavior coverage for the known failures:

- Open a second tab, then close it. Shell rows return to the first size.
- Close several tabs before the animation ends. Keep the grid size fixed.
- Slide the content origin with the bar during collapse.
- Resize once after the close presentation ends.
- Close and reopen during an animation. Keep the bar reservation.
- Select an inactive tab after resize or zoom. Its grid fits its content.
- Transfer a tab across windows. Its grid fits the destination.

Run the build and the relevant unit tests.
Run `python3 tools/dev/dirgraph.py build` with no violations or cycles.
Check the changed UI with the inspector.
The inspector lint must report no findings.
Measure shell rows as well as view bounds in the live resize check.
A correct screenshot alone does not prove that the PTY grid is correct.
