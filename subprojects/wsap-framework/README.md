# WSAP framework

The WSAP framework is a C23 toolkit for native desktop user interfaces.
It contains a platform layer, a window system, a widget toolkit, font
handling, a scene renderer, and shared utility libraries.
The repository is a framework-only project.
It does not contain the terminal application or the editor.

## Layers

| Library | Contents |
| --- | --- |
| `wsap-base` | Utilities, platform code, and the event loop. |
| `wsap-input` | Input mapping and key bindings. |
| `wsap-paint` | Scenes, damage regions, and paint primitives. |
| `wsap-font` | Font discovery, shaping, and rasterization. |
| `wsap-ui` | Widget toolkit and the CSS engine. |
| `wsap-theme` | Theme catalog and appearance data. |
| `wsap-render` | CPU and GPU scene renderer. |
| `wsap-ui-yasl` | Window system and UI binding. |
| `wsap-desktop` | Default window runner and frame composer. |
| `yasl` | Subproject with desktop surfaces and input. |

Libraries depend on specific lower layers, not every lower layer. Yasl is a separate native platform dependency.
Run the include graph check after a source change:

```sh
python3 tools/dev/dirgraph.py build
```

## Build

The build needs Meson 1.12 or later, Ninja, a C23 compiler, Python 3.10 or
later, HarfBuzz, yyjson, and the window system development files.
On Linux, the build also needs FreeType, Fontconfig, Wayland or X11, and
xkbcommon.
The AccessKit fallback needs Cargo and Rust 1.87 or later.
A clean build of that fallback downloads the Rust crates that
`subprojects/accesskit-c-0.23.2/Cargo.lock` names.
An offline machine needs those crates in the Cargo cache before the build.
Run `cargo fetch --locked` in `subprojects/accesskit-c-0.23.2` on a machine
with network access, and copy `~/.cargo/registry` to the offline machine.
Alternatively, vendor the crates with `cargo vendor` and point
`subprojects/accesskit-c-0.23.2/.cargo/config.toml` at the vendor directory.

```sh
meson setup build -Dtests=false -Dbenchmarks=false
ninja -C build
```

The default build makes only the framework libraries.
Use `-Dtests=true` to build the framework tests.
Use `-Dbenchmarks=true` to build the font benchmarks.

## Install

```sh
meson install -C build --destdir "$PWD/STAGE"
```

The install puts the static libraries in `lib`, the public headers in
`include/wsap`, and the pkg-config files in `lib/pkgconfig`.
It also installs the `yasl` and AccessKit dependencies.

## Use from pkg-config

```sh
export PKG_CONFIG_PATH="$PWD/STAGE/usr/local/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$PWD/STAGE"
cc app.c $(pkg-config --cflags --libs wsap-ui)
```

Use `wsap-ui` for widgets without a display server. Use `wsap-desktop` to
open a window without writing a native event loop or frame composer. See
[Run a desktop window](docs/desktop.md) for an example that displays pixels.

## Documentation

- [Run a desktop window](docs/desktop.md)
- [Build and install](docs/build.md)
- [Font API](docs/font-api.md)
- [UI library](docs/ui-library-state.md)
- [Build a window screen](docs/ui-screen-guide.md)
- [UI inspector](docs/ui-debugging.md)
- [High-refresh displays](docs/high-refresh.md)

## License

MIT. See [LICENSE](LICENSE).

## Third-party notices

The framework ships data and code from other projects. `LICENSES/NOTICE.md`
lists them. The install puts the notice and the license texts in
`share/licenses/wsap-framework`.

| Component | License |
| --- | --- |
| FiraCode Nerd Font Mono (`assets/fonts/`) | OFL-1.1, plus the MIT notice of the Nerd Fonts patcher. |
| Adwaita Mono subset (`src/font/backends/freetype/cards_font.inc`) | OFL-1.1. |
| `font5x7.h` | Public domain character generator table. |
| accesskit-c (subproject) | MIT OR Apache-2.0, BSD-3-Clause for `LICENSE.chromium`, LGPL-2.1 for the build scripts. |
| yasl (subproject) | MIT, as this repository. |
| Unicode data (`src/util/unicode/unicode_data.inc`) | Unicode License V3. |
| Rust crates (via `accesskit-c`) | Permissive only: MIT, Apache-2.0, Unlicense, Zlib, and Unicode-DFS-2016. [THIRD_PARTY.md](THIRD_PARTY.md) lists every crate and its license. |
