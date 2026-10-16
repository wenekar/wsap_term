# wsap_term

"Abandon All Hope, Ye Who Enter Here

This is undoubtedly the most ugly program in the distribution."
xterm's own README from years ago, still resonates well 
with me. Abandon all hope, as you witness an entirely 
AI generated terminal emulator.

`wsap_term` is a Wayland terminal emulator focused on fast 
rendering, full Unicode and CSI support, and an optional SDL
backend for MacOS. After configuring basic font rendering and
Wayland display, I let AI agents loose on this repository
until they produced something that passed all benchmarks, leak
sanitizers and static code checks, and Terminal-Benchmark.

The terminal is a joint-effort of GPT 5.6/6, Claude 5 Opus, and Gemini 3.8 Flash.

Do not try to understand why something is the way it is. I certainly don't. 
If you are the kind of person that is smart enough to understand hand rolled
AVX-512 assembly, and have enough free time to figure out why certain things
are fast, maybe in hopes to build a better terminal, by all means, go ahead.

For anyone else, use with caution, maybe experiment? Do not expect stability.

## Build

Requires Meson 1.12.0+, Python 3.10+, Ninja, pkg-config, a C23 compiler,
yyjson 0.12+, HarfBuzz, and ICU. Linux also needs Wayland, FreeType, and
Fontconfig development files, plus `gio-2.0`, `wayland-client`,
`wayland-cursor`, and `xkbcommon`. macOS uses Cocoa, Metal, and CoreText. Each
backend is tied to its platform. The `-Dwindow_backend=` option only reselects
the backend that the host uses. Linux draws on the CPU. macOS draws with Metal.

On macOS, install the build tools and libraries with Homebrew:

```sh
brew install llvm meson ninja pkgconf yyjson harfbuzz icu4c
```

`make` uses Homebrew LLVM on Apple Silicon and Intel Macs. It puts the build
in `build-homebrew/`. Set `LLVM_BIN` to use another LLVM installation.

```sh
make
./build/bin/wsap_term
make install
```

`make` configures `build/` on first use and compiles it. Installation defaults
to `~/.local`; override it with `make PREFIX=/usr/local install`.
`make release` builds only the installed programs, not the tests and
benchmarks, and `make install` builds the same set. On macOS,
the program installs as `wsap_term.app`. A prefix under `$HOME` puts it in
`~/Applications`, and any other prefix puts it in `/Applications`. Set
`-Dmacos_app_dir=DIR` to choose another directory. `<prefix>/bin/wsap_term`
starts the program from the bundle. To use the wsap_term terminfo entry as root
and for all users, run `sudo <prefix>/bin/install-terminfo.sh`. `make install`
prints the exact path. `make dist` uses a separate build directory
and tests the source archive. It uses the x86-64-v2 baseline on x86-64 and the
compiler default on other architectures. Set `DIST_ISA=<target>` to override
the baseline. It requires a clean Git tree. Use `make dist-local` for an archive
from a dirty tree without archive tests. Meson builds that archive from the
latest commit, so it does not include uncommitted changes. Meson setup options
for the local build go through `MESON_SETUP_FLAGS`, which replaces the defaults
wholesale:

```sh
make MESON_SETUP_FLAGS='--prefix=$HOME/.local --buildtype=release -Disa=x86-64-v3'
```

Use `BUILD_DIR=` for a second configuration alongside the default one. Setup
flags only take effect when the build directory is first configured, so change
them with `meson configure` or start a fresh `BUILD_DIR`.

The default build uses `-O3`, native CPU instructions, and LTO. Use `-Disa=`
for the compiler's default architecture, or `-Disa=x86-64-v3` for an explicit
ISA. The term core still picks AVX2 or AVX-512 at run time above that ISA.
Set `-Db_lto=false` to disable LTO. Each configuration has its own build
directory; the preload library stays beside the terminal executable.

## Tests

GitHub Actions uses self-hosted macOS and Linux runners.
See [CI setup](docs/ci.md) for runner labels and Linux setup.

```sh
make test

# Sanitizers, each in its own build directory with strict runtime options
make test-asan          # AddressSanitizer + Leak + UndefinedBehaviour
make test-tsan          # ThreadSanitizer
make test-msan          # MemorySanitizer (clang; skips the extlib suite)
make test-sanitizers    # all three

# A single test, or the ISA-sensitive subset
meson test -C build term-test --print-errorlogs
meson test -C build --suite isa --print-errorlogs
```

`make test` runs all regular test suites for the host platform. It uses
`build-test` by default. Set `TEST_BUILD_DIR` to use another test build.
On Linux, it also enables the Sway and Kitty suites. It configures
the test build on first use and reuses it on later runs. Use `meson configure`
to change its options. The application build keeps its own settings.
Sanitizers, CPU variants, and benchmarks have separate commands.

Tests reuse compiled objects; Meson tracks header and compiler-option changes.
Assertions remain enabled in release builds. The preload shim and its exec
helper are unsanitized so they can load into ordinary child processes.

Linux/Wayland tests additionally require wayland-server, wayland-protocols,
and wayland-scanner. Font checks need installed CJK, color emoji, and musical
symbol fonts; COLRv1 checks need a COLRv1 font.
The full Linux run also needs Sway, swaymsg, and grim.

`python3 tools/test-matrix.py` runs sanitizer tests natively, then terminal,
font, render, and PTY tests for `x86-64-v2`, `x86-64-v3`, and `znver5` on x86-64.
The compiler and CPU must support each requested ISA; there is no emulation
or automatic skipping. Override with `--isas x86-64-v2 x86-64-v3`, or pass
`--isas` alone for native only. Non-x86 hosts default to native only.

For direct Meson builds, enable optional suites at setup.
Use `-Dintegration_tests=true` for Sway integration tests
and `-Dkitty_tests=true` for the Linux Kitty parser suite
(unsanitized build).

## Benchmarks and tools

```sh
meson test -C build --benchmark term-write-bench --verbose
meson test -C build --benchmark render-bench --verbose
meson compile -C build term-live-bench
./tools/analyze.sh
```

Other benchmarks: `transport-bench` (Linux only), `font-lookup-bench`, and
`render-bench`. `font-lookup-bench` renders glyphs at the default font instance.
Run performance measurements from an optimized, unsanitized
build. See [development notes](docs/development.md) for more detail.

`benchmarks/run-termbench.sh` runs termbench in the terminal.
`benchmarks/run-termbench-headless.sh` runs it on a private headless Sway,
for a machine with no desktop session. Its numbers compare two builds on
the same machine only.

Other tools:

- `python3 tools/loc_per_function.py src` lists functions by code lines.
- `python3 tools/build_macos_icon.py` makes `assets/icons/wsap-term.icns`
  from the SVG icon. It needs `resvg` and the macOS `iconutil`.
- `tools/strip_comments.py` removes comments from the files it gets.
  [The comment cleanup rules](AGENT_COMMENT_CLEANUP.md) say when to use it.

## Usage

Open Settings from the command palette or the terminal context menu.
Press `Ctrl+Shift+,` on Linux or `Cmd+,` on macOS.
The settings form opens in its own window.
Use search, categories, and dropdowns to edit the options.
Save writes the changed values to the active config file.
See [the settings window guide](SETTINGS_WINDOW.md) for details.

run the thing. Like, that's it? Why are you reading this?

`./build/bin/wsap_term` and that's it. Try --help for extra flags. After
`meson install -C build`, launch it as `wsap_term` or from your desktop's application menu.
Use `ninja -C build uninstall` to remove installed files. Set `--prefix`
during setup to choose another installation directory.

Connecting somewhere over ssh? Source `assets/shell/wsap-ssh.fish` (or `.sh`) to have
`ssh` push wsap_term's terminfo to the remote on first connect. See
[colors over ssh](docs/ssh.md).

## License

This project uses the MIT License. See [LICENSE](LICENSE).
