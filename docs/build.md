# Build and install

## Accessibility dependency

Native accessibility is off by default.
Enable it with `meson configure BUILD_DIR -Daccessibility=true`.
For a new build, use `meson setup BUILD_DIR -Daccessibility=true`.
The yasl binding uses [AccessKit C](https://github.com/AccessKit/accesskit-c)
for AT-SPI on Linux and AppKit accessibility on macOS.
The UI core has no AccessKit dependency.

Install Rust and Cargo 1.87 or later with [rustup](https://rustup.rs/).
Meson skips native accessibility if either tool is absent or too old.
This rule also applies when AccessKit C is installed.
When the option is off, Meson does not check for Rust or AccessKit C.
It does not download or build AccessKit C.

When enabled, Meson checks for `accesskit-c-0.23` version 0.23.1 or later.
If it is absent, Meson builds the release pinned in
`subprojects/accesskit-c.wrap`. Cargo uses that release's lockfile.
The first build downloads the Rust dependencies.
For a cross build, set `-Daccesskit-c:triplet=TARGET` to the Rust target triple.
The build summary reports whether native accessibility is enabled.
Native provider tests run only when it is enabled.

## Build targets

| Command | Result |
| --- | --- |
| `make` | Build the app, tests, and benchmarks. |
| `make release` | Build installed programs. |
| `make install` | Build and install programs after confirmation. |
| `make PREFIX=/usr/local install` | Install under another prefix. |
| `make dist` | Build and test a source archive from a clean Git tree. |
| `make dist-local` | Build a source archive without archive tests. |

Source archives contain the latest commit.
They do not include uncommitted changes.

## Configuration

| Variable | Default |
| --- | --- |
| `BUILD_DIR` | `build` on Linux; `build-homebrew` on macOS. |
| `PREFIX` | `~/.local`. |
| `LLVM_BIN` | Homebrew LLVM `bin` directory on macOS. |
| `TEST_BUILD_DIR` | `$(BUILD_DIR)-test`. |
| `DIST_BUILD_DIR` | `$(BUILD_DIR)-dist`. |
| `DIST_ISA` | `x86-64-v2` on x86-64; compiler default elsewhere. |

The default build uses `-O3`, native CPU instructions, and LTO.
`MESON_SETUP_FLAGS` replaces all default setup flags:

```sh
make BUILD_DIR=build-portable MESON_SETUP_FLAGS="--prefix=$HOME/.local --buildtype=release -Disa=x86-64-v3"
```

Setup flags apply to new build directories.
Use `meson configure BUILD_DIR` to change an existing build.

| Meson option | Effect |
| --- | --- |
| `-Disa=` | Use the compiler's default CPU target. |
| `-Disa=x86-64-v3` | Set an explicit CPU target. |
| `-Db_lto=false` | Disable LTO. |
| `-Daccessibility=true` | Enable native accessibility if Rust and Cargo are available. |
| `-Dtests=false` | Exclude tests. |
| `-Dbenchmarks=false` | Exclude benchmarks. |
| `-Dmacos_app_dir=DIR` | Set the macOS app directory. |

The term core selects its SIMD level at run time above the build target.

## Installation

On Linux, start `wsap_term` from the shell or application menu.
On macOS, start `wsap_term.app` or the installed `wsap_term` launcher.

| macOS prefix | Default app directory |
| --- | --- |
| Under `$HOME` | `~/Applications`. |
| Other prefixes | `/Applications`. |

To install terminfo for all users:

```sh
sudo ~/.local/bin/install-terminfo.sh
```

Use your install prefix if it differs from `~/.local`.
Remove installed files with `ninja -C BUILD_DIR uninstall`.
