# wsap_term

wsap_term is a terminal emulator for Linux and macOS.
Linux supports Wayland with Vulkan GPU rendering and a CPU rendering fallback.
macOS uses Cocoa and Metal GPU rendering.

## Build

See [Build instructions](docs/build.md) for prerequisites and configuration options.

### Linux

Run these commands to build and start the app:

```sh
make
./build/bin/wsap_term
```

### macOS

Install dependencies with Homebrew:

```sh
brew install llvm meson ninja pkgconf yyjson harfbuzz icu4c rust
```

Build and start the app:

```sh
make
./build-homebrew/bin/wsap_term
```

Run `make install` to install files under `~/.local`.

## Usage

Run `wsap_term --help` to show command-line options.
Press `Ctrl+Shift+,` on Linux or `Cmd+,` on macOS to open Settings.
See [Settings](docs/settings.md) and [SSH setup](docs/ssh.md).

## Development

Run the test suite:

```sh
make test
```

See [Development](docs/development.md), [CI](docs/ci.md), and the
[documentation index](docs/README.md).

## License

[MIT](LICENSE).
