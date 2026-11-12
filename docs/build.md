# Build and install

## Prerequisites

The build requires these tools and libraries:

- A C23 compiler (GCC 14 or later, or Clang 18 or later)
- Meson 1.12 or later
- Ninja and pkg-config
- Python 3.10 or later
- HarfBuzz 7 or later
- ICU
- yyjson 0.12 or later

### Linux prerequisites

Linux builds require these additional packages:

- Wayland development files (`libwayland-dev`, `wayland-protocols`)
- FreeType and Fontconfig
- xkbcommon development files (`libxkbcommon-dev`)
- GIO and GLib 2.0 (`libglib2.0-dev`)

For Vulkan GPU rendering, Linux builds also require:

- Vulkan 1.3 headers and loader (`libvulkan-dev`)
- The `glslc` shader compiler

If Vulkan 1.3 or `glslc` is missing, the build falls back to the CPU renderer.

### macOS prerequisites

macOS builds require macOS 14 or later and Homebrew packages:

```sh
brew install llvm meson ninja pkgconf yyjson harfbuzz icu4c rust
```

## AccessKit

The build links AccessKit C statically for accessibility.
Meson first checks for the `accesskit-c-0.23` pkg-config package.
The build needs version 0.23.2 or a later 0.23 release.
If the package is missing, Meson downloads the pinned 0.23.2 source archive.
The fallback build needs Cargo and Rust 1.87 or later.
The wrap file adds the dependency version to the upstream Meson metadata.
The first build downloads the Rust dependencies.
Later builds use the Cargo cache.
Set `PKG_CONFIG_PATH` if the system package uses a custom prefix.
Use `--wrap-mode=nofallback` to require the system package.
`make install` skips subprojects and installs only the application files.

## Build targets

| Command | Result |
| --- | --- |
| `make` | Build the application, tests, and benchmarks. |
| `make release` | Build installed programs. |
| `make install` | Build and install programs after confirmation. |
| `make PREFIX=/usr/local install` | Install under a different prefix. |
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
Use `meson configure BUILD_DIR` to change an existing build directory.
`make install` sets the prefix of the build to `PREFIX` before it builds.
It builds and installs only the application.
It does not build the tests.
It does not install the yasl or wsap_vtcore libraries or headers.
A `--prefix` in `MESON_SETUP_FLAGS` does not change where `make install` installs files.

| Meson option | Effect |
| --- | --- |
| `-Disa=` | Use the compiler default CPU target. |
| `-Disa=x86-64-v3` | Set an explicit CPU target. |
| `-Db_lto=false` | Disable LTO. |
| `-Dtests=false` | Exclude tests. |
| `-Dbenchmarks=false` | Exclude benchmarks. |
| `-Dvulkan_render=auto` | Enable Vulkan on Linux when Vulkan 1.3 and `glslc` are available. Fall back to CPU rendering if either is missing. |
| `-Dvulkan_render=enabled` | Require Vulkan 1.3 and `glslc` on Linux. Fail the build if either is missing. |
| `-Dvulkan_render=disabled` | Disable Vulkan rendering. Use only the CPU renderer. |
| `-Dmacos_app_dir=DIR` | Set the macOS application directory. |

The term core selects its SIMD level at run time at or above the CPU target.

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

## macOS updates

Update a source installation with these commands:

```sh
git pull --ff-only
make install
```

Restart the app after the install.
The install does not close open terminal sessions.

The first local install creates a code-signing certificate in your default user Keychain.
Later installs use the same certificate.
This keeps the application identity stable when the binary changes.
macOS can ask for folder access once when you switch from an ad-hoc signature.
Later updates keep those permissions.
Start the installed application, not the binary in the build directory.
Keep the same application directory and bundle identifier.

Do not delete the `wsap_term local development` identity from Keychain.
Keep a Keychain backup that includes its private key.
`make clean` does not remove this identity.
The certificate remains valid for ten years.
A new certificate can require new permissions.
The installer does not replace an identity that lacks its private key.
It does not change certificate trust or the macOS privacy database.
Run local installs without `sudo`.

To use an existing signing identity, set its name or fingerprint:

```sh
MACOS_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" make install
```

Use the same value for every update.
To disable persistent signing, run `MACOS_SIGN_IDENTITY=- make install`.
Ad-hoc signatures do not keep permissions across changed builds.
Staged installs with `DESTDIR` use ad-hoc signing by default.
They do not create a local Keychain identity.
Packages also use ad-hoc signing unless you set `MACOS_SIGN_IDENTITY`.

See [macOS release updates](macos-updates.md) for the secure updater requirements.
