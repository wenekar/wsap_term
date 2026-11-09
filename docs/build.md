# Build and install

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

## macOS updates

Update a source install with these commands:

```sh
git pull --ff-only
make install
```

Restart the app after the install.
The install does not close terminal sessions for you.

The first local install creates a code-signing certificate in your default user Keychain.
Later installs use the same certificate.
This keeps the app identity stable when the executable changes.
macOS can ask for folder access once when you switch from the old ad-hoc signature.
Later updates should keep those grants.
Start the installed app, not the executable in the build directory.
Keep the same app directory and bundle identifier.

Do not delete the `wsap_term local development` identity from Keychain.
Keep a Keychain backup that includes its private key.
`make clean` does not remove this identity.
The certificate lasts ten years.
A new certificate can require new access grants.
The installer does not replace an identity that has lost its private key.
It does not change certificate trust or the macOS privacy database.
Run local installs without `sudo`.

To use an existing signing identity, set its name or fingerprint:

```sh
MACOS_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" make install
```

Use the same value for each update.
To opt out of persistent signing, use `MACOS_SIGN_IDENTITY=- make install`.
Ad-hoc signatures do not keep access grants across changed builds.
Staged installs with `DESTDIR` use ad-hoc signing by default.
They do not create a local Keychain identity.
Packages also use ad-hoc signing unless you set `MACOS_SIGN_IDENTITY`.

See [macOS release updates](macos-updates.md) for the secure updater requirements.
