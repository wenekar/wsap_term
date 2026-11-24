# wsap_term

wsap_term is a terminal emulator for Linux and macOS. It supports tabs and lets you change fonts, colors, and themes from the Settings window.

| Platform | Window system | Rendering |
|---|---|---|
| Linux | Wayland | Vulkan with CPU fallback |
| macOS 14 or later | Cocoa | Metal |

## Build and run

Install the prerequisites for your platform from the [build guide](docs/build.md), then run `make` from the repository root.

| Platform | Launch command |
|---|---|
| Linux | `./build/bin/wsap_term` |
| macOS | `./build-homebrew/bin/wsap_term` |

**Local installation:** `make install` installs under `~/.local` by default. The [installation guide](docs/build.md#installation) explains where to launch the installed app.

## Using the terminal

| Task | Shortcut or guide |
|---|---|
| Open Settings | Linux: `Ctrl+Shift+,`; macOS: `Cmd+,` |
| Change fonts, colors, or themes | [Settings guide](docs/settings.md) |
| Display inline images | [SIXEL and Kitty graphics](docs/graphics.md) |
| List command-line options | `wsap_term --help` |
| Set up terminal capabilities on a remote host | [SSH guide](docs/ssh.md) |

## Development

**Test suite:** `make test`

Start with the [development guide](docs/development.md) for the source layout and test workflow. The [documentation index](docs/README.md) links to the architecture notes, debugging tools, and CI setup.

## License

[MIT](LICENSE)
