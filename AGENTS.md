# Repository instructions

This document defines the writing, source layout, testing, and performance rules for `wsap_term` and its framework subproject.

## Writing

Read [WRITING_RULES.md](WRITING_RULES.md) before writing or reviewing documentation, code comments, or work summaries.

## Source and build layout

| Item | Location or convention |
|---|---|
| Packaged data | `assets/`: themes, stylesheets, terminfo, shell integration, icons, and the `.desktop` entry |
| Default UI stylesheet and `:root` variables | `assets/ui/core.css` |
| Default app stylesheet | `assets/ui/app.css` |
| Source lists | The owning directory's `meson.build` |
| Test source layout | Subsystem directories under `tests/`; shared helpers in `tests/helpers/` |
| Meson test names | Flat names in `tests/meson.build`, without directory prefixes |

- Do not add packaged data at the repository root. Source paths and install paths are independent.
- Add a module to the directory that owns it. Do not create a sibling directory that splits a subsystem. Each source directory lists its sources and calls `subdir()` for its children. The top-level source build files assemble the libraries in dependency order.
- Do not check in generated headers. The build embeds the stylesheets and the Vulkan and Metal shaders in headers.
- Keep test names stable when moving test files.

## Library boundaries

The UI framework lives in `subprojects/wsap-framework`. Its `src/` tree provides the base, input, paint, font, UI, theme, render, and window binding libraries.

| Library or subsystem | Responsibility |
|---|---|
| Base (`base_lib`) | Shared utilities, platform helpers, and the event loop in `util/`, `platform/`, and `loop/` |
| Terminal core (`subprojects/term`) | Standalone terminal processing |
| Session (`src/session/`) | Integration between the terminal core and PTY |
| Paint (`subprojects/wsap-framework/src/paint/`) | Paint data shared by the UI and renderer |
| UI core (`subprojects/wsap-framework/src/ui/`) | Widget toolkit without a yasl dependency |
| Window binding (`wsap-ui-yasl`) | Window system and UI integration in `subprojects/wsap-framework/src/ui/yasl/` |
| Terminal grid renderer (`src/terminal/render/`) | Adapter from terminal frames to the framework renderer |
| App runtime (`src/runtime/`) | Frame composition helpers, scene capture, thumbnails, and GPU idle release |

Base, paint, and session are exceptions to the single-owner directory rule. Keep shared logic in those libraries rather than duplicating it in consumers.

A library can use its declared lower layers. Run the include graph check after changing dependencies. The check must report no violations or cycles.

| Check | Command or constraint |
|---|---|
| Include graph | `python3 tools/dev/dirgraph.py build` |
| Libraries allowed to include `yasl/` headers | `base`, `pty`, `session`, `ui_yasl`, `terminal`, `runtime`, and `app` |
| Window binding | No render headers |

### Terminal core

The terminal core builds, tests, and benchmarks independently. It does not include headers from the parent project.

| Item | Value |
|---|---|
| Public headers | `wsap_vtcore.h` and the header-only `unicode/` codec and tables under `subprojects/term/include/wsap_vtcore/` |
| Parent-project dependency | `term_dep` |
| Subproject test and benchmark names | `term:<name>` |
| Embedding and amalgamation | [Terminal core README](subprojects/term/README.md) |

### Window composition

Yasl (`subprojects/yasl`) provides the platform layer. The window binding keeps pacing, presentation, input, clipboard, drag, and input method handling in the same library as the window host, popup menus, accessibility, and inspector server. Keep them together: splitting the library would require reversing the dependency between the window and its UI.

- Keep window content in its own subsystem directory, such as `src/terminal/`. The content supplies its root widget and frame data to the app. Open app windows and mount their root widgets with `window_ui_mount()` from `src/app/`.
- The app owns one renderer. The window tracks framebuffer history for each native surface and presents the frame with its damage region. App code calls `render_draw()` for window frames; the app runtime also calls it for scene captures and thumbnails.
- Keep shared GPU frame preparation in `subprojects/wsap-framework/src/render/gpu/build.c`. It builds instance lists, draw commands, the atlas, and UI image slots without calling a GPU API. The Metal and Vulkan backends contain API-specific code. Do not duplicate frame preparation in a backend.

| Interface or resource | Location or owner |
|---|---|
| Theme sources | `subprojects/wsap-framework/src/theme/` |
| Theme store | The app |
| Window UI API | `subprojects/wsap-framework/src/ui/yasl/client.h` |
| Window inspector API | `subprojects/wsap-framework/src/ui/yasl/inspector.h` |

### C types and widget styling

- Pass positions and sizes as `struct vector2i` values across API boundaries. Use the `vector2i_*` and `rect2i_*` helpers from `subprojects/wsap-framework/src/util/geom.h` instead of duplicating per-axis expressions. The standalone terminal core uses `struct wsap_vtcore_vector2i`: it has no dependency on the framework utilities.
- Use designated initializers for `struct rect2i`. Its anonymous unions expose the same geometry as `pos` and `size` or as `x`, `y`, `width`, and `height`. A positional initializer such as `{0, 0, w, h}` triggers `-Wmissing-braces`. Do not include `geom.h` in C++ translation units: the aliases rely on C union type punning.
- Design types so an all-zero value is valid and meaningful. Rely on zero initialization instead of listing zero-valued fields.
- Prefer CSS for widget appearance. Define new visual properties in `subprojects/wsap-framework/src/ui/css.c` with C fallbacks instead of hardcoding values in widgets.

## Testing

Test observable behavior through stable public interfaces. Choose representative scenarios and regression cases that add coverage.

- Avoid tests that reproduce implementation logic, call private helpers, or assert internal state, bookkeeping, or call order.
- Do not add a test for every edit or duplicate existing coverage.
- If a refactor breaks a test without changing public behavior, revise the test rather than preserving implementation details for it.

## Performance work

Use explicit SIMD in throughput-critical input and rendering loops, such as those exercised by TermMark, `termbench`, and `cat /dev/random`. The shared x86-64 code targets the base instruction set, so auto-vectorization cannot supply AVX2 or AVX-512 kernels there.

| Target | Paths |
|---|---|
| x86 | AVX2 and AVX-512 |
| AArch64 | NEON |
| Fallback | Portable scalar code |

- Use `scan_ascii_run`, `put_ascii_chunk`, and `clear_cells_default` in `subprojects/term/src/` as models for widening input, arranging cells with permutes or zips, writing block stores, and folding range checks into one unsigned subtraction and comparison. See `scan_byte_run` for a kernel that scans line feed bursts.
- Compile the terminal core once and dispatch to x86 SIMD paths at runtime. Do not test `__AVX2__` or `__AVX512*__` in `subprojects/term/`. Put each x86 SIMD path in a separate function and check the matching runtime predicate before calling it. See [SIMD conventions](subprojects/term/src/simd.h).

| x86 path | Function attribute | Runtime predicate |
|---|---|---|
| AVX2 | `WSAP_VTCORE_TARGET_AVX2` | `wsap_vtcore_use_avx2()` |
| AVX-512 | `WSAP_VTCORE_TARGET_AVX512` | `wsap_vtcore_use_avx512()` |

- Pass scalars and pointers to SIMD kernels. Keep inputs shorter than one vector on the inline scalar path: a call costs more than a short scan. For a large function with kernels in its loop, use one always-inline body and a wrapper per target, as `bulk_ascii_lines_body` and `text_run_body` do. This lets the kernels inline into each wrapper.
- Keep loop state in registers and batch bookkeeping. Hoist `cursor_x`, defer `next_row_uid` and `full_gen` updates until the run ends, and fuse known CSI sequences such as adjacent `38;2;` and `48;2;` SGR sequences.
- Detect PTY output floods from drain-loop behavior rather than from IOCTLs. Kernel PTY reads can be short, and `FIONREAD` reports `0` on macOS PTY masters.
- Preserve cell output regardless of load or timing. Limit shaping cost through the layout analysis in `subprojects/wsap-framework/src/font/glyph/layout.c` and the shaping windows in `subprojects/wsap-framework/src/font/glyph/run.c`. Do not skip shaping under load.

| Measurement | Command or benchmark case |
|---|---|
| Live terminal throughput | `meson compile -C build term-live-bench` |
| Headless terminal write throughput | `meson test -C build --benchmark term-write-bench --verbose` |
| Headless renderer | `meson test -C build --benchmark render-bench --verbose` |
| Output floods | `flood-*` cases in `render-bench` |
| Previous results and methods | `benchmarks/*.md` |

## Debugging and tools

| Tool | Purpose |
|---|---|
| `scripts/gdb_buffer_inspector.py` | Trace Wayland buffer contents, damage rectangles, and pixel values across frame commits |
| `scripts/wayland-compositors` | List Wayland compositors and identify those that accept clients |
| `scripts/wsap-resize-latency` | Measure resize response from the protocol log of a private headless sway instance |
| `scripts/wsap-kitty-keys` | Display kitty keyboard protocol reports for keys pressed in the terminal; manual check outside Meson |

### Wayland diagnostics

**Buffer inspection:** `gdb -ex "source scripts/gdb_buffer_inspector.py" -ex "run" build/bin/wsap_term`

Use `scripts/wayland-compositors --env NAME` to print `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` for the first matching compositor. This helps run live tests from a shell without `WAYLAND_DISPLAY`, such as an SSH session. Headless Weston and sway tests do not need this lookup.

**Live KWin example:** `env $(scripts/wayland-compositors --env kwin) build/bin/wsap_term`

The resize latency script measures the time from each drag tick to the configure, acknowledgment, and commit at the new size. Sway does not pace these resizes to the refresh rate, so the measurement reflects app response rather than display latency.

| Resize measurement setting | Value or effect |
|---|---|
| Default drag rate | 1,000 Hz |
| `--gpu cpu` | Select CPU rendering |
| `--fill` | Fill the scrollback before measuring |
| `--out DIR` | Save the logs |

### UI inspector

Use the [UI debugging guide](docs/ui-debugging.md) for the full reference. For each UI change, run the inspector's `lint` command on the affected screens and resolve its findings. For regression tests, prefer `ui_debug_lint_scene()` or the widget's debug parts to pixel positions copied from the implementation.

| Task | Interface |
|---|---|
| Start the inspector | `--debug-socket PATH` or `WSAP_DEBUG_SOCKET=PATH` |
| Query or exercise the UI | `scripts/wsap-debug`: widget tree, hit tests, scene operations, alignment lint, overlays, PNG screenshots, and input injection |
| Run without a display | Start `weston --backend=headless` and point `WAYLAND_DISPLAY` at it; injected input does not need a seat |
| Toggle the overlay in any build | `Ctrl+Shift+F12` (`debug_overlay`) |
| Inspect in GDB | `scripts/gdb_ui_inspector.py`: `ui-cmd` runs socket commands in process; `ui-tree` and `ui-widget` also work on core dumps |

Give each widget operations table a `.name`. A widget that lays out subrectangles without child widgets, such as tab regions or menu items, must report those rectangles through `ui_widget_ops.debug_parts` in the same coordinate space it uses for hit testing.
