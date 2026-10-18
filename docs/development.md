# Development notes

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

| Command | Scope |
| --- | --- |
| `make test` | Regular host tests in `$(BUILD_DIR)-test`. |
| `make test-asan` | Address, leak, and undefined behavior checks. |
| `make test-tsan` | Thread checks. |
| `make test-msan` | Memory checks without the `extlib` suite. |
| `make test-sanitizers` | All sanitizer targets. |
| `make test-fuzz` | 50,000 parser fuzz seeds. |
| `meson test -C build --suite isa --print-errorlogs` | ISA tests. |

Linux tests need Wayland server libraries, protocol files, and `wayland-scanner`.
`make test` also enables Sway and Kitty tests on Linux.
The Sway suite needs `swaymsg` and `grim`.
Font tests need JetBrains Mono, CJK, color emoji, and musical symbol fonts.
JetBrains Mono is the reference family. Without it, some font checks fail or skip.
The font tests set slight hinting and grayscale antialiasing.
They also check the raster invariants with RGB subpixel antialiasing.
The render tests set grayscale antialiasing.
The fontconfig settings of the host do not change the results.

`python3 tools/test-matrix.py` runs sanitizer checks across CPU targets.
On x86-64, the default targets are native, x86-64-v2, x86-64-v3, and znver5.
The CPU and compiler must support each target.
Use `--isas` for native only.

## Benchmarks

Run benchmarks from an optimized build without sanitizers.

The app benchmarks share clocks and sample statistics in `benchmarks/bench.h`.
`BENCH_RUN` runs one warmup batch and seven timed batches.
The workload stays inline. It needs no callback or context object.
The UI microbenchmarks report nanoseconds per operation.
Their layout case runs 1,000 passes per batch.
The frame benchmark keeps its four scenarios and per-scenario limits.
Reports use the upper middle sample for even sample counts.

The term benchmarks use `subprojects/term/benchmarks/bench.h`.
This keeps the term project independent of the app.
The term benchmarks keep their existing percentile rules.

`termbench` is a C program. Build it with `meson compile -C build termbench`.
It prepares text and 256 changing color frames before it times the writes.
Each case writes 64 MiB for `small`, 1 GiB for `normal`, or 16 GiB for `large`.
Each write request is at most 64 KiB.
The timer stops when the last write returns.
The terminal's 1 MiB pipe can still hold data at that time.
Keep each size much larger than that pipe.
The report version is `termbench-v3`.
The workloads and byte counts differ from TermMarkV2. Record new baselines.

```sh
meson test -C build --benchmark term-write-bench --verbose
meson test -C build --benchmark render-bench --verbose
meson compile -C build term-live-bench
```

The live profile needs a desktop session.
It uses `perf` on Linux and `sample` on macOS.
Reports go in `benchmarks/`.

| Tool | Purpose |
| --- | --- |
| `benchmarks/run-termbench.sh [size] [test]` | Run termbench in the terminal. |
| `benchmarks/run-termbench-headless.sh` | Compare builds on one headless Linux host. |
| `tools/ab.sh CASE N BIN_A BIN_B` | Alternate benchmark runs and report medians. |
| `tools/analyze.sh` | Run static checks. |
| `python3 tools/dev/dirgraph.py build` | Check library dependencies. |
| `python3 tools/loc_per_function.py src` | Count code lines per function. |
| `python3 tools/dev/one_liners.py build` | List one-line functions and how code uses them. |
| `python3 tools/build_macos_icon.py` | Build the macOS icon with resvg and iconutil. |

See [performance reports](../benchmarks/) for results and methods.

## Sanitizers

Sanitizer targets use separate build directories.
They clear `LD_PRELOAD` and `DYLD_INSERT_LIBRARIES` before each run.
The preload shim and its exec helper remain unsanitized.

For a direct MemorySanitizer run:

```sh
meson test -C build-msan-clang --suite msan-safe --print-errorlogs
```

`msan-safe` lists core and UI tests that pass without suppressions.
Tests that use uninstrumented system libraries stay in `unit` and `extlib`.
`ui-debug-test` uses uninstrumented yyjson and stays outside `msan-safe`.

`tests/tsan.supp` suppresses GLib finalizers.
`tests/asan.supp` suppresses the macOS CoreGraphics PDF lexer overread.
Keep suppressions limited to system code.

The regular suite runs `write-fuzz-test` with 1,000 seeds.
Set `-Dwrite_fuzz_seeds=50000` for the full workload.
`make test-fuzz` runs 50,000 seeds without a timeout.
