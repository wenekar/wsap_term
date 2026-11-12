# Architecture

wsap_vtcore has no renderer, PTY, font system, or window dependency.
Headers under `include/wsap_vtcore/` form the entire public interface.
Private source headers in `src/` remain internal to the core.

## Source map

Paths in this table are relative to `src/`.
Each source directory lists its own build inputs.

| File | Responsibility |
| --- | --- |
| `term.c` | Modes, cursor options, focus events, host reports, scrollback limits |
| `term_state.c` | Combined grapheme, history, and mode compilation unit |
| `core.c`, `core.h` | Shared write primitives |
| `screen.c`, `screen.h` | Screen ring, editing, resize, and reflow |
| `marks.c`, `marks.h` | Prompt and output marks (OSC 133) and their history records |
| `rect.c`, `rect.h` | Rectangle edits (DECFRA, DECERA, DECCRA) and column shifts |
| `hist.c`, `hist.h` | Logical-line storage and page arena |
| `hist_view.c`, `hist_view.h` | Viewport anchors and history lookup |
| `snapshot.c` | Copied views, row keys, and scroll movement |
| `selection.c` | Selection, coordinate mapping, and UTF-8 export |
| `gstore.c`, `gstore.h` | Per-terminal grapheme storage |
| `text.c` | Byte decoding and control dispatch |
| `text_run.c`, `text_run.h` | SIMD UTF-8 text runs |
| `text/charset.c`, `text/charset.h` | ISO 2022 character sets: designations, shifts, and maps |
| `fastcsi.c` | Common CSI fast paths |
| `input.c` | Key, mouse, and paste encoding |
| `escape.c` | ESC, CSI, DCS, and OSC state machine |
| `pen.h` | Graphic attributes |
| `style.c`, `style.h` | Style table: links and underline colors of cells |
| `osc/dispatch.c` | OSC dispatch and palette queries |
| `osc/link.c` | OSC 8 hyperlinks |
| `osc/clipboard.c` | OSC 52 clipboard requests |
| `osc/misc.c` | Titles, the title stack, paths, and notifications |
| `osc/shell.c` | OSC 133 marks, OSC 9;4 progress, OSC 9;9, and OSC 1337 |

## Write core

Text parsing and control sequences use shared primitives in `core.c`.
These primitives process printing, line feeds, scroll margins, and cursor movement.
Fast paths preserve identical visible behavior to the scalar base path.
See `src/simd.h` for runtime target rules.

The `translate` bit is active when GL holds a non-ASCII character set or when a single shift is pending.
Print fast paths test only this bit.
When the bit is set, text routes through the base path to map characters through the designated set.
Character set state changes must use functions in `text/charset.h` or call `wsap_vtcore_charset_sync()`.

Writes of 1 KiB or larger with complete printable ASCII lines use a line stream.
The line stream accepts LF line endings in newline mode and CRLF line endings in other modes.
Both modes generate identical screen and history contents.
The stream flushes scrolled-off rows to history in a single batch.
Remaining visible rows reference a shared copy of input text.
Row access functions convert ASCII rows on demand through `screen_get_row()` to materialize cell attributes.

## Screen and history

The screen consists of a circular ring of rows.
Rows that scroll off the top enter the logical-line history store.
History stores UTF-8 text and delta-encoded attribute runs.
The alternate screen maintains no scrollback history.

A cell stores 16 bits of flags and a 16-bit pad field (`cell.h`).
The pad field stores blink, conceal, DECSCA protection flags, and a style identifier.
The style table maps the style identifier to an OSC 8 hyperlink and an underline color.
Identifier zero represents plain text without links or custom underline colors, avoiding table lookups.
History runs, text rows, and reflow operations copy the pad field without modification.
When the style table fills, a sweep pass reclaims unused identifiers.
The link table releases unused hyperlinks during the same sweep.

A viewport anchor records a logical line index and an unwrapped cell offset.
Terminal output keeps the anchor fixed to the same logical text.
Terminal resize maps the cell offset to the corresponding row segment at the new width.
History eviction clamps the anchor within retained history bounds.
When a line exceeds the history byte budget, eviction removes line prefix bytes.
The anchor adjusts its cell offset for removed prefix cells.

Relative scrolling traverses lines from the viewport anchor.
Absolute scrolling indexes rows with a circular Fenwick tree of wrapped row counts.
Snapshot capture constructs this index on the first history query.
The write path does not update the Fenwick index.
Width changes assign estimated row counts to wide lines.
Snapshot capture resolves exact counts for lines visible in the view.

## Snapshot publication

`struct wsap_vtcore_snapshot` owns copied cells, colors, selection spans, and grapheme clusters.
The snapshot does not borrow memory from the terminal instance.
A snapshot allows concurrent read access.
Updating or freeing a snapshot requires exclusive access.
Separate snapshot instances isolate independent reader threads.

Row keys uniquely identify row contents for incremental presentation.
Scroll metadata reports vertical row movements to permit pixel shifting and selective repainting.
The core provides text attributes and graphemes without rendering or shaping glyphs.
The core never alters text layout or skips shaping state under write load.

Snapshot capture queries internal history state and acquires the terminal writer lock.
Getter functions access snapshot data without acquiring the terminal lock.
See [api.md](api.md) for pointer lifetimes and allocation failure contracts.
