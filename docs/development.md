# Development notes

This document contains repository layout, backend, testing, and performance
details for contributors.

For the UI API and a screen example, see [UI library](ui-library-state.md).

## Native windows

yasl owns native windows, the event loop, input, and frame pacing.
`subprojects/yasl/include/yasl/yasl.h` gives its surface and event
contracts.

The window in `src/ui/yasl/window.c` makes one yasl surface for each
window. A menu popup gets a yasl popup surface of its own.
The window keeps the framebuffer history of each surface.

`window_native_draw()` gives the app composer the framebuffer of one surface.
The app composer renders the frame.
It calls `window_present()` with the frame and its damage.
`window_present()` calls `yasl_cpu_present()` to show the frame.

## Shortcut input

The keymap matcher has no event state.
The caller supplies the keymap to the shortcut router.
The matcher ignores Caps Lock and Num Lock.
It prefers the layout character, then the shifted character, then the US key position.
Equal matches use binding order.
Removing a binding preserves the order of the other bindings.

Shortcuts start on a key press.
Only font size and scroll actions repeat.
A repeat cannot start a shortcut after an ordinary key press.
The router uses four bits per key to record its route.
A route records consumption and the allowed repeat action.
A held key cannot change its shortcut action.
It consumes the release even if the modifiers change.
It also consumes repeats when the chord no longer matches.
A fresh press clears a stale route for that key.

The router reports a dropped shortcut when the input queue is full.
The window prints an error for this result.
The key stays consumed so it cannot become ordinary input.
On macOS, text suppression applies only to the current key event.
Committed text outside that event does not inherit shortcut state.

The command palette sends editing shortcuts to the focused UI control.
Copy, paste, and select-all actions work in focused text inputs.
Selectable labels support copy and select-all actions.

## Input flow

The window turns yasl input events into `input_event` values
(`src/ui/yasl/native_input.c`).
The shortcut router queues shortcut events.
It leaves ordinary key queueing to its caller.
File drops carry `text/uri-list` bytes through `INPUT_DROP_FILES`.
The backend does not quote paths for a shell.

The window core updates held keys and the pointer before the UI sees an event.
The UI client routes each event next.
The inspector overlay shortcut comes first, and it ends routing.
The app's command hook gets other shortcuts next. If the hook runs one, routing ends.
Otherwise the UI gets the shortcut.
Keys, text, and paste go straight to the UI.
The key surface takes them. It is the top menu popup, or else the main surface.
A key goes to the focused widget. With nothing focused, the default focus takes it.
In a terminal window, the terminal view is the default focus.
The terminal root takes paste, file drops, shortcuts, and window focus.
A key capture on a root runs before the focused widget. Settings uses one on its root.
A handled key requests no frame by itself. A widget that changes requests a frame through invalidation.
A handled key drops the text that follows it when the key defers its text.
A key handler can keep that text with `ui_surface_keep_key_text()`.

Each text event records its source.
`INPUT_TEXT_KEY` follows its key press or repeat.
All chunks from that key keep the same source.
A key release does not change its text route.
`INPUT_TEXT_COMMIT` is independent of the key route.
`INPUT_TEXT_SHORTCUT` is text from a consumed shortcut.
The window drops that text.
Focus loss clears held keys and the key text route.
A tab change does not release held keys.

Terminal content reads the active program's input modes.
`term_input_encode_key()` and `term_input_encode_mouse()` only encode.
Their status distinguishes unhandled input, consumed input, bytes, and overflow.
Only `TERM_INPUT_BYTES` permits use of the returned buffer slice.
Mouse encoding receives a cell position separately from the input event.
`session_write_bytes()` sends encoded bytes in order.
`session_paste()` filters text and applies bracketed-paste mode.
Terminal content quotes file paths before it pastes them.

The input queue owns copied paste and drop data.
A successful pop gives that ownership to the caller.
Call `input_event_release()` after dispatch.
`input_queue_push_text()` returns the accepted byte count.
Paste and file-drop helpers accept the whole payload or return zero.

## Event loop

The app has one loop, a `yasl_loop`. The app owns it. The PTY rings, the
sessions, the signal watch, the inspector socket, the host timers and the
yasl display all run on it. `app_run()` runs it with `yasl_loop_run()`.

At the first `window_open()`, the window host gives yasl the loop and an
event callback. Each loop step then does these things, in this order:

1. yasl draws the surfaces that are due and arms its display read.
2. The loop waits for a descriptor, a timer or the display.
3. yasl reads the display and gives each event to
   `window_host_dispatch_event()`.
4. Descriptor callbacks run, then the timers that are due, then the
   signal callbacks.

Work schedules itself. Each event, `window_redraw()`,
`window_inject_input()` and `window_close()` arm the host service. The host
service is a zero-delay timer. It frees closed windows, drains input, and
draws the windows that asked for a frame. Thus the loop never waits while
work is pending.

A window can ask for a redraw while it draws, for the next frame of an
animation. Then `window_redraw()` calls `yasl_surface_request_frame()`. That
frame waits until the compositor shows the frame being drawn.

No timer holds a window. A window that wants a frame later calls
`window_wake_at()`. The host keeps one wake timer for all windows, and it
finds the windows when it fires. So a closed window needs no cleanup. The
cursor blink and the visual bell are deadlines that the tick of the terminal
view returns.

A child's exit reaches its tab after the child's last output. On SIGCHLD,
`session_reap_exited()` calls `pty_ring_end()`. The ring reads what the
transports still hold, and reports the end after the terminal has read those
bytes. Only then does the tab close.

When the display is lost and cannot come back, the app quits. A Wayland
compositor that restarts is not such a loss: the windows wait for it.

## Tests

Tests share one link per source library by default.
Each test starts in a separate process.
Test names and binary paths stay the same.
Tests with arguments, stubs, or allocation overrides keep separate binaries.
Set `-Dtest_link_groups=false` to link all tests as separate binaries.
This option keeps LTO enabled.

Grouped tests must use `int main(void)` and return a status explicitly.
Add tests with arguments or symbol overrides to `standalone_tests` in
`tests/meson.build`.

Run the automated regression suite with:

```sh
meson setup build/sanitize --buildtype=debugoptimized -Doptimization=1 \
  -Db_lto=false -Db_lundef=false -Db_sanitize=address,undefined
meson test -C build/sanitize --print-errorlogs
```

This configuration enables AddressSanitizer and UndefinedBehaviorSanitizer.

A preload in the environment can stop a sanitizer build from starting. The
preload shim loads before the sanitizer runtime, and every test then aborts
with "ASan runtime does not come first in initial library list". Clear the
variable for the run:

```sh
env -u LD_PRELOAD meson test -C build/sanitize --print-errorlogs
```

The window regression fixture also requires `wayland-server`,
`wayland-protocols`, and `wayland-scanner`; it starts its own compositor over a
socket pair and does not require a desktop session.
The font suite requires installed CJK, color emoji, and musical-symbol fonts;
Noto Sans CJK, Noto Color Emoji, and Noto Music are suitable choices.

## Performance benchmark

The Linux window backend's design, local idle measurements, and regression
commands are recorded in [window-backend.md](../benchmarks/window-backend.md).

Measure parser/screen throughput with:

```sh
meson test -C build --benchmark term-write-bench --verbose
```

The benchmark feeds repeated ASCII, mixed UTF-8, color, cursor, and erase
sequences to a 120-column, 40-row terminal. Each case warms up with 16 buffers,
then times 512 buffers of approximately 64 KiB. It excludes PTY transport,
rendering, and compositor work.

For the parser refactor, both versions were built with the same driver using
GCC 16.2.1 and `-O3 -march=native` on an AMD Ryzen 7 9700X. Medians of seven
runs per version, alternating which version ran first:

| Workload | Before (MiB/s) | After (MiB/s) | Change |
| --- | ---: | ---: | ---: |
| ASCII | 375.27 | 615.73 | +64.1% |
| Mixed UTF-8 | 454.11 | 515.26 | +13.5% |
| Colors | 716.40 | 705.04 | -1.6% |
| Cursor commands | 765.93 | 818.71 | +6.9% |
| Erase | 170.80 | 170.12 | -0.4% |

These are local synthetic measurements, not end-to-end terminal speedups.
Small differences are within the observed run-to-run variation.

For an end-to-end live profile, run `meson compile -C build term-live-bench` from an active
Wayland session on Linux (requires `perf`) or on macOS (requires `sample(1)`).
The raw or symbolized profile is written under `benchmarks/`.

Grapheme clustering later cost about 4% on the mixed UTF-8 case, measured the
same way against the commit before it, and left the other cases unchanged. Each
non-ASCII character needs its width class looked up once, which the clustering
rules and the cell span share, and the Unicode property tables are consulted
only once the cheaper checks cannot settle the boundary.

## Sanitizer test scopes

The Clang MemorySanitizer build has a dedicated project-owned UI scope:

```sh
meson setup build-msan-clang --wipe \
  --native-file <(printf '[binaries]\\nc = '\''clang'\\''\\ncpp = '\''clang++'\\''\\n') \
  --buildtype=debugoptimized -Doptimization=1 -Db_lto=false \
  -Db_lundef=false -Db_sanitize=memory
meson test -C build-msan-clang --suite msan-safe --print-errorlogs
```

`msan-safe` is an explicit allowlist of core and typed-UI tests. Tests which
initialize fontconfig, Wayland, xcursor, or other third-party libraries remain
in the normal `unit`/`extlib` suites because those dependencies are commonly
not built with MemorySanitizer instrumentation. They are not silently excluded
from ordinary test runs, and a project-owned test must be added to `msan-safe`
only after it passes without suppressing project reports.

`ui-debug-test` is not in `msan-safe`. It serializes the widget tree with
yyjson, and the installed yyjson reports its own pool as uninitialized before
the test does any work. MemorySanitizer takes no function name suppression, so
the test stays out for the same reason the fontconfig and Wayland tests do.

### ThreadSanitizer

Every terminal call enters the terminal's writer section, a spin lock.
`term-test` feeds on one thread while another calls every public reader, so a
call that skips the section is reported.

`tests/tsan.supp` names GLib finalizers only. GLib is not built with TSan,
so TSan cannot see the atomic reference counts that order those frees.
Project code uses pthread locks where it shares state with a GLib thread,
because TSan cannot see a `GMutex` either.

```sh
printf "[binaries]\nc = 'clang'\ncpp = 'clang++'\n" > build-tsan.ini
meson setup build-tsan --native-file build-tsan.ini \
  --buildtype=debugoptimized -Doptimization=1 -Db_lto=false \
  -Db_lundef=false -Db_sanitize=thread
meson test -C build-tsan --timeout-multiplier 4 --print-errorlogs
```

Write the machine file to a real path. A process substitution is not read
correctly here.

The routine suite runs `write-fuzz-test` with 1,000 seeds.
Each seed keeps the same parser and screen checks.
Set `-Dwrite_fuzz_seeds=50000` for the full workload.
The standalone binary defaults to 50,000 seeds.
`make test-fuzz` builds the test and runs 50,000 seeds without a timeout.
It does not change the seed count for the routine suite.
Long fuzz runs can need the timeout multiplier under a sanitizer.
