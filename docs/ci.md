# GitHub Actions CI

The workflow is `.github/workflows/ci.yml`.
It runs only when you start it manually.
Start CI from **Actions > CI > Run workflow**.
The jobs use self-hosted runners with repository-specific labels.
The checkout step does not keep GitHub credentials.

Each job runs `bash tools/ci/build.sh`.
The script configures a release build in `build/ci`.
It uses the compiler's default CPU target and enables LTO.
It checks the include graph, builds the app, and runs the regular tests.
The Linux run includes the Kitty parser tests.
CI does not run benchmarks or display integration tests.
Each job uploads Meson logs and keeps them for seven days.

## macOS runner

This Mac uses these labels:

```text
self-hosted, macOS, ARM64, wsap-term-macos
```

Install Xcode command line tools and these Homebrew packages:

```sh
brew install llvm meson ninja pkgconf yyjson harfbuzz icu4c
```

The build script selects Homebrew LLVM and its archive tools.
The runner service must have Homebrew on its `PATH`.
Font tests need CJK, emoji, and musical symbol fonts.
Metal tests need access to the GPU.

Keep the Mac awake and the runner user logged in.

## Linux runner

The Linux job stays disabled until the repository variable
`WSAP_LINUX_CI` has the value `true`.

Prepare a Linux host with a C23 compiler, Meson 1.12.0 or newer,
Python 3.10 or newer, Ninja, pkg-config, and `tic`.
Install the development files for yyjson 0.12 or newer,
HarfBuzz 7 or newer, ICU, FreeType, Fontconfig, GLib/GIO,
Wayland client, Wayland cursor, Wayland server, Wayland protocols,
and xkbcommon.
Install `wayland-scanner`.
Install CJK, color emoji, musical symbol, and COLRv1 fonts.
The regular tests do not need a display server.
The runner user must have access to PTYs.

Open the repository's **Settings > Actions > Runners > New self-hosted runner**.
Select Linux and the host architecture.
Follow the download and configuration commands on that page.
Add `--labels wsap-term-linux` to the `./config.sh` command.
Keep the default `self-hosted`, `Linux`, and architecture labels.
Install and start the runner service as shown in the
[GitHub service guide](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/configure-the-application).

When the runner is online, enable the Linux job:

```sh
gh variable set WSAP_LINUX_CI --repo wenekar/wsap_term --body true
```

You can also set the variable in
**Settings > Secrets and variables > Actions > Variables**.
Then start CI from **Actions > CI > Run workflow**.
Remove the variable or set it to `false` to disable the Linux job.
