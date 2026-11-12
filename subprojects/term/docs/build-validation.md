# Build validation

## Prerequisites

The standalone build requires these tools and libraries:

- A C11 compiler
- Meson and Ninja
- pkg-config
- System math library (`libm`)
- POSIX threads

Tests also require:

- Python 3.10 or later
- Bash
- Zig (for cross-compilation checks)

## Standalone build

Build and test the core without the application:

```sh
meson setup build -Dkitty=disabled
meson compile -C build
meson test -C build --print-errorlogs
```

## Validation targets

### Cross-platform test

`term-cross-test` uses Zig to verify compilation.
It builds the source files and the amalgamation for these targets:

- Linux AArch64
- macOS x86-64 and AArch64
- Windows GNU x86-64

The test skips automatically when Zig is not installed.
Install Zig to run this check.

### Package test

`term-package-test` validates installation and public consumers.
It disables tests and installs the core to a temporary prefix.
It then builds and links four consumer programs:

- A C shared-library consumer
- A C static-library consumer
- A C++ shared-library consumer
- An amalgamation consumer

Consumers use only public headers.

### Fuzzing target

Configure with `-Dfuzzing=true` under Clang to enable libFuzzer targets.
This option adds the `fuzz` test suite.
See [testing.md](testing.md) for full test suite details.

## Source distribution

Create a source archive with:

```sh
meson dist -C build --no-tests
```

The standalone archive contains public headers, source files, tests, and tools.
