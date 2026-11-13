# Tab ownership architecture

This document describes the tab ownership architecture.
It separates application tabs from terminal content.

## Subsystems

| Subsystem | Responsibility |
| --- | --- |
| `src/app/tabs/` | Manages the tab model and tab views. `src/app/tabs/model.c` owns each terminal handle and the native drag. Each window view holds tab order, selection, close state, the tab bar, previews, and the command palette. |
| `src/terminal/` | Manages one session and its terminal widgets, snapshot, font, zoom, selection, scroll state, and terminal input. |
| `src/ui/widgets/tabbar/` | Keeps tab bar style, geometry, scene drawing, and interaction state in separate files. It sends tab bar actions to the application. |
| `src/ui/yasl/` | Manages native windows, surfaces, input routing, scale, and frame scheduling. Each host pass calls the service step of the owner. It contains no tab policy. |
| `src/session/` | Manages the term core, PTY, input queue, and PTY size notifications. |
| `src/app/app.c` | Creates windows and composes frames. It alone calls the renderer. It owns the tab model. Its host hook runs the tab service step. |

A terminal handle has no tab array and no active-tab index.
It has no tab-bar, palette, preview, or window-transfer state.
The application uses public terminal headers.
The terminal library includes no application header.
The include graph enforces this separation.

The tab bar public header is `src/ui/widgets/tabbar.h`.
Its private types and functions stay in `src/ui/widgets/tabbar/internal.h`.
Only the tab bar implementation can include that private header.

## Terminal-content interface

The header `src/terminal/terminal.h` declares an opaque `struct terminal`.
The public interface operates on this handle.
It does not use window lookups.
This layer holds terminal configuration and frame-source data.
It provides the theme-picker host interface for the terminal menu.

The core interface provides these operations:

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
It also supplies the session configuration, theme, current working directory, and host callbacks.
The initial content size starts the PTY at the target cell grid.
A zero content size uses a default size before native configure.
The terminal library does not subtract tab-bar height.

The host reports title changes and process exit to the application.
The application sets the host once at creation time.
The model is the user data of every host.
Every report passes the terminal handle.
The model finds the tab by identity, not by an array index.
The tab records the window that holds it.
A tab without a window receives no reports.
Terminal commands request application actions through host callbacks.
Host callbacks do not expose application tabs to terminal content.

The terminal borrows the window pointer.
The window pointer provides native services, such as clipboard and redraw.
A null window pointer is valid while content is in transfer.
Binding a window does not move ownership of the terminal handle.
A bind updates scale and native service targets before input resumes.

The session and widget accessors return borrowed pointers.
The application keeps the terminal handle alive while it uses these pointers.
The frame source stays valid until the next prepare call or destruction.

## App tab interface

The header `src/app/tabs/tabs.h` declares the application tab interface.
The header `src/app/tabs/model.h` declares the tab model.
The header `src/app/tabs/internal.h` holds the state of the window views.
The directory `src/app/tabs/` has its own `meson.build`.
The build file `src/app/meson.build` calls `subdir('tabs')`.

The application interface provides these operations:

- Mount a tabbed root widget in a window (`app_tabs_mount`).
- Create, close, select, and reorder tabs (`app_tabs_new`, `app_tabs_close`, `app_tabs_select`, `app_tabs_reorder`).
- Get the active terminal handle (`app_tabs_active`).
- Get the live tab count and active tab index (`app_tabs_count`, `app_tabs_active_index`).
- Move a tab to another window (`app_tabs_move`).
- Detach a tab into a new window (`app_tabs_detach`).
- Handle tab-bar actions, preview requests, and tab shortcuts (`app_tabs_shortcut`).
- Retire closing tabs in the service step (`app_tabs_service`).

Functions in this interface use the `app_tabs_` prefix.
The terminal interface contains no tab ownership operations.
The application root owns the window UI mount.
Settings windows keep a separate mount.

## Tab model

The application struct `struct app` owns one tab model: `struct app_tab_model`.
The module `src/app/tabs/model.c` owns every terminal handle for its full life.
The model stores tabs in a flat array sorted by ID.
Each tab entry holds an ID, its terminal content, its close label, and its retire timestamp.
Each tab entry records the window that currently holds it.
The application provides the model to each tab view in `app_tabs_spec`.

Windows hold tab IDs and borrow content widgets.
The window view keeps tab IDs in tab-bar order and tracks the active index.
A window never destroys a terminal.
Only `src/app/tabs/model.c` calls `terminal_destroy()`.
When a window closes, its destroy hook returns its tab IDs to the model.
The model then destroys those tabs.
The host reaps closed windows at the start of each host pass.
No terminal callback is on the call stack at that time.

The model destroys a terminal only in these situations:

- The service step of the host pass.
- The window reap pass.
- A drag completion or a failed transfer outside terminal callbacks.
- Application teardown.

Callbacks and hooks can mark a tab as closing, select a tab, or reorder tabs.
Callbacks and hooks never destroy a terminal.

The function `app_tabs_move` handles tab moves between windows.
Internal helpers bind and unbind tabs from windows.
Unbinding leaves the tab in the model without a window.
The caller then binds the tab to a window or destroys the tab.
Binding and window opening take the tab only on success.
On failure, the caller still holds the tab.
A failed transfer restores the source window or destroys the tab.
Binding rejects a closing tab or an exited terminal process.
No code path leaves a tab in two windows at the same time.

The model owns the native drag operation in `model->flight`.
A zero flight value indicates no active drag.
The flight structure tracks the dragged tab.
The tab remains in the model during the full drag.
A carry window can hold the tab during a drag, or no window holds it.
If a window closes during a drag, the model keeps the dragged tab.
The drag target of each window sets the model as its user data pointer.
A cancelled drag operates like a drop onto the desktop.
The function `app_destroy` destroys the model after the host reaps all windows.

## Tab state and close

The application is the only authority for live tabs.
A tab has a stable identity across reorders and transfers: its tab ID.
A zero ID indicates no tab.
The model never reuses an ID.

Closing a tab changes its logical state before the next frame.
The application selects the next live tab in the same operation.
The application does not select, send input to, preview, or transfer a closing tab.
The live tab count calculation excludes closing tabs.
Closing the last live tab closes the window.

The application can keep an owned label as a display entry for a close animation.
A closing display entry is not a live terminal tab.
The service step of the host pass retires session content.
The service step runs outside every frame, input event, and content callback.
The system never frees content while a callback of that content is on the stack.
Widget animation completion does not decide session destruction.
An application presentation deadline retires the display entry.
The service step first removes each expired entry from its window.
The window then updates its bar and labels, invalidates its layout, and requests a frame.
The model then destroys the terminal content.
A closed window that is not yet reaped keeps its tabs until the reap pass.
The root tick returns the deadline.
A frame or a wake follows the deadline even if the widget is idle.
The service step and the frames of a single pass use the same timestamp.
No frame observes an expired deadline that the service step did not process.
The function `app_destroy` destroys all remaining content after the host reaps the windows.

The application publishes label and selection state immediately after each model change.
The application does not defer tab updates to root prepare.
Widget array capacity and label count are not application state.
The view updates labels immediately when widgets are removed.

## Geometry commit

The laid-out content widget is the only source of terminal geometry.
The application derives the content rectangle from native window bounds, CSS, and tab presentation.
Interfaces pass whole `rect2i` and `vector2i` values.

The view reserves the full CSS tab-bar height while two or more tab entries exist.
A closing display entry keeps this reservation until its presentation ends.
Logical close and tab selection take effect immediately.
The view releases the tab-bar space after the application retires the last closing entry.
The application commits the resulting grid change once, after UI layout.
The content origin follows the visible tab-bar height.
The content slides up as the bar collapses.
The view keeps the reserved content size until the close presentation ends.
The content then expands once at its final origin.
A drag preview can display as an overlay.
Animation progress can change the content origin.
Animation progress does not change content size or the cell grid.
The function `ui_tabbar_steady_height` provides the full CSS height.
This height does not depend on animation progress or widget tab count.

A tab mutation, configure event, restyle, scale change, or zoom invalidates layout.
These paths do not resize the session directly.
After UI layout, `terminal_commit_geometry()` derives the cell grid from the content widget and font metrics.
This call is the only session-resize site in the terminal layer.
Terminal creation uses this grid calculation for initial session size.
An unchanged grid performs no work.
The layer does not use a cached tab-bar height to suppress geometry commits.

Inactive content keeps its last grid until that tab becomes active.
Before a newly selected tab renders or receives input, the application applies current geometry.
The tab does not retain a pending grid computed for a different window or font.
The session owns PTY size notification throttling.
The session keeps the latest pending size and sends the final notification.

## Frame order

Each host pass runs these stages in order:

1. Reap closed windows. Each closed window returns its tabs to the model.
2. Run the application service step.
3. Drain input events for each window.
4. Run the application service step again, with the frame timestamp of the pass.
5. Draw due windows.

The tab model changes during input events, content reports, and the service step.
The tab model never changes during frame drawing.
For the main surface, the application runs these stages in order:

1. Tick and lay out the UI.
2. Commit active terminal geometry.
3. Capture the active terminal snapshot.
4. Build the UI scene and compose the frame.
5. Present the surface and schedule the next frame.

A frame never closes a window and never changes a tab.
The frame reads the active terminal handle once.
The tab service step never opens or closes a window.

During macOS live resize, the platform blocks the main loop, and no normal host pass runs.
The display refresh callback runs a modal service step before it draws.
The platform permits no surface creation or destruction at that time.
A modal service step does not open or close a window.

A frame never uses a snapshot from before a geometry change in that same frame.
If scene preparation changes content bounds, the application skips the frame and retries.
Snapshot capture after layout keeps text-blink scheduling accurate.
The frame pipeline performs one snapshot capture per frame in the established sequence.

## Behavioral guarantees and verification

The tab architecture provides these guarantees:

- Tab creation inherits current working directory and zoom level.
- Tab reorder, native drag, detach, and move operations preserve terminal sessions.
- Theme reload, settings, clipboard, IME, accessibility, and key encoding operate correctly.
- Tab previews and the command palette operate correctly.
- Process exit closes the correct tab after its final output.
- The application renderer and native windows maintain their established ownership.

The automated test suite verifies these behaviors:

- Opening a second tab and closing it restores original shell rows.
- Closing multiple tabs during animations keeps the grid size stable.
- The content origin slides with the bar during collapse animations.
- A geometry resize occurs once after close presentation completes.
- Closing and reopening tabs during animations maintains bar height reservation.
- Selecting an inactive tab after resize or zoom adjusts its grid to content bounds.
- Transferring a tab between windows adjusts its grid to the destination window.

Verification tools enforce architecture rules:

- `python3 tools/dev/dirgraph.py build` validates the include graph with zero violations and zero cycles.
- The UI inspector lint reports zero findings.
- Live resize tests verify both shell row counts and view bounds.
- Screenshots alone do not verify PTY grid correctness.
