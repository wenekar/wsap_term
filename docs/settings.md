# Settings window

Open **Settings** from the command palette or the terminal context menu.
On Linux, press `Ctrl+Shift+,`.
On macOS, press `Cmd+,`.
The form opens in its own window.
A second request reuses that window.
The settings window stays windowed when terminals start fullscreen.
Its text size does not change with the terminal font size.

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
