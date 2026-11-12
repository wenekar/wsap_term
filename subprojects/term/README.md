# wsap_vtcore

wsap_vtcore is a headless terminal core.
It parses terminal output and maintains screen, scrollback, input-mode, and snapshot state.
The library uses C11.
It requires libm and a threads library.

| Path | Contents |
| --- | --- |
| `include/wsap_vtcore/` | Public C headers and Unicode headers |
| `src/` | Private implementation |
| `tests/` | Unit, model, fuzz, portability, and Kitty parser tests |
| `benchmarks/` | Write, resize, and history benchmarks |
| `tools/` | Amalgamation and Unicode generators |
| `examples/` | Stdin-to-snapshot embedding example |

Build and test the standalone library:

```sh
meson setup build && meson test -C build
```

## Embedding

Include `<wsap_vtcore/wsap_vtcore.h>`.
The header includes input and type definitions.
The header compiles with C11 and C++ compilers.
The package exports only its include directory.
This prevents header shadowing from other libraries.
Zero-initialize `struct wsap_vtcore_options` with `{0}`.
See [docs/api.md](docs/api.md) for ownership, locking, and failure rules.

Embedding options:

- **Meson subproject:** Use `dependency('wsap_vtcore')` or `subproject('wsap_vtcore').get_variable('wsap_vtcore_dep')`. The dependency adds libm and threads.
- **Amalgamation:** Run `tools/amalgamate.py . OUT`. The script writes `OUT/wsap_vtcore.c` and `OUT/wsap_vtcore.h`. Compile `wsap_vtcore.c` with a C11 compiler and link libm and threads. On POSIX systems, add `-pthread`. In Rust `build.rs`, call `cc::Build::new().file("wsap_vtcore.c")`.
- **Direct sources:** Compile `src/*.c` and `src/osc/*.c`. Do not compile `src/gstore.c`, `src/hist.c`, or `src/term.c` directly. `src/term_state.c` includes them. Add `src` and `include/wsap_vtcore` to the include path. Link libm and threads. `term-cross-test` validates this configuration.

On Windows, the internal writer lock uses system `SRWLOCK` primitives without additional links.

Integration uses four main interfaces:

1. `wsap_vtcore_new()` creates the terminal instance. The `callbacks.reply` function pointer receives terminal replies. The `name` string sets the terminal name for XTVERSION and XTGETTCAP reports.
2. `wsap_vtcore_feed()` accepts terminal output bytes for parsing.
3. `wsap_vtcore_input_encode_key()` and `wsap_vtcore_input_encode_mouse()` encode input events into terminal escape sequences.
4. `struct wsap_vtcore_snapshot` exposes screen contents, colors, and cell attributes for drawing.

`examples/stdin_snapshot.c` demonstrates instance creation, byte feeding, and snapshot reading.
It compiles against the installed Meson dependency or against the amalgamation with `-DWSAP_VTCORE_AMALGAMATION`.

## SIMD

Hot code paths provide AVX2, AVX-512, and NEON kernels.
Kernels accelerate line feed bursts and scroll operations.
The build requires no special flags:

- On x86-64 with GCC or Clang, functions compile with target attributes for each level. The first call to `wsap_vtcore_new()` selects the highest supported CPU level: `base` (x86-64), `avx2` (x86-64-v3), or `avx512` (x86-64-v4 with VBMI and VBMI2). `wsap_vtcore_isa()` returns the active level name. The `WSAP_VTCORE_ISA` environment variable overrides this selection.
- The `arch_flag` option (such as `-march=x86-64-v3`) sets the minimum target level. Checks at or below that level evaluate as compile-time constants and inline their kernels.
- On aarch64, code always uses NEON without runtime selection.
- On MSVC, builds enable only the levels specified by the `/arch` flag.

See `src/simd.h` for SIMD kernel rules.

## Public API

`wsap_vtcore_version()` returns the library version code.
Every public symbol begins with `wsap_vtcore_`.
The library hides internal symbols by default.
Shared library builds export only `WSAP_VTCORE_API` symbols.
`term-symbols-test` verifies both rules.

Public structs are non-opaque for direct field access.
Struct layouts belong to the current interface version without ABI stability guarantees across releases.
See [docs/compatibility.md](docs/compatibility.md).

Static inline helpers (`wsap_vtcore_color_*`, `wsap_vtcore_view_color`, `wsap_vtcore_attr_underline`) decode bit layouts.
Foreign-function bindings can reimplement these helpers or use static wrapper functions.

The core contains no renderer, font rasterizer, PTY driver, or process runner.

## Documents

| Document | Contents |
| --- | --- |
| [docs/api.md](docs/api.md) | Ownership, threads, callbacks, and failure rules |
| [docs/architecture.md](docs/architecture.md) | Source map, write core, history, and snapshots |
| [docs/security.md](docs/security.md) | Untrusted input, host policy, and resource limits |
| [docs/testing.md](docs/testing.md) | Test suites, fuzzing, and allocation-failure checks |
| [docs/build-validation.md](docs/build-validation.md) | Standalone build, package, and cross checks |
| [docs/compatibility.md](docs/compatibility.md) | Version and ABI limits |
