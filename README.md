# wsap_term

A terminal emulator for Linux and macOS.
Linux uses Wayland and CPU rendering.
macOS uses Cocoa and Metal.

## Build

Requires Meson 1.12+, Python 3.10+, Ninja, pkg-config, a C23 compiler,
yyjson 0.12+, HarfBuzz 7+, and ICU.
Linux also needs Wayland, FreeType, Fontconfig, GIO, and xkbcommon development files.
Accessibility uses AccessKit C 0.23.1+ (API 0.23).
Meson builds the pinned release if it cannot find an installed library.
This build needs Rust and Cargo 1.87+.

On Linux:

```sh
make
./build/bin/wsap_term
```

On macOS:

```sh
brew install llvm meson ninja pkgconf yyjson harfbuzz icu4c
make
./build-homebrew/bin/wsap_term
```

Run `make install` to install under `~/.local`.
See [build options](docs/build.md) for other configurations and packages.

## Usage

Run `wsap_term --help` for command options.
Open Settings with `Ctrl+Shift+,` on Linux or `Cmd+,` on macOS.
See [settings](docs/settings.md) and [SSH setup](docs/ssh.md).

## Development

```sh
make test
```

See [development](docs/development.md), [CI](docs/ci.md), and the
[documentation index](docs/README.md).

## License

[MIT](LICENSE).
