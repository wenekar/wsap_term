# wsap_vtcore

wsap_vtcore is a headless terminal core: the parser, screen ring,
logical-line history store, write core, and viewport snapshots. It is a C11
library with no dependency but libm and threads.

| Path | Contents |
| --- | --- |
| `include/wsap_vtcore/` | The public headers: `wsap_vtcore.h`, and the header-only `unicode/` |
| `src/` | The core. Its headers are private. |
| `tests/` | Unit, model, fuzz, and portability tests, and the Kitty parser suite |
| `benchmarks/` | `term-write-bench`, `term-resize-bench`, and `hist-bench` |
| `tools/` | `amalgamate.py` and `generate_unicode.py` |

`meson setup build && meson test -C build` builds and tests it alone.

## Embedding

Pick one of these:

- **Meson subproject**: `dependency('wsap_vtcore')`, or
  `subproject('wsap_vtcore').get_variable('wsap_vtcore_dep')`. It brings
  libm and `dependency('threads')` with it. The `arch_flag` option sets the
  lowest instruction set, such as `-march=x86-64-v3`.
- **Amalgamation**: run `tools/amalgamate.py . OUT`. It writes
  `OUT/wsap_vtcore.c` and `OUT/wsap_vtcore.h`. Compile `wsap_vtcore.c` with
  any C11 compiler and link libm and threads: `dependency('threads')` in
  Meson, `-pthread` elsewhere on POSIX. A Rust `build.rs` needs one
  `cc::Build::new().file("wsap_vtcore.c").compile("wsap_vtcore")`.
- **Sources**: compile `src/*.c` and `src/osc/*.c` once, with `src` and
  `include/wsap_vtcore` on the include path. Link libm and threads, as for
  the amalgamation.

The library needs nothing else. On Windows the writer lock is an SRWLOCK
from the system library, so it needs no extra link. The `*-amalgamation`
tests run the wsap_vtcore tests against the generated `wsap_vtcore.c`, and
`term-cross-test` compiles it for aarch64, macOS, and Windows.

### SIMD level

The hot paths have AVX2, AVX-512, and NEON versions. Without them,
scroll and line-feed output runs 8 to 30 times slower. The build needs
no flags to get them:

- On x86-64 with GCC or Clang, each kernel compiles at every level with
  a target attribute. The first `wsap_vtcore_new()` picks the highest level the
  CPU supports. The levels are `base` (x86-64), `avx2` (x86-64-v3), and
  `avx512` (x86-64-v4 with VBMI and VBMI2). `wsap_vtcore_isa()` names the level
  in use. Set `WSAP_VTCORE_ISA` to a lower level to force it.
- `-march` sets the lowest level. The checks for that level and below
  become constants, and their kernels inline into their callers.
- aarch64 always has NEON, so it needs no choice at run time.
- MSVC gets only the levels that its `/arch` flag enables.

`simd.h` states the rules for a new kernel.

- `wsap_vtcore.h` is the only header a host includes. A host writes
  `#include <wsap_vtcore/wsap_vtcore.h>`. The package adds only the include
  directory, so `wsap_vtcore.h` from another library, such as ncurses, is not
  shadowed. The header includes `wsap_vtcore_input.h` and
  `wsap_vtcore_types.h`. It compiles as C11 and as C++.
- Every global symbol starts with `wsap_vtcore_`. The library builds with hidden
  symbols, so a shared build exports only the `WSAP_VTCORE_API` functions of
  `wsap_vtcore.h`. `term-symbols-test` checks both.
- Any thread can call any function. Calls on one terminal run one at a
  time. `wsap_vtcore.h` states the rules.
- The `static inline` helpers (`wsap_vtcore_color_*`, `wsap_vtcore_view_color`,
  `wsap_vtcore_attr_underline`) only decode documented bit layouts. A binding
  generator such as bindgen skips them. Mirror them in the binding, or
  use the generator's static-function wrappers.

A host wires the terminal in four places:

1. Create it with `wsap_vtcore_new()`. Set `wsap_vtcore_options.callbacks.reply` to
   send replies to the program. Set `wsap_vtcore_options.name` to the name
   programs read with XTVERSION and XTGETTCAP. It defaults to `wsap_vtcore`.
2. Feed program output with `wsap_vtcore_feed()`.
3. Translate key and mouse events into `struct wsap_vtcore_key_event` and
   `struct wsap_vtcore_mouse_event`. Encode them with `wsap_vtcore_input_encode_key()`
   and `wsap_vtcore_input_encode_mouse()`, and send the bytes to the program.
4. Paint from a `struct wsap_vtcore_snapshot`. The terminal gives code points,
   colors, and attributes. The host shapes and draws the glyphs.

## Files and responsibilities

The files are in `src/`.

| File | Responsibility |
| --- | --- |
| `term.c` | Mode queries, cursor options, mouse reports, focus events |
| `core.c`, `core.h` | Unified write core primitives (`sc_print_ascii`, `sc_linefeed`, etc.) |
| `screen.c`, `screen.h` | Screen ring buffer, line feed, editing, resize, and reflow |
| `hist.c`, `hist.h` | Logical-line history store, SIMD cell encoder, and page arena |
| `hist_view.c`, `hist_view.h` | Viewport anchor, history row lookup, and scroll offset |
| `snapshot.c` | Immutable view snapshot, row keys, and scroll movement detection |
| `selection.c` | Text selection, coordinate mapping, and UTF-8 export |
| `gstore.c`, `gstore.h` | Per-terminal grapheme cluster table |
| `text.c` | Byte stream decoder, control character dispatch, and text run routing |
| `text_run.c`, `text_run.h` | SIMD UTF-8 text run blit |
| `fastcsi.c` | Fast CSI sequence parser |
| `input.c` | Key and mouse encoding |
| `escape.c` | Parser state machine for ESC, CSI, DCS, and OSC |
| `pen.h` | SGR graphics rendition state |
| `osc/dispatch.c` | OSC command dispatcher and palette queries |
| `osc/link.c` | OSC 8 hyperlink table |
| `osc/clipboard.c` | OSC 52 clipboard reads and writes |
| `osc/misc.c` | Operating system commands: title, directory, notifications |

## Architecture

### Write core
All text and control sequences route through unified primitives in
`core.c`. Primitives handle character printing, line feeds, margins,
and cursor movement.

### Screen and storage
The visible screen is a circular ring of rows (`struct screen`).
Rows that scroll off the top move into the logical-line history store
(`struct hist`).
The history store compacts lines into UTF-8 text and delta-encoded
attribute runs.

### Viewport and snapshots
The viewport names a logical line and a wrapped segment, or the live bottom.
The anchor also keeps the cell offset before wrapping.
Output keeps the anchor on the same text.
Resize maps that cell offset to a segment at the new width.
Eviction clamps the anchor to the oldest retained line.
A line can lose its prefix when it exceeds the history byte limit.
The anchor tracks the removed cells to keep its text position.
The alternate screen has no history.

Relative scroll walks from the anchor.
Absolute scroll uses a circular Fenwick index of line row counts.
Snapshot capture updates this index.
The write path does not update the index.
History descriptors keep row totals for the current width.
A width change starts wide lines with estimated row counts.
Snapshot capture makes the counts exact for lines in the view.
Counts above 254 rows use a separate cache.
That cache grows only when a visited line needs it.

`wsap_vtcore_snapshot_update` takes a mutable terminal.
It updates history query metadata before it copies the view.
Snapshot getters and history getters do not change terminal state.
Renderers do not read terminal memory directly. Renderers read an
immutable snapshot (`struct wsap_vtcore_snapshot`).
Snapshots track row keys based on row content identities.
When output scrolls, snapshots detect the scroll distance `k`.
Renderers shift existing pixels and draw records by `k` rows and paint
only changed rows.
