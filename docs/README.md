# Documentation

| Guide | Contents |
| --- | --- |
| [Build](build.md) | Build targets, options, installation, and archives. |
| [Settings](settings.md) | Controls, validation, and saved values. |
| [SSH](ssh.md) | Remote terminfo and color setup. |
| [Development](development.md) | Architecture, tests, and benchmarks. |
| [CI](ci.md) | Runner setup and workflow controls. |
| [UI library](ui-library-state.md) | Widgets, actions, layout, and ownership. |
| [UI inspector](ui-debugging.md) | Layout checks, screenshots, and input injection. |
| [Font API](font-api.md) | Glyph ownership, caches, and thread rules. |
| [Data layout](data-layout.md) | Cell layout and cache line constraints. |
| [Fast PTY](fastpty.md) | Transport, stream order, and backpressure. |
| [Terminal core](../subprojects/term/README.md) | Standalone builds and embedding. |

Performance reports are in [benchmarks](../benchmarks/).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | App code and libraries. |
| `assets/` | Installed scripts, themes, fonts, icons, and terminfo. |
| `tests/` | Tests grouped by source subsystem. |
| `subprojects/term/` | Standalone terminal core and its tests. |
| `subprojects/yasl/` | Native platform library. |
| `benchmarks/` | Benchmark programs and measurement reports. |
| `scripts/` | Inspector and debug clients. |
| `tools/` | Build, install, package, and maintenance tools. |
