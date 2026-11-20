# Settings window

This guide explains how to edit and save terminal preferences in the “Settings” window.

| Entry point | Action |
|---|---|
| Command palette or terminal context menu | Choose “Settings” |
| Linux shortcut | `Ctrl+Shift+,` |
| macOS shortcut | `Cmd+,` |
The settings form opens in a dedicated window.
Subsequent requests reuse that window.
The settings window stays in windowed mode when terminals start fullscreen.
Terminal font size changes do not affect the settings window text size.

## Controls

The form shows all public configuration options.
Use the category buttons to select an option group.
The search box matches names, configuration keys, help text, categories, and current values.
Press `Ctrl+F` or `Cmd+F` to focus search.

Theme and font controls provide a text field and a dropdown menu.
The font dropdown lists installed monospace families.
The theme dropdown lists the theme catalog.
The text field accepts a custom font pattern or CSS file path.
Other dropdown controls list all valid values.

Each dropdown includes a search box and a scroll track.
Press Up, Down, and Enter to choose a value.
Press Escape or click outside the dropdown to close it.
Focus returns to the input control after the dropdown closes.
Tab stays inside an open dropdown.
The main form also has a scroll track.
Tab brings the next field into view.
Press Page Up or Page Down to scroll the form.

Color palette fields accept `#rrggbb`, `#rgb`, and `rgb:rr/gg/bb`.
Select the default theme to use palette settings from the configuration file.
A named theme supplies its own palette colors.

Click **Config file** to open the active file with the system file handler.
Click **Font help** or **Color help** to open documentation in your browser.

## Save and cancel

Edits remain in the form until you save them.
Invalid fields show an error and disable **Save**.
When you choose “Save,” the app updates changed keys in the configuration file it loaded at startup. The app updates duplicate keys and option aliases, and preserves comments, unknown keys, and unchanged settings. It writes a temporary file and renames the temporary file over the configuration file.
A failed write leaves the existing configuration file and runtime settings intact.
The form shows the error and keeps the draft.

Press `Ctrl+S` or `Cmd+S` to save a valid draft.
Click **Reset all** to load default values into the draft.
Click **Cancel** or press Escape to close the window and discard unsaved edits.
Closing the settings window does not close terminal windows.

Theme, palette, font, and cursor changes apply to open terminals immediately.
Font size changes preserve each tab zoom offset.
Shell command and history changes apply to new tabs.
Window size, fullscreen, and vertical sync changes apply to new windows.
Environment variables and command-line arguments override configuration file values at startup.
