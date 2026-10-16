# Settings window

Open **Settings** from the command palette or the terminal context menu.
On Linux, press `Ctrl+Shift+,`.
On macOS, press `Cmd+,`.
The form opens in its own window.
A second request reuses that window.
The settings window stays windowed when terminals start fullscreen.
Its text size does not change with the terminal font size.

The form lives in `src/app/config/settings.c`.
The app opens its window in `open_settings()` (`src/app/app.c`).
`settings_mount()` then shows the form on that window.
The app passes a `settings_host` to the form.
The form reads the config through this host.
The host applies and saves each change through app callbacks.
The option table is in `src/app/config/config.c`.
The value parser is in `src/util/options.c`.
The window and terminal layers do not include app headers.

## Controls

The form shows every public config option.
Use the category buttons to select a group.
Search matches names, config keys, help text, categories, and current values.
Press `Ctrl+F` or `Cmd+F` to focus search.

Theme and font controls have a text field and a dropdown.
The font dropdown lists installed monospace families.
The theme dropdown lists the theme catalog.
A text field accepts a custom font pattern or CSS path.
Other choice controls list every valid value.

Each dropdown has search and a scroll track.
Use Up, Down, and Enter to choose a value.
Press Escape or click outside to cancel.
Focus returns to the control after the dropdown closes.
Tab stays inside an open dropdown.
The main form also has a scroll track.
Tab brings the next field into view.
Page Up and Page Down scroll the form.

The palette fields accept `#rrggbb`, `#rgb`, and `rgb:rr/gg/bb`.
Choose the Default theme to use the config palette.
A named theme supplies its own palette entries.

Click **Config file** to open the active file with the system file handler.
The font and color help links open in a browser.

## Save and cancel

Edits stay in the form until Save.
Invalid fields show an error and disable Save.
Save updates only changed keys in the file selected at startup.
It keeps comments, unknown keys, and unchanged settings.
It updates duplicate keys and option aliases.
It writes a temporary file and then renames it over the config file.
A failed write leaves the old file and runtime settings intact.
The form shows the error and keeps the draft.

Press `Ctrl+S` or `Cmd+S` to save a valid draft.
Reset all puts the default values in the draft.
Cancel or Escape closes the window and discards unsaved edits.
Closing the settings window does not close terminal windows.

Theme, palette, font size, and cursor changes apply to open terminals.
Font size changes preserve each tab's zoom offset.
Font family, hinting, and antialiasing changes apply to open terminals.
Shell command and history changes apply to new tabs.
Window size, fullscreen, and vertical sync changes apply to new windows.
Environment variables and command arguments still override the file at startup.

## UI API

`ui_dropdown` is a reusable control in `src/ui/ui.h`.
It copies its labels and values.
Set an overlay parent with stack layout before opening it.
A user selection emits `UI_ACTION_CHANGE` with the original item index.
A programmatic value change emits no action.
Destroying the control also releases its popup.

`ui_scrollview_set_scrollbar()` turns the scroll track on or off. It is on by default.
CSS styles the `scrollbar` and `scrollbar-thumb` parts.
The inspector reports the track and thumb rectangles.
Scrollviews reveal focused descendants.

## Checks

`settings-window-test` covers search, validation, save errors, dropdowns,
reset, cancel, and window reuse.
`ui-dropdown-test` covers selection, modal focus, search, and cancellation.
`settings-test` covers config saves and value metadata.
`look-test` covers palette changes and theme cache refresh.
Use the UI inspector to run lint and capture screenshots.
