# GitHub Actions CI

The workflow is in `.github/workflows/ci.yml`.
It runs on pushes to `main`, on pull requests, and on manual starts.
It skips changes that touch only files that CI does not read.
The `paths-ignore` list in the workflow names these files.
Add a path there only if the build and regular tests do not read it.
Start CI manually from **Actions > CI > Run workflow**.
The jobs run on GitHub-hosted runners.
The checkout step does not keep GitHub credentials.

Each job runs `bash tools/ci/build.sh`.
The script configures a release build in `build/ci`.
It uses the default CPU target of the compiler.
It disables LTO to avoid whole-program links for each test binary.
The script checks the include graph, builds the app, and runs regular tests.
The Linux run includes the Kitty parser tests.
CI does not run benchmarks or display integration tests.
Each job uploads Meson logs and keeps them for seven days.

## macOS runner

The macOS job runs on `macos-14` (Apple Silicon).
It installs these Homebrew packages:

```sh
brew install llvm meson ninja pkgconf yyjson harfbuzz icu4c rust
```

The build script selects Homebrew LLVM and its archive tools.
Metal tests use the Apple Silicon GPU on the runner.
The signing test uses temporary Keychains.
It checks that changed builds keep the same certificate-bound identity.
It also checks that a different signer cannot meet the identity requirement.
The test does not change certificate trust or the privacy database.

## Linux runner

The Linux job runs on `ubuntu-24.04`.
It installs GCC 14, Wayland libraries, HarfBuzz, ICU, FreeType, and fonts.
It also installs `libvulkan-dev` and `glslc` to build and test the Vulkan renderer.
The fonts are JetBrains Mono, DejaVu, Noto, Noto CJK, and Noto Color Emoji.
It uses the CMake and Ninja tools of the runner image.
It installs Meson with pip and compiles yyjson from source.
It updates the stable Rust toolchain for the AccessKit source fallback.
Both jobs build the pinned AccessKit C library with Meson.
The runner keeps the default fontconfig settings of the image.
These settings select RGB subpixel antialiasing.
The font and render tests set their own rendering mode.
The regular tests do not need a display server.
