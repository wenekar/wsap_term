# UI debugging and inspection

The UI inspector shows the widget tree.
It shows the painted operations.
It shows differences between the painted result and the widget layout.
Three front ends share one implementation:

| Front end | Use it to | Needs |
| --- | --- | --- |
| Overlay (`Ctrl+Shift+F12`) | See bounds, box model, parts, and lint marks on the live window | Any build |
| Socket and `scripts/wsap-debug` | Script inspection, screenshots, and input, also headless | `--debug-socket PATH` |
| `scripts/gdb_ui_inspector.py` | Inspect a stopped process or a core file | A build with debug info |

The code has two layers:

- `src/ui/debug.h` is part of the UI library. It reads a widget tree and
  a built scene with its scopes. It writes JSON. It runs the alignment
  lint. It draws the overlay. It has no window or platform dependencies.
- `src/ui/yasl/inspector.h` is the window layer with three components:
  `debug.c` stores the last scene of each window surface, the overlay,
  and injected input. `debug_cmd.c` runs protocol commands.
  `debug_server.c` serves the socket, answers frame waits, and creates
  PNG screenshots. A captured scene owns its buffers. Building the next
  frame does not change what the inspector reports.

The build always includes the inspector.
The inspector stays idle until you start the socket or activate the overlay.

When the app loads a CSS file, it reports an unknown property, invalid value,
invalid syntax, or unsupported pseudo-class with the file path and location.
The parser keeps the other valid declarations.
Use `css_parse_with_diagnostics()` to collect these messages in a UI client.
The callback receives borrowed strings during the call.

## Quick start

```sh
build/bin/wsap_term --debug-socket /tmp/wsap.sock &
export WSAP_DEBUG_SOCKET=/tmp/wsap.sock
scripts/wsap-debug tree --depth 1
scripts/wsap-debug lint
scripts/wsap-debug overlay on
scripts/wsap-debug screenshot /tmp/window.png
```

`scripts/wsap-debug` prints the `result` of each reply as JSON.
Chain commands with `;`.
Without a command argument, the script reads commands from standard input.
Enter one command per line.

### Headless runs

The inspector does not need a desktop session.
The headless backend of Weston is sufficient.
Injected input does not need an input seat:

```sh
weston --backend=headless --renderer=pixman --width=1400 --height=900 \
       --socket=wsap-headless --idle-time=0 &
WAYLAND_DISPLAY=wsap-headless build/bin/wsap_term --debug-socket /tmp/wsap.sock &
scripts/wsap-debug shortcut new_tab ";" frame ";" screenshot /tmp/tabs.png
```

Native popups require an input seat.
For this reason, menus open inline on a headless compositor.
Use a desktop session to inspect native popups with `-p LEVEL`.

## Concepts

### Surfaces

Each window has one main surface.
Each native menu popup level has its own surface.
Commands target the main surface by default.
Add `-p L` to target popup level `L`.
Add `-w N` to target window `N` from `windows`.

### Selectors

Commands that take a widget accept these selectors:

| Selector | Meaning |
| --- | --- |
| `/` | The root of the surface |
| `/1/0` | The path of child indexes from the root |
| `tabbar` | The first widget, in paint order, with this type name |
| `.class`, `#id` | The first widget with this style class or id |
| `@key` | The first widget with this inspector key. View nodes use their descriptor key. |
| `0x55d0…` | A widget address. The inspector accepts it only when the widget is in the tree. |

Type names come from `ui_widget_ops.name`.
Paths follow paint order.
For sibling widgets, the last child paints on top.

### The window tree

The window root links the content root and inline menu levels as children.
The links follow paint order.
This tree shows a terminal window:

```text
/            window-root
/0           app-tabs-root
/0/0         scrollview     (of the active content; its bar is the track)
/0/0/0       terminal-view
/0/1         tabbar
/0/2         tab-preview    (while shown)
/0/3         palette
/1 …         menu-level     (inline levels only)
```

The content paints first.
The tab bar paints after the content.
The bar covers the content when the bar collapses as an overlay.

The framework paints child widgets in tree order.
It targets pointer input in reverse order.
Root preparation updates content before style and layout.
A window with a UI mount places the mounted root (`window_ui_spec.root`) at `/0`.
A native popup places `menu-popup-root` at `/`.
The menu-level widget of the popup is `/0`.

The UI surface sends key events and committed text to the focused widget.
Committed text uses `UI_EVENT_TEXT`.
Paste uses `UI_EVENT_PASTE`.
An unhandled Tab key moves focus forward.
Shift+Tab moves focus backward.
Pointer input targets the topmost visible widget.
Unhandled pointer events bubble to widget ancestors.
Actions also bubble to widget ancestors.

### Parts

Some widgets lay out sub-rectangles without child widgets.
Examples include tabs, close buttons, menu items, list rows, and the scroll bar.
These widgets report sub-rectangles through `ui_widget_ops.debug_parts`.
They report them in the frame used for hit testing.
A part with a name other than `content` is a hit region.
A widget with hit regions occupies only those regions for `hit` and for the overlay.
This rule is necessary.
The tab bar paints into a canvas of window size.

`content` is the box-model content rectangle.
`ui_box` reports this rectangle.

### Scene scopes

Every scene records which widget emitted each operation.
Each widget paints inside a scope.
A widget can split operations into parts, such as one tab or one row.
The framework gives each child widget its own scope.
Widget paint callbacks draw only their own content.
The inspector does not dereference a scope owner removed from the tree.

Scopes also control redraw damage.
A scope hashes its own operations.
It records the pixels that those operations touch.
`ui_damage_track()` compares scopes between two frames.
It marks the old and new pixels of each changed scope as damaged.

### Coordinates

Widget bounds and event positions use surface space.
A widget paints in its local frame.
The local origin sits at the top left of the widget bounds.
Widget APIs use this local frame.
Use `ui_widget_to_local()` to convert a position to the local frame.
Use `ui_widget_rect_to_surface()` to convert a rectangle to surface space.
Do not add `bounds.pos` manually.

### Damage

`scripts/wsap-damage-probe [OUTDIR]` runs a private instance on headless Weston.
It prints the Wayland damage of each commit during an idle period and during tab title changes.
An idle frame commits no damage or only the cursor row.
A tab title change damages only its own tab.

### Text boxes

The renderer draws a text operation at the origin of the operation rectangle.
It draws the text in a cell box of size `cells × advance` by `cell_h`.
It ignores the remaining area of the rectangle.
The inspector, lint check, and overlay use that cell box.
In scene dumps, the `rect` field shows the rectangle requested by the widget.
The `box` field shows the area that the renderer paints.

## Commands

| Command | Result |
| --- | --- |
| `help` | The command list |
| `windows` | Each window: index, size, tabs, and menu and palette state |
| `state` | Pointer, hover zone, focused and captured widgets, tab titles |
| `tree [SEL] [--depth N]` | The widget tree: type, path, bounds, measured size, flags, dirty work, op range |
| `widget SEL` | One widget: resolved style, parts, and the text runs it painted |
| `hit X Y` | The visible widget and parts at a point, and the topmost ops there |
| `scene [SEL]` | All ops of the last frame, or the ops of one subtree, each with its owner |
| `lint` | Alignment findings in the last frame |
| `overlay ARGS` | Turn the overlay on or off, or change it |
| `frame` | Reply after the next frame of the surface |
| `screenshot PATH.png` | Reply after the next frame, with the frame written as PNG |
| `key CHORD…` | Press and release each chord, for example `ctrl+shift+p`, `escape`, `down` |
| `type TEXT…` | One key press per character |
| `text TEXT…` | Text as an input method commits it |
| `move`, `down`, `up`, `click X Y [BUTTON]` | Pointer events. `BUTTON` is `left`, `middle`, or `right`. |
| `scroll X Y LINES` | A wheel event |
| `leave` | The pointer leaves the surface |
| `shortcut ACTION` | A keybind action, for example `new_tab` or `command_palette` |

Any command accepts these options:
- `-w N`: select window `N`.
- `-p L`: select popup level `L`.
- `-m MODS`: input modifiers, for example `shift+ctrl`.

A command fails immediately if the targeted popup level is not open.

In a `;` chain, `-w N` applies only to its own command.
Repeat `-w N` after each `;` to target that window in subsequent commands.

Each request line produces one reply line:
`{"ok":true,"result":…}` or `{"ok":false,"error":"…"}`.
Replies arrive in request order.
A `frame` or `screenshot` command waits for the next frame.
Subsequent lines wait for that frame as well.
A wait times out after three seconds.

Injected input passes through `window_drain_input()`.
This is the same path used by native input.
Key chords use the active keymap.
A bound chord executes its configured shortcut.
The injection queue expands as needed.
A long `type` command loses no input characters.
Run `frame` after input before you inspect the result.

## The overlay

The `overlay` command accepts these arguments:

| Argument | Effect |
| --- | --- |
| `on`, `default` | Bounds, box, parts, label, and lint |
| `all` | Also text boxes |
| `off`, `toggle` | Turn it off, or switch it |
| `bounds`, `box`, `text`, `parts`, `label`, `lint` | Set only these flags (several are allowed) |
| `+FLAG`, `-FLAG` | Add or remove one flag |
| `select SEL`, `select none` | Show the box model for a fixed widget instead of the widget under the pointer |

Colors:

| Color | Meaning |
| --- | --- |
| Blue, orange, green, and violet outlines | Widget bounds, by tree depth |
| Orange, green, and blue bands | Margin, padding, and content of the focus widget |
| Yellow outlines | Parts of the focus widget. The part under the pointer is thicker. |
| Cyan | Text cell boxes (`text`) |
| Magenta | A text op's requested rect, when it is larger than the cell box |
| Red | Lint findings, with the surface that the text sits on |

The overlay appends operations after the UI operations.
It does not modify UI operations.
If the scene reaches capacity, the overlay stops.
The scene remains valid.

## The alignment lint

The lint examines text operations from the last frame.
For each text run, it locates the supporting surface in this order:

1. The smallest hit part of the owner that contains the text start.
2. The nearest earlier fill operation of the owner that contains the text start.
3. The bounds of the owner.

A surface qualifies only when its height is between one and three cell heights.
The lint never treats a panel background as a row.

| Finding | Meaning |
| --- | --- |
| `v-center` | The cell box is more than 1 px off vertical center in its surface. `offset` is positive when the text is too low. |
| `overflow` | The cell box extends past its surface by `offset` px |
| `row` | Two text runs of one widget share a row, but their centers differ by more than 1 px |

The lint does not check horizontal alignment.
Left alignment is normal and correct.
Use `widget SEL` to compare the text `box` with widget parts.

## gdb

```sh
gdb -ex "source scripts/gdb_ui_inspector.py" build-debug/bin/wsap_term
(gdb) ui-cmd tree --depth 1
(gdb) ui-cmd -p 0 lint
(gdb) ui-tree
(gdb) ui-widget &((struct terminal *)w->ui.spec.content)->tabbar.widget
```

A window embeds its UI state in `w->ui` as a `struct window_ui`.
The field `spec.content` holds the pointer returned by `window_ui_content()`.

`ui-cmd` calls `window_debug_eval()` inside the stopped process.
The socket server does not need to run.
A stopped process does not render frames.
The server therefore rejects input injection, `frame`, and `screenshot`.
`ui-tree` and `ui-widget` only read memory.
They also work on core dump files.
They start from `window_ui_host_last`.

A release build lacks debug information.
Configure a separate build directory with `--buildtype=debug` or `debugoptimized` for GDB.

## Tests

Use the inspector as the oracle for layout regression tests.
Do not copy pixel positions from the implementation into tests.
`tests/ui/widgets/list_test.c` builds a list and asserts `ui_debug_lint_scene() == 0`.
`tests/ui/yasl/debug_server_test.c` covers the protocol.
`tests/ui/debug_test.c` covers selectors, hit testing, operation attribution, the lint, and overlay safety.

## Security

The socket accepts input injection.
A client can execute commands in terminal shells.
The server enforces these security protections:

- It creates the socket with file mode `0600`.
- It accepts only peer processes with the same user ID.
- It refuses to replace a file path unless that path is a stale socket owned by the same user.

Do not place the socket in a directory where other users have write access.

## Limits

- A scene capture holds the last frame of each surface.
  After input injection, run `frame` before you inspect.
- A scene records up to 512 scopes.
  The `scene` command reports `untracked` when a frame exceeds this limit.
  A frame with more than 512 scopes damages the whole surface.
- The palette and menu levels compose input and list structures internally.
  The inspector displays them as parts, not as child widgets.
- The inspector reads CPU pixels.

## Other tools

`scripts/gdb_buffer_inspector.py` traces Wayland buffer contents, damage rectangles, and pixel values across frame commits.
See `AGENTS.md` for usage instructions.
