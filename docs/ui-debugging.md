# UI debugging and inspection

The UI inspector shows what the widget tree is, what it painted, and where
the painted result differs from what the widgets think they laid out. It
has three front ends that share one implementation:

| Front end | Use it to | Needs |
| --- | --- | --- |
| Overlay (`Ctrl+Shift+F12`) | See bounds, box model, parts, and lint marks on the live window | Any build |
| Socket and `scripts/wsap-debug` | Script inspection, screenshots, and input, also headless | `--debug-socket PATH` |
| `scripts/gdb_ui_inspector.py` | Inspect a stopped process or a core file | A build with debug info |

The code is in two layers:

- `src/ui/debug.h` is part of the UI library. It reads a widget tree and
  a built scene with its scopes. It writes JSON, runs the alignment
  lint, and draws the overlay. It has no window or platform dependencies.
- `src/ui/yasl/inspector.h` is the window side, in three parts: `debug.c`
  keeps a copy of the last scene of each window surface, the overlay, and
  the injected input; `debug_cmd.c` runs the protocol commands;
  `debug_server.c` serves the socket and answers frame waits and PNG
  screenshots. A captured scene owns its buffers, so building the next
  frame does not change what the inspector reports.

The inspector is always compiled in. It does nothing until you start the
socket or turn on the overlay.

## Quick start

```sh
build/bin/wsap_term --debug-socket /tmp/wsap.sock &
export WSAP_DEBUG_SOCKET=/tmp/wsap.sock
scripts/wsap-debug tree --depth 1
scripts/wsap-debug lint
scripts/wsap-debug overlay on
scripts/wsap-debug screenshot /tmp/window.png
```

`scripts/wsap-debug` prints the `result` of each reply as JSON. Chain
commands with `;`. Without a command, it reads commands from standard
input, one per line.

### Headless runs

The inspector does not need a desktop session. Weston's headless backend
is sufficient, and injected input does not need a seat:

```sh
weston --backend=headless --renderer=pixman --width=1400 --height=900 \
       --socket=wsap-headless --idle-time=0 &
WAYLAND_DISPLAY=wsap-headless build/bin/wsap_term --debug-socket /tmp/wsap.sock &
scripts/wsap-debug shortcut new_tab ";" frame ";" screenshot /tmp/tabs.png
```

Menus open inline on a headless compositor, because native popups need a
seat. Use a real session to inspect native popups with `-p LEVEL`.

## Concepts

### Surfaces

Each window has one main surface and one surface for each native menu
popup level. Commands use the main surface. Add `-p L` to use popup
level `L`, and `-w N` to use window `N` from `windows`.

### Selectors

Commands that take a widget accept:

| Selector | Meaning |
| --- | --- |
| `/` | The root of the surface |
| `/1/0` | The path of child indexes from the root |
| `tabbar` | The first widget, in paint order, with this type name |
| `.class`, `#id` | The first widget with this style class or id |
| `0x55d0…` | A widget address. The inspector accepts it only when the widget is in the tree. |

Type names come from `ui_widget_ops.name`. Paths follow paint order.
For siblings, the last child paints on top.

### The window tree

The window root links the content's root and the inline menu levels as
children, in paint order. For a terminal window:

```text
/            window-root
/0           terminal-root
/0/0         tabbar
/0/1         scrollview     (of the active tab; its bar is the track)
/0/1/0       terminal-view
/0/2         tab-preview    (while shown)
/0/3         palette
/1 …         menu-level     (inline levels only)
```

The framework paints children in tree order.
It targets pointer input in the reverse order.
Root preparation updates content before style and layout.
A window with a UI mount has the mounted root (`window_ui_spec.root`) at `/0`.
A native popup has a `menu-popup-root` at `/`. Its menu-level widget is `/0`.

The UI surface sends key events and committed text to the focused widget.
Committed text uses `UI_EVENT_TEXT`. Paste uses `UI_EVENT_PASTE`. An unhandled
Tab moves focus. Shift+Tab moves focus back. Pointer input targets the top
visible widget. Unhandled pointer events bubble to its ancestors. Actions also
bubble to ancestors.

### Parts

Some widgets lay out sub-rects without child widgets. Tabs, close
buttons, menu items, list rows, and the scrollview bar are examples.
These widgets report the sub-rects through `ui_widget_ops.debug_parts`,
in the frame that they hit-test in. A part that has a name other than
`content` is a hit region. A widget with hit regions occupies only those
regions for `hit` and for the overlay. This rule is necessary because the
tabbar paints into a canvas the size of the window.

`content` is the box-model content rect. `ui_box` reports it.

### Scene scopes

Every scene records which widget emitted each op. Each widget paints in a
scope. A widget can split its ops into parts, such as one tab or one row.
The framework gives each child its own scope. Widget paint callbacks draw
only their own content. The inspector does not dereference a scope owner that
is no longer in the tree.

Scopes also drive damage. A scope hashes its own ops and records the
pixels they touch. `ui_damage_track()` compares the scopes of two frames
and damages the old and the new pixels of each scope that changed.

### Coordinates

Widget bounds and event positions are in surface space. A widget paints,
and its own API speaks, in its local frame, with the origin at the top
left of its bounds. Use `ui_widget_to_local()` and `ui_widget_to_surface()`
to convert. Never add `bounds.pos` by hand.

### Damage

`scripts/wsap-damage-probe [OUTDIR]` runs a private instance on headless
weston and prints the Wayland damage of every commit, for an idle period
and for tab title changes. An idle frame should commit no damage or only
the cursor row. A title change should damage only its tab.

### Text boxes

The renderer draws a text op at the origin of the op's rect, in a cell box
of `cells × advance` by `cell_h`. It ignores the rest of the rect. The
inspector, the lint, and the overlay use that cell box. The `rect` field
in scene dumps is what the widget asked for. The `box` field is what the
renderer paints.

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

Options for any command: `-w N` (window), `-p L` (popup level), and
`-m MODS` (modifiers for input, for example `shift+ctrl`). A command on a
popup level that is not open fails at once.

In a `;` chain, `-w N` applies only to the command it stands in, so repeat it
after each `;` to target another window.

Each line gets exactly one reply line:
`{"ok":true,"result":…}` or `{"ok":false,"error":"…"}`. Replies come in
request order. A `frame` or `screenshot` reply waits for the frame, and
the lines after it wait too. A wait fails after three seconds.

Injected input goes through `window_drain_input()`, the same path as
native input. Key chords go through the active keymap, so a bound chord
fires its shortcut. The injection queue grows as necessary, so a long
`type` loses no input. Run `frame` after input before you inspect the
result.

## The overlay

`overlay` takes these arguments:

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

The overlay appends ops after the UI's own ops, so it does not change them.
When the scene is full, the overlay stops and the scene stays valid.

## The alignment lint

The lint examines the text ops of the last frame. For each run, it finds
the surface that the text sits on:

1. The smallest hit part of the owner that contains the start of the text.
2. Else, the nearest earlier fill op of the owner that contains it.
3. Else, the owner's bounds.

A surface qualifies only if its height is between one and three cell
heights. A panel background is therefore never taken as a row.

| Finding | Meaning |
| --- | --- |
| `v-center` | The cell box is more than 1 px off vertical center in its surface. `offset` is positive when the text is too low. |
| `overflow` | The cell box extends past its surface by `offset` px |
| `row` | Two text runs of one widget share a row, but their centers differ by more than 1 px |

The lint does not examine horizontal alignment, because left-aligned text
is correct. Use `widget SEL` and compare the text `box` with the parts.

## gdb

```sh
gdb -ex "source scripts/gdb_ui_inspector.py" build-debug/bin/wsap_term
(gdb) ui-cmd tree --depth 1
(gdb) ui-cmd -p 0 lint
(gdb) ui-tree
(gdb) ui-widget &((struct terminal *)w->ui.spec.content)->tabbar.widget
```

A window embeds its UI state as `w->ui`, a `struct window_ui`. Its
`spec.content` is the pointer that `window_ui_content()` returns.

`ui-cmd` calls `window_debug_eval()` in the stopped process. The socket
server does not have to run. Input injection, `frame`, and `screenshot`
are refused, because a stopped process does not draw. `ui-tree` and
`ui-widget` only read memory, so they also work on core files. They start
from `window_ui_host_last`.

A release build has no debug info. Configure a separate build directory
with `--buildtype=debug` or `debugoptimized` for gdb.

## Tests

Use the inspector as the oracle for layout regressions, not pixel
positions copied from the implementation. `tests/ui/widgets/list_test.c`
builds a list and asserts `ui_debug_lint_scene() == 0`.
`tests/ui/yasl/debug_server_test.c` covers the protocol.
`tests/ui/debug_test.c` covers selectors, hit testing, op attribution,
the lint, and overlay safety.

## Security

The socket accepts input injection, so a client can type commands into
the shells of the terminal. The server:

- creates the socket with mode `0600`,
- accepts only peers with the same user id,
- refuses to replace a path that is not a stale socket of the same user.

Do not put the socket in a directory that other users can write to.

## Limits

- A scene capture holds the last frame of each surface. After input, run
  `frame` before you inspect.
- A scene records up to 512 scopes. `scene` reports `untracked` when a
  frame has more. Such a frame damages the whole surface.
- The palette and menu levels compose their input and list internally.
  The inspector shows them as parts, not as child widgets.
- The inspector reads CPU pixels.

## Other tools

`scripts/gdb_buffer_inspector.py` traces Wayland buffer contents, damage
rectangles, and pixel values across frame commits. `AGENTS.md` shows how
to use it. The old `scripts/gdb_layout_inspector.py` and the
`WSAP_DEBUG_LAYOUT*` variables targeted the removed `ui_node` tree.
