# GitHub Actions CI

The workflow is `.github/workflows/ci.yml`.
It runs on pushes to `main`, pull requests, and manual starts.
Start CI manually from **Actions > CI > Run workflow**.
The jobs run on GitHub-hosted runners.
The checkout step does not keep GitHub credentials.

Each job runs `bash tools/ci/build.sh`.
The script configures a release build in `build/ci`.
It uses the compiler's default CPU target and enables LTO.
It checks the include graph, builds the app, and runs the regular tests.
The Linux run includes the Kitty parser tests.
CI does not run benchmarks or display integration tests.
Each job uploads Meson logs and keeps them for seven days.

## macOS runner

The macOS job runs on `macos-14` (Apple Silicon).
It installs these Homebrew packages:

```sh
brew install llvm meson ninja pkgconf yyjson harfbuzz icu4c
```

The build script selects Homebrew LLVM and its archive tools.
Metal tests use the Apple Silicon GPU on the runner.

## Linux runner

The Linux job runs on `ubuntu-24.04`.
It installs GCC 14, build tools, Wayland libraries, HarfBuzz, ICU, FreeType, and fonts.
It installs Meson with pip and compiles yyjson from source.
The regular tests do not need a display server.
