# Documentation

This index links to the user and developer guides for `wsap_term` and its libraries.

| Guide | Contents |
| --- | --- |
| [Build](build.md) | Prerequisites, targets, options, and installation. |
| [macOS updates](macos-updates.md) | Local signing and release updater requirements. |
| [Settings](settings.md) | Controls, validation, and saved values. |
| [SSH](ssh.md) | Remote terminfo and color setup. |
| [Development](development.md) | Architecture, tests, and benchmarks. |
| [CI](ci.md) | Runner setup and workflow controls. |
| [Tab ownership](tab-ownership.md) | Tab model, content lifecycle, and window transfers. |
| [UI library](ui-library-state.md) | Widgets, actions, layout, and ownership. |
| [Build a window screen](ui-screen-guide.md) | Screen creation, mounting, styling, and debugging. |
| [UI inspector](ui-debugging.md) | Layout checks, screenshots, and input injection. |
| [Font API](font-api.md) | Glyph ownership, caches, and thread rules. |
| [Data layout](data-layout.md) | Cell layout and cache line constraints. |
| [Fast PTY](fastpty.md) | Transport, stream order, and backpressure. |
| [High-refresh displays](high-refresh.md) | Frame stages, pacing, and presentation feedback. |
| [Terminal core](../subprojects/term/README.md) | Standalone builds and embedding. |

The [benchmarks](../benchmarks/README.md) directory contains performance reports.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Application code and libraries. |
| `assets/` | Runtime assets: scripts, themes, stylesheets, fonts, icons, and terminfo. |
| `tests/` | Tests grouped by source subsystem. |
| `subprojects/term/` | Standalone terminal core and core tests. |
| [`subprojects/wsap-framework/`](../subprojects/wsap-framework/README.md) | UI toolkit, renderer, and window binding |
| `subprojects/yasl/` | Symlink to the native platform library vendored inside `wsap-framework` |
| `benchmarks/` | Benchmark programs and measurement reports. |
| `scripts/` | Inspector and debug clients. |
| `tools/` | Build, install, package, and maintenance tools. |
