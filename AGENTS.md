# Repository instructions

Write docs, comments, and summaries in ASD-STE100 style: short sentences,
simple words, active voice, one instruction or fact per sentence.

## Layout and conventions

The rules that constrain new code:

- Everything shipped as data rather than compiled lives in `assets/` — themes,
  terminfo, shell integration, icons, and the `.desktop` entry. Do not add data
  files at the repository root. Source paths there are independent of install
  paths; changing one does not change the other.
- `tests/` mirrors the `src/` subsystem layout (`tests/pty/`, `tests/ui/`,
  …), with `tests/stubs/` and `tests/helpers/` as the non-subsystem
  exceptions. Test *names* in `tests/meson.build` stay
  flat and carry no directory prefix, so a file move never changes how a test
  is invoked. Keep it that way.
- Source directories form a tree. Each directory lists its own sources in
  its `meson.build` and calls `subdir()` for its children. `src/meson.build`
  only links the libraries, lowest layer first. A new module goes under the
  directory that owns it. Do not add a sibling that splits an existing
  subsystem.
- yasl (`subprojects/yasl`) is the platform layer. `src/ui/` is the UI core.
  The UI core includes no yasl header and has no yasl dependency.
  `src/ui/yasl/` is the binding library, `wsap-ui-yasl`. It joins the UI to
  yasl: the window and its host, native input, menu popups, accessibility,
  and the inspector server. Only base, pty, session, ui_yasl, terminal, and
  app can include `yasl/` headers. `dirgraph.py` checks this rule.
- Window content lives in its own directory, such as `src/terminal/`.
  Content mounts its widgets with `window_ui_mount`. Producers send frame
  data to the app composer. Only `src/app/` opens app windows.
- The term core is the subproject `subprojects/term`, the lowest layer
  after base. It builds, tests, and benchmarks alone, and it includes
  nothing from the main tree. Its public headers are in `include/wsap_vtcore/`:
  `wsap_vtcore.h` and the header-only `unicode/` (the UTF-8 codec and the Unicode
  tables). The main tree gets only those, through `term_dep`. Its tests
  and benchmarks live in the subproject and run as `term:<name>`.
  `subprojects/term/README.md` describes embedding and the amalgamation.
- The library order is base, term, pty, session, input, paint, font, ui,
  theme, render, ui_yasl, terminal, and app. A library can use only its
  declared lower layers. Run `python3 tools/dev/dirgraph.py build` to check
  the include graph. The check must report no violations or cycles.
- `base_lib` owns `util/`, `platform/`, and `loop/`. This is the only library
  that spans these three source directories.
- `paint/` and `session/` are shared modules. They are exceptions to the
  single-owner directory rule. Paint holds data that UI and render share.
  Session joins term and pty.
- The app owns one renderer. The window keeps the framebuffer history of
  each native surface. Render accepts one `render_frame`. Only app calls
  `render_draw`. The window presents a framebuffer and its damage region.
  ui_yasl includes no render header.
- Themes live in `src/theme/`. The app owns the theme store.
  The window UI API is `src/ui/yasl/client.h`.
  The window inspector API is `src/ui/yasl/inspector.h`.
- Integer geometry uses `struct vector2i` and `struct rect2i` from
  `src/util/geom.h`. Pass positions and sizes as whole vectors across API
  boundaries rather than as loose `int x, int y` pairs, and prefer the
  `vector2i_*`/`rect2i_*` helpers over writing the same expression once per
  axis. `rect2i` aliases `pos`/`size` onto `x`/`y`/`width`/`height` through
  anonymous unions, so it must be initialized with designated initializers —
  positional `{0, 0, w, h}` trips `-Wmissing-braces`. The unions are union
  type-punning, which is valid C and undefined in C++: keep `geom.h` out of any
  C++ translation unit.
- The standalone term API uses `struct wsap_vtcore_vector2i`. This is an exception
  to the geometry rule. The term library has no util dependency.
- Zero Is Initialization. Design types so an all-zero value is valid and
  meaningful, and rely on it rather than writing explicit zero fields.
- Prefer driving appearance from CSS over hardcoded constants. Widget styling
  resolves through `src/ui/css.c`; a new visual property belongs there with a
  C-side fallback, not as a literal in the widget.

## Testing

Write tests for observable behavior and stable public interfaces, not internal
implementation details. Tests should remain useful when the implementation is
refactored without changing its behavior.

- Prefer representative scenarios and externally observable regression cases.
- Avoid tests that mirror implementation logic, call private helpers directly,
  or assert internal state, bookkeeping, or a particular call sequence.
- Add tests when they provide meaningful coverage of a behavior or bug; do not
  add them automatically for every edit or duplicate existing coverage.
- If a refactor breaks a test while preserving public behavior, reconsider the
  test's coupling to the implementation rather than preserving internals for it.

## Performance work

Throughput targets (TermMark/termbench, `cat /dev/random`) require
assembly-level optimizations; scalar code does not get there. When working on
the input or render hot paths:

- Write explicit SIMD for the hot loops: AVX-512 and AVX2 on x86, NEON on
  aarch64, keeping the portable scalar path as fallback. See
  `scan_ascii_run`, `put_ascii_chunk`, and `clear_row` in `subprojects/term/src/` for the
  established pattern (widen inputs, permute/zip into the cell layout, block
  stores; fold range checks into a single unsigned subtract+compare).
- The term core compiles once and picks its x86 level at run time. Do not
  test `__AVX2__` or `__AVX512*__` in `subprojects/term/`. Put each AVX2 or AVX-512
  path in its own function marked `TERM_TARGET_AVX2` or `TERM_TARGET_AVX512`.
  Call it only when `term_use_avx2()` or `term_use_avx512()` is true.
  `subprojects/term/src/simd.h` states the rules. Give a kernel only scalar and pointer
  parameters. Keep inputs shorter than one vector on the inline base path,
  because a call costs more than a short scan. For a large function with
  kernels in its loop, write one always-inline body and one target wrapper
  per level, as `bulk_ascii_lines_body` and `text_run_body` do. The kernels
  then inline into each wrapper.
- Auto-vectorized loops do not survive this: the shared code compiles for
  base x86-64. Write a kernel for any loop that must run at vector speed,
  as `scan_byte_run` does for line feed bursts.
- Keep per-iteration state in registers and batch bookkeeping: hoist
  `cursor_x`, defer `row_len`/`row_gen` updates to the end of a run, and fuse
  known sequence pairs (e.g. `38;2;`/`48;2;` SGR) in the CSI fast path.
- Measure with `meson compile -C build term-live-bench` (samples the
  live terminal), and the headless benchmarks:
  `meson test -C build --benchmark term-write-bench --verbose` and
  `meson test -C build --benchmark render-bench --verbose`.
  `benchmarks/*.md` documents previous passes and their methodology.
- Watch the transport too: kernel PTY reads are small (and `FIONREAD` reports
  0 on macOS PTY masters), so output floods must be detected by drain-loop
  behavior, not ioctls.
- Never make what a cell shows depend on load or timing. Shaping cost is
  bounded by the layout analysis in `src/font/glyph/layout.c` and the shaping
  windows in `src/font/glyph/run.c`, not by skipping shaping. Measure floods
  with the `flood-*` cases of `render-bench` and `metal-render-bench`.
- On the SDL/macOS backend, never issue `SDL_UpdateTexture` per item during a
  frame — each call flushes SDL's Metal command queue. Stage into a CPU copy
  and upload one dirty rect per frame before drawing.

## Debugging and tools

- `scripts/gdb_buffer_inspector.py`: GDB Python script. It traces and inspects
  Wayland buffer contents, damage rectangles, and pixel values across frame
  commits:
  `gdb -ex "source scripts/gdb_buffer_inspector.py" -ex "run" build/bin/wsap_term`

- `scripts/wayland-compositors` lists the Wayland compositors on the machine.
  It tells which ones accept a client. Use it to find the live session for a
  live test, for example over SSH, where `WAYLAND_DISPLAY` is not set.
  `--env NAME` prints `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` of the first
  match: `env $(scripts/wayland-compositors --env kwin) build/bin/wsap_term`.
  Headless weston and sway tests do not need it.

- The UI inspector (`src/ui/debug.h`, `src/ui/yasl/inspector.h`) is the tool for
  layout and alignment work. `docs/ui-debugging.md` has the full reference.
  - Start the app with `--debug-socket PATH` or `WSAP_DEBUG_SOCKET=PATH`.
    Then use `scripts/wsap-debug` to dump the widget tree, hit-test a point,
    list scene ops, run the alignment lint, draw the overlay, take PNG
    screenshots, and inject keys, text, pointer events, and shortcuts.
  - It works headless: run a `weston --backend=headless` compositor and
    point `WAYLAND_DISPLAY` at it. Injected input needs no seat.
  - `Ctrl+Shift+F12` (`debug_overlay`) toggles the overlay in any build.
  - `scripts/gdb_ui_inspector.py` adds `ui-cmd` (the socket protocol,
    in-process) and `ui-tree`/`ui-widget` (also on core files).
- Check a UI change with the inspector before you call it done: `lint` must
  report no findings for the screens you touched. For a regression test,
  prefer `ui_debug_lint_scene()` or the debug parts as the oracle over pixel
  positions copied from the code.
- A widget that lays out sub-rects without child widgets (tabs, menu items)
  must report them through `ui_widget_ops.debug_parts`, in the frame it
  hit-tests in. Give every ops table a `.name`.

