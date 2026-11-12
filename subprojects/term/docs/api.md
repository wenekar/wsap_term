# API guide

Include `<wsap_vtcore/wsap_vtcore.h>`.
The public header compiles as C11 and C++.
Functions use C linkage.

## Create and free

Zero-initialize `struct wsap_vtcore_options`.
Passing `NULL` selects default options.
A zero width or height selects 80 columns and 24 rows.
Zero scrollback selects `WSAP_VTCORE_SCROLLBACK_DEFAULT`.
`WSAP_VTCORE_SCROLLBACK_MAX` is the maximum scrollback limit.
Zero theme selects built-in colors.
A NULL or empty name selects `wsap_vtcore`.
Zero cursor style selects a block cursor (`WSAP_VTCORE_CURSOR_BLOCK`).
The reserved field requires zero initialization.

`wsap_vtcore_new()` creates a terminal instance.
The function returns `NULL` for invalid options or memory allocation failures.
The core copies configuration options except `callbacks.ud`.
The host retains ownership of `callbacks.ud` pointer targets.
`wsap_vtcore_free()` releases a terminal instance.
Calling core functions on a freed terminal causes undefined behavior.

Internal allocation failure preserves terminal state validity.
Functions with boolean return types return false on allocation failure.
Functions without failure return values drop unallocatable output without crashing.
A full reset (`ESC c`) restores initial screen state after memory becomes available.
`alloc-failure-test` verifies core behavior under allocation failures.

## Threads and callbacks

An internal writer lock serializes core operations on each terminal instance.
`wsap_vtcore_lock()` and `wsap_vtcore_unlock()` expose this lock for batch operations.
The lock supports recursive locking on the same thread.
Each lock call requires a matching unlock call on that thread.
Concurrent destruction of a terminal instance causes undefined behavior.
Snapshot objects operate independently from terminal instances.
Multiple threads can read the same snapshot concurrently when no thread updates or frees it.

Callbacks execute synchronously during core operations.
Callback text pointers reference temporary buffers that become invalid when the callback returns.
The terminal holds its internal lock during callback execution.
Callbacks must not invoke mutating operations (`wsap_vtcore_feed`, `wsap_vtcore_resize`, `wsap_vtcore_free`).
The recursive lock does not permit parser re-entry.
Queue deferred mutations until enclosing calls return.
The `reply` callback delivers terminal response bytes and their byte length.
The callback must copy or transmit response bytes before returning.

## Feeding output

`wsap_vtcore_feed()` supplies output bytes to the terminal parser.
A NULL byte pointer is valid only when `len` is zero.
`WSAP_VTCORE_FEED_ONLCR` is the only supported flag.
The function returns false for a NULL terminal, an invalid byte pointer, or unsupported flags.
The function returns true when the parser accepts the byte span.
A true return does not guarantee support for every sequence or success of every internal allocation.
The core does not retain input buffers after the function returns.
The parser accepts arbitrary chunk boundaries, including splits inside multibyte characters or escape sequences.

## Character sets

The core decodes input as UTF-8.
It supports ISO 2022 character sets from DEC terminals.
A character set mapping modifies only printable ASCII codes (0x21 to 0x7E).
Space (0x20), DEL (0x7F), and non-ASCII code points do not change.

- `ESC ( F`, `ESC ) F`, `ESC * F`, and `ESC + F` designate 94-character sets into G0, G1, G2, or G3.
- `ESC - F`, `ESC . F`, and `ESC / F` designate 96-character sets into G1, G2, or G3.
- SI activates G0 in GL. SO activates G1 in GL.
- `ESC n` (LS2) and `ESC o` (LS3) activate G2 or G3 in GL.
- `ESC N` (SS2) and `ESC O` (SS3) select G2 or G3 for the next character only. U+008E and U+008F provide equivalent C1 controls. Intervening control sequences do not cancel a pending single shift. Combining marks join the previous cell without consuming the single shift.
- `ESC ~`, `ESC }`, and `ESC |` (LS1R, LS2R, LS3R) activate character sets in GR. Bytes from 0x80 decode as UTF-8, so GR mappings do not alter displayed text. The core retains GR designations for DECSC and DECCIR.

| Final byte | Character set |
| --- | --- |
| `B` | ASCII |
| `0` | DEC Special Graphics |
| `<` or `% 5` | DEC Supplemental Graphics |
| `>` | DEC Technical |
| `A` (96-character designator) | ISO Latin-1 Supplemental |
| `A` | United Kingdom |
| `4` | Dutch |
| `C` or `5` | Finnish |
| `R` or `f` | French |
| `Q` or `9` | French Canadian |
| `K` | German |
| `Y` | Italian |
| `E`, `6`, or `` ` `` | Norwegian and Danish |
| `% 6` | Portuguese |
| `Z` | Spanish |
| `H` or `7` | Swedish |
| `=` | Swiss |

National Replacement Character Sets (NRCS):

- National sets other than UK require NRCS mode (DECNRCM, `CSI ? 42 h`). Without NRCS mode, these designators select ASCII. Disabling NRCS mode preserves current designations.
- The UK set replaces `#` with `£` only in NRCS mode.
- NRCS mode does not alter UTF-8 decoding.
- DECRQM reports mode 42 state. Full reset (RIS) clears mode 42. Soft reset (DECSTR) preserves mode 42.

Character set mapping rules:

- DEC Supplemental Graphics maps codes to DEC Multinational characters at `code + 0x80`. ISO Latin-1 Supplemental maps codes to `0x80 + code`. The core maps both sets in GL.
- Unassigned DEC Technical positions (including summation components 0x38 to 0x3B) map to U+2426 (reversed question mark).
- The parser ignores unknown set designators and designators with two intermediate bytes. C0 controls terminate designator sequences immediately. VT500 sets (such as `% 2` and `" 4`) are unsupported.
- DECSC saves G0 through G3, GL, and GR. DECRC restores them. DECSC does not save active single shifts. DECRC preserves pending single shifts. Alternate screen switches (modes 47, 1047, 1049) save and restore character sets with cursor state.
- DECCIR restore (`DCS 1 $ t`) sets single shifts, GL, GR, and all four designations. Bit *n* of the Scss field indicates a 96-character set in G*n*.
- Soft reset (DECSTR) and full reset (RIS) restore G0 through G3 to ASCII, set GL to G0, set GR to G2, and cancel pending single shifts.
- REP repeats the previous character as displayed on screen without remapping. DECFRA does not remap its fill character.

## Snapshots and text

`wsap_vtcore_snapshot_new()` creates a snapshot instance.
`wsap_vtcore_snapshot_free()` releases a snapshot instance.
`wsap_vtcore_snapshot_update()` captures current terminal state into the snapshot.
The function returns false for NULL arguments or memory allocation failures.
View structures, row buffers, palettes, and selection spans belong to the snapshot.
Pointers from snapshot getters remain valid until the next update call or snapshot destruction.
A failed update preserves previous contents, but can relocate internal buffers.
Re-fetch getter pointers after every update attempt.

A snapshot owns an independent copy of terminal state.
It remains valid after releasing its source terminal instance.
Getters read snapshot data without acquiring the terminal writer lock.
Multiple threads can read one snapshot concurrently.
Updating or freeing a snapshot requires exclusive access.
One snapshot instance can capture different terminal instances across successive updates.

Cells with `text & WSAP_VTCORE_TEXT_CLUSTER` hold cluster handles instead of scalar code points.
`wsap_vtcore_snapshot_cluster()` copies code points for a cluster handle into a caller-supplied buffer.
The function returns total cluster length, even when capacity is smaller.
A normal cell returns one code point.
The function returns zero for invalid handles or NULL snapshot pointers.
`wsap_vtcore_snapshot_cluster_direct()` returns a direct pointer to cluster code points and sets cluster length.
Cluster handles belong only to the current snapshot version.
Do not retain cluster handles across snapshot updates.

Cell width defines grid column spans:
- Width zero marks a continuation cell of a wide character. Continuation cells emit no text.
- Width one marks a single-column cell. Render the code point or cluster once.
- Width two marks a double-column cell. Render the code point or cluster once across two columns.

`wsap_vtcore_snapshot_row_size()` returns the line size of a view row:
- `WSAP_VTCORE_LINE_SINGLE`: standard single-width line.
- `WSAP_VTCORE_LINE_DOUBLE_WIDTH`: double-width line (DECDWL). Draw cells in the first half of the columns at double width. Remaining columns are blank.
- `WSAP_VTCORE_LINE_DOUBLE_TOP`: top half of a double-height line (DECDHL). Render upper halves of glyphs at double height and double width.
- `WSAP_VTCORE_LINE_DOUBLE_BOTTOM`: bottom half of a double-height line (DECDHL). Render lower halves of glyphs at double height and double width.
History rows and invalid row indices always return `WSAP_VTCORE_LINE_SINGLE`.
Line size modifications alter the row key.

The view and its palette belong to the snapshot.
`wsap_vtcore_view_color()` resolves a `wsap_vtcore_color` value to an ARGB integer using the view palette.

## Reports and host data

The core emits report responses through the `reply` callback.
Certain reports require host configuration:

- `wsap_vtcore_set_cell_pixels()` sets cell dimensions in pixels: `x` is width and `y` is height. `CSI 14 t` reports text area in pixels. `CSI 16 t` reports cell size in pixels. Zero or negative values indicate unknown pixel dimensions; reports then give zero pixels. Mouse mode 1016 (SGR pixels) does not use this setting; it reports coordinates directly from `wsap_vtcore_mouse_event.pixel`.
- `wsap_vtcore_set_color_scheme()` sets color scheme preference. `CSI ? 996 n` requests the active scheme. The core replies with `CSI ? 997 ; 1 n` for dark mode or `CSI ? 997 ; 2 n` for light mode. With `WSAP_VTCORE_COLOR_SCHEME_UNKNOWN`, the core compares theme foreground and background luminance. It reports light mode when background luminance exceeds foreground luminance. Otherwise, it reports dark mode. The core ignores invalid enum values.

Autonomous report notifications:

- Mode 2031: When mode 2031 is active, the core emits `CSI ? 997` when the effective color scheme changes. Only `wsap_vtcore_set_color_scheme()` and `wsap_vtcore_set_theme()` trigger this report. Dynamic palette changes (OSC 11) do not trigger it.
- Mode 2048: When mode 2048 is active, the core emits `CSI 48 ; rows ; cols ; height ; width t` upon enabling the mode, after size-changing resizes, and upon cell pixel size updates. Pixel values report as zero when cell dimensions are unknown.

Other reports requiring no host data:
- `CSI 18 t` reports text area size in characters.
- `CSI 19 t` reports display screen size in characters.
- `CSI ? 6 n` (DECXCPR) reports cursor position and page 1, respecting origin mode (DECOM).
- `CSI = c` (DA3) reports unit identification `00000000`.

Reports caused by host API calls execute synchronously during the call while holding the terminal lock.
Soft reset (DECSTR) and full reset (RIS) clear modes 2031 and 2048 without resetting host parameters.

## Titles

When configured with `callbacks.title`, the core stores the latest title set by OSC 0, 1, or 2.
Window titles and icon names share one common string storage.
`CSI 22 t` pushes the current title onto an internal title stack.
`CSI 23 t` pops the top title from the stack and delivers it through the `title` callback.
Parameters 0, 1, and 2 select the same title.
Other parameters and xterm stack index parameters perform no action.

The title stack holds up to 10 titles.
Pushing to a full stack discards the oldest title.
Popping an empty stack performs no action.
The core truncates titles at 4096 bytes without splitting multibyte UTF-8 sequences.
A title saved before setting any title restores as an empty string.
Full reset (RIS) clears the title stack and preserves the current title.
Without a title callback, title stack commands perform no action.

## Shell integration

The core parses OSC sequences for shell integration:

- `OSC 133 ; A` starts a prompt at the cursor row. A parameter other than `k=i` marks a secondary prompt, which the core ignores.
- `OSC 133 ; B` marks the start of command input.
- `OSC 133 ; C` marks the start of command output.
- `OSC 133 ; D` marks the end of command execution with an optional 32-bit exit code. The core ignores out-of-range or non-decimal exit codes.

A row can retain one prompt mark and one output mark.
Marks follow rows into history, through scrolls, and through grid resizes.
The alternate screen maintains no marks.
Full reset (RIS) clears marks on visible screen rows.
ED 3 removes marks from history rows.
Erasing an entire line clears marks on that line.

Row indices in `wsap_vtcore_find_prompt()` and `wsap_vtcore_command_at()` use viewport coordinates.
Row 0 represents the top row of the viewport.
Negative row indices reference history lines above the viewport.
Indices equal to or greater than viewport height reference lines below the viewport.
`wsap_vtcore_scroll(t, -row)` scrolls the specified row to the top of the viewport.
These functions return false on the alternate screen or for invalid arguments.
Returned command structures stay valid until the next terminal or viewport mutation.

`wsap_vtcore_find_prompt()` returns the nearest prompt row above (`dir < 0`) or below (`dir > 0`) a specified row.
The search excludes the start row itself.
`wsap_vtcore_command_at()` describes the command containing a specified row.
A command begins at a prompt row and ends before the next prompt row.
`cmd->finished` is true after `OSC 133 ; D` or after encountering a subsequent prompt.
`cmd->has_output` is true when `OSC 133 ; C` marked a row before the next prompt.
Command output ends at the cursor row.

Other shell integration sequences:

- `OSC 9 ; 4 ; state ; percent` reports command progress through `callbacks.progress`:
  - `WSAP_VTCORE_PROGRESS_REMOVE` (0): removes the progress state.
  - `WSAP_VTCORE_PROGRESS_NORMAL` (1): indicates normal progress.
  - `WSAP_VTCORE_PROGRESS_ERROR` (2): indicates an error state.
  - `WSAP_VTCORE_PROGRESS_INDETERMINATE` (3): indicates indeterminate progress.
  - `WSAP_VTCORE_PROGRESS_PAUSED` (4): pauses progress.
  Percent is -1 when the state provides no percentage value. A missing percent for state 1 defaults to 0. Percents above 100 clamp to 100. Non-decimal values or states above 4 are ignored. Progress reports do not trigger `callbacks.notify`.
- `OSC 9 ; 9 ; path` and `OSC 1337 ; CurrentDir=path` set the current working directory. `wsap_vtcore_cwd()` returns a newly allocated copy of this path. Enclosing quotes are stripped. The core stores at most 4096 bytes of a path.
- `OSC 1337 ; SetUserVar=name=base64` decodes a base64 value and delivers it to `callbacks.user_var`. The callback receives the decoded byte length and a terminating NUL character. Variable names support up to 255 bytes. Decoded values support up to 4096 bytes. The core rejects longer names, longer values, and invalid base64 text. Name and value pointers remain valid only during callback execution.

`callbacks.progress` and `callbacks.user_var` overlay the first reserved fields of `struct wsap_vtcore_options` to preserve struct size across versions.

## Cell styles

The `attrs` field holds SGR attributes in a 16-bit bitfield (`wsap_vtcore_attr`).
`WSAP_VTCORE_ATTR_BLINK` represents SGR 5 and SGR 6 without distinction between slow and rapid blink.
`WSAP_VTCORE_ATTR_CONCEAL` represents SGR 8.
Concealed cells preserve underlying text characters.
`wsap_vtcore_attr_underline()` extracts the active `wsap_vtcore_underline` style from the attribute bitfield.

`wsap_vtcore_snapshot_row_underline_colors()` returns an array of underline colors (SGR 58) for one view row.
The array provides one color value per column.
`WSAP_VTCORE_COLOR_NONE` indicates that the cell has no custom underline color.
Underlines without custom colors use the foreground color of the cell.
A NULL return indicates that no cell in the row uses an underline color.
Returned pointers follow getter lifetime rules.

DECSCA protection controls cell preservation during selective erases (DECSED, DECSEL, DECSERA).
Standard erase commands (ED, EL, ECH) erase protected cells.
DECSERA replaces characters with spaces while preserving attributes and colors.

The core interns hyperlink and underline color pairs in a style table.
Cells, history runs, and reflow operations retain style table identifiers.
The style table holds up to 8191 concurrent style pairs.
When the table fills, newly written text drops link and underline color attributes until unused entries are swept.

## Private modes

Reverse wraparound alters Backspace (BS) and Cursor Backward (CUB) behavior.
Modes 45 and 1045 require autowrap mode (DECAWM, mode 7).
Moving left past the left margin moves the cursor to the right margin of the row above:
- Mode 45 enters a row only when that row wrapped into the current row. It stops at the start of a wrapped line. It can cross the top margin.
- Mode 1045 enters any row. At the top margin, it wraps to the bottom margin. Above the top margin, the cursor stops at the top of the screen.
- After filling a line, the cursor retains a pending wrap state. The first backward step clears the pending wrap without moving the cursor.

Mode 1007 (alternate scroll) defaults to disabled.
`wsap_vtcore_modes.alternate_scroll` reports this setting.
`wsap_vtcore_input_encode_mouse()` converts wheel events into Up and Down cursor keys when all conditions match:
- Mode 1007 is active.
- The alternate screen is active.
- Mouse reporting is disabled.
- The event is a button press or scroll event.
The encoder emits one key per line of `scroll_lines`, and at least one key.
Application cursor mode (DECCKM, mode 1) emits `ESC O A` and `ESC O B` instead of `ESC [ A` and `ESC [ B`.

Private mode save and restore:
- `CSI ? Pm s` (XTSAVE) saves states for specified DEC private modes.
- `CSI ? Pm r` (XTRESTORE) restores saved mode values.
Each mode retains one saved state.
Saving a mode overwrites its previous saved state.
The core ignores unknown or fixed modes.
XTRESTORE alters a mode only when its current value differs from the saved value.
Modes without saved values do not change.
Mode 1048 saves and restores cursor state.
Mouse mode numbers are saved individually.
`CSI s` and `CSI r` without `?` retain standard ANSI/DEC meanings (SCOSC or DECSLRM, and DECSTBM).

Keypad mode 66 (DECNKM) mirrors `ESC =` (application keypad) and `ESC >` (numeric keypad).

DECRQM (`CSI ? Pm $ p`) reports status 3 (permanently set) for immutable features:
- Mode 2 (DECANM): ANSI mode is permanent; VT52 mode is unsupported.
- Mode 1046: alternate screen switching is always available.
- Mode 2027: grapheme cluster joining (UAX #29) is always active.
- ANSI mode 12 (SRM, `CSI 12 $ p`): local echo is permanently disabled.
Mode 1048 reports status 2 (reset).

Soft reset (DECSTR) and full reset (RIS) reset modes 45, 1045, and 66.
Both resets preserve mode 1007.
Soft reset preserves XTSAVE values.
Full reset clears all XTSAVE values.

## Input encoding and allocated strings

Input encoders (`wsap_vtcore_input_encode_key`, `wsap_vtcore_input_encode_mouse`, `wsap_vtcore_input_encode_paste`) write into caller-provided byte buffers.
Each function returns `struct wsap_vtcore_input_result`:
- `status == WSAP_VTCORE_INPUT_BYTES`: The buffer contains a valid encoded sequence of `len` bytes.
- `status == WSAP_VTCORE_INPUT_NONE`: The event produces no terminal input bytes.
- `status == WSAP_VTCORE_INPUT_BUFFER_FULL`: The provided buffer capacity is insufficient. Partial sequences may be written; discard buffer contents.
- `status == WSAP_VTCORE_INPUT_INVALID`: Input event arguments or mode parameters are invalid.

Functions returning heap-allocated strings (`wsap_vtcore_cwd`, `wsap_vtcore_pointer_shape`, `wsap_vtcore_selection_text`, `wsap_vtcore_link_at`, `wsap_vtcore_history_text`):
- Return a newly allocated NUL-terminated string, or NULL on empty values or allocation failure.
- Release returned strings with `wsap_vtcore_text_free()`.
- Do not pass returned pointers to standard `free()`.

## Invalid arguments

Functions validate input parameters individually.
Query functions return zero or NULL values when given NULL pointers or out-of-range coordinates.
Mutating functions reject invalid arguments without altering terminal state.
Documented function contracts define specific return behaviors for invalid parameters.
