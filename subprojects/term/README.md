# wsap_vtcore

wsap_vtcore is a headless terminal core. It parses terminal output and keeps
screen, scrollback, input-mode, and snapshot state. It is a C11 library. It
needs libm and threads.

| Path | Contents |
| --- | --- |
| `include/wsap_vtcore/` | Public C headers and Unicode headers |
| `src/` | Private implementation |
| `tests/` | Unit, model, fuzz, portability, and Kitty parser tests |
| `benchmarks/` | Write, resize, and history benchmarks |
| `tools/` | Amalgamation and Unicode generators |
| `examples/` | Small stdin-to-snapshot embedding example |

Build and test the library alone:

```sh
meson setup build && meson test -C build
```

## Embedding

Use `#include <wsap_vtcore/wsap_vtcore.h>`. The header includes the input and
type headers. It compiles as C11 and as C++. The package adds only the
include directory, so a `wsap_vtcore.h` from another library does not shadow
it. Initialize options with `{0}`. See [docs/api.md](docs/api.md) for
ownership, locking, and failure rules.

- **Meson subproject:** use `dependency('wsap_vtcore')` or get
  `wsap_vtcore_dep` from `subproject('wsap_vtcore')`. The dependency adds
  libm and threads.
- **Amalgamation:** run `tools/amalgamate.py . OUT`. It writes
  `OUT/wsap_vtcore.c` and `OUT/wsap_vtcore.h`. Compile `wsap_vtcore.c` with a
  C11 compiler and link libm and threads. On POSIX, pass `-pthread`. A Rust
  `build.rs` needs one `cc::Build::new().file("wsap_vtcore.c")` call.
- **Sources:** compile `src/*.c` and `src/osc/*.c` once each. Do not compile
  `src/gstore.c`, `src/hist.c`, or `src/term.c`: `src/term_state.c` includes
  them. Add `src` and `include/wsap_vtcore` to the include path. Link libm
  and threads. `term-cross-test` uses this list.

On Windows the writer lock is an SRWLOCK from the system library. It needs no
extra link.

A host connects the terminal in four places:

1. Create it with `wsap_vtcore_new()`. Set `callbacks.reply` to send replies
   to the program. Set `name` to the name that programs read with XTVERSION
   and XTGETTCAP.
2. Feed program output with `wsap_vtcore_feed()`.
3. Put key and mouse events into `struct wsap_vtcore_key_event` and
   `struct wsap_vtcore_mouse_event`. Encode them with
   `wsap_vtcore_input_encode_key()` and `wsap_vtcore_input_encode_mouse()`.
   Send the bytes to the program.
4. Paint from a `struct wsap_vtcore_snapshot`. The snapshot gives code points,
   colors, and attributes. The host shapes and draws the glyphs.

`examples/stdin_snapshot.c` shows steps 1, 2, and 4. It builds against the
installed Meson dependency, or against the amalgamation with
`-DWSAP_VTCORE_AMALGAMATION`.

## SIMD

The hot paths have AVX2, AVX-512, and NEON kernels. Without them, scroll and
line-feed output runs 8 to 30 times slower. The build needs no flags to get
them:

- On x86-64 with GCC or Clang, each kernel compiles at every level with a
  target attribute. The first `wsap_vtcore_new()` selects the highest level
  that the CPU supports: `base` (x86-64), `avx2` (x86-64-v3), or `avx512`
  (x86-64-v4 with VBMI and VBMI2). `wsap_vtcore_isa()` names the level in use.
  Set `WSAP_VTCORE_ISA` to a lower level name to force it.
- The `arch_flag` option, such as `-march=x86-64-v3`, sets the lowest level.
  Checks at or below that level become constants, and their kernels inline.
- aarch64 always has NEON, so it needs no choice at run time.
- MSVC gets only the levels that its `/arch` flag enables.

See `src/simd.h` before you change or add a kernel.

## Public API

`wsap_vtcore_version()` returns the library version. Every global symbol
starts with `wsap_vtcore_`. The library builds with hidden symbols, so a
shared build exports only the `WSAP_VTCORE_API` functions. `term-symbols-test`
checks both rules.

Public structs are not opaque. Their layout is part of the current interface,
but this version makes no ABI promise. See
[docs/compatibility.md](docs/compatibility.md).

The `static inline` helpers (`wsap_vtcore_color_*`, `wsap_vtcore_view_color`,
`wsap_vtcore_attr_underline`) decode documented bit layouts. Binding
generators such as bindgen skip them. Copy them into the binding, or use the
generator's wrappers for static functions.

The library does not provide a renderer, font system, PTY, or process runner.

## Documents

| Document | Contents |
| --- | --- |
| [docs/api.md](docs/api.md) | Ownership, threads, callbacks, and failure rules |
| [docs/architecture.md](docs/architecture.md) | Source map, write core, history, and snapshots |
| [docs/security.md](docs/security.md) | Untrusted input, host policy, and resource limits |
| [docs/testing.md](docs/testing.md) | Test suites, fuzzing, and allocation-failure checks |
| [docs/build-validation.md](docs/build-validation.md) | Standalone build, package, and cross checks |
| [docs/compatibility.md](docs/compatibility.md) | Version and ABI limits |
