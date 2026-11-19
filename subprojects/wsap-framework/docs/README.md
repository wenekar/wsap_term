# Documentation

| Guide | Contents |
| --- | --- |
| [Build](build.md) | Prerequisites, options, build, install, and pkg-config use. |
| [Font API](font-api.md) | Glyph ownership, caches, and thread rules. |
| [UI library](ui-library-state.md) | Widgets, actions, layout, and ownership. |
| [Build a window screen](ui-screen-guide.md) | Screen creation, mounting, styling, and debugging. |
| [UI inspector](ui-debugging.md) | Layout checks, screenshots, and input injection. |
| [High-refresh displays](high-refresh.md) | Frame stages, pacing, and presentation feedback. |

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Framework libraries, lowest layer first. |
| `subprojects/yasl/` | Native platform library. |
| `assets/ui/` | UI stylesheets. |
| `assets/fonts/` | Default fonts. |
| `tests/` | Framework tests. |
| `tools/` | Build, CI, and maintenance tools. |
