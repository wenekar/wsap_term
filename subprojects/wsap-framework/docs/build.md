# Build and install the WSAP framework

This guide covers standalone builds, tests, installation, and linking against the framework SDK.

## Prerequisites

- Meson 1.12 or later
- Ninja
- A C23 compiler: GCC 14 or later, or Clang 18 or later
- Python 3.10 or later
- HarfBuzz 7 or later
- yyjson 0.12 or later
- pkg-config

Linux also needs these packages:

- Wayland development files, or X11 development files
- xkbcommon
- FreeType and Fontconfig
- Vulkan 1.3 and `glslc` for GPU rendering

macOS 14 or later needs Homebrew LLVM, HarfBuzz, yyjson, and Rust.

## Configure

```sh
meson setup build
```

The default configuration builds only the framework libraries.
`-Dwindow_backend=x11` selects X11 on Linux.
`-Dvulkan_render=disabled` selects the CPU renderer.
`-Disa=` uses the compiler default CPU target.
`-Ddev_paths=true` makes the font code read the source fonts.
The default is `false`, so a release or SDK install embeds no source path.

## Build and test

Build the SDK with `meson compile -C build`. To enable and run the framework tests in that build directory:

```sh
meson configure build -Dtests=true
meson test -C build --print-errorlogs
```

## Install

```sh
meson install -C build --destdir "$PWD/STAGE"
```

The install contains these files:

- `lib/libwsap-*.a`
- `lib/libyasl.*`
- `include/wsap/**`
- `include/yasl/**`
- `lib/pkgconfig/*.pc`

## Check a pkg-config consumer

```sh
export PKG_CONFIG_PATH="$PWD/STAGE/usr/local/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$PWD/STAGE"
cc tools/ci/pkgconfig_consumer.c -o consumer \
    $(pkg-config --cflags --libs wsap-ui)
./consumer
```

The pkg-config files name the system libraries that the static framework
libraries need.
