# Build validation

Build the core without the app:

```sh
meson setup build -Dkitty=disabled
meson compile -C build
meson test -C build --print-errorlogs
```

The `term-cross-test` uses Zig to check the source files and amalgamation for
Linux AArch64, macOS x86-64 and AArch64, and Windows GNU x86-64. It returns a
Meson skip when Zig is not installed. Install Zig to run this check.

`term-package-test` builds with tests disabled, installs to a temporary prefix,
and runs C shared and static consumers, a C++ shared consumer, and an
amalgamation consumer. It uses only public headers.

Build with `-Dfuzzing=true` and Clang to add the libFuzzer target and its
`fuzz` suite. [testing.md](testing.md) describes all test layers.

Create a source archive with:

```sh
meson dist -C build --no-tests
```

The standalone build needs a C11 compiler, Meson, Ninja, pkg-config, libm, and
threads. Tests also use Python 3.10 or newer and Bash. The standalone source
archive contains the public headers, implementation files, tests, and tools.
