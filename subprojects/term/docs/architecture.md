# Architecture

The core has no renderer, PTY, font system, or window dependency.
Only headers under `include/wsap_vtcore/` form the public interface.
Do not include private source headers from a host.

## Source map

Paths in this table are relative to `src/`.
Each source directory lists its own build inputs.

| File | Responsibility |
| --- | --- |
| `term.c` | Modes, cursor options, focus events, scrollback limits |
| `term_state.c` | Combined grapheme, history, and mode compilation unit |
| `core.c`, `core.h` | Shared write primitives |
| `screen.c`, `screen.h` | Screen ring, editing, resize, and reflow |
| `hist.c`, `hist.h` | Logical-line storage and page arena |
| `hist_view.c`, `hist_view.h` | Viewport anchors and history lookup |
| `snapshot.c` | Copied views, row keys, and scroll movement |
| `selection.c` | Selection, coordinate mapping, and UTF-8 export |
| `gstore.c`, `gstore.h` | Per-terminal grapheme storage |
| `text.c` | Byte decoding and control dispatch |
| `text_run.c`, `text_run.h` | SIMD UTF-8 text runs |
| `fastcsi.c` | Common CSI fast paths |
| `input.c` | Key, mouse, and paste encoding |
| `escape.c` | ESC, CSI, DCS, and OSC state machine |
| `pen.h` | Graphic attributes |
| `osc/dispatch.c` | OSC dispatch and palette queries |
| `osc/link.c` | OSC 8 hyperlinks |
| `osc/clipboard.c` | OSC 52 clipboard requests |
| `osc/misc.c` | Titles, paths, and notifications |

## Write core

Text and control sequences use shared primitives in `core.c`.
These primitives handle printing, line feeds, margins, and cursor movement.
Fast paths must preserve the same visible behavior as the base path.
See `src/simd.h` for runtime target rules.

A write of 1 KiB or more of complete printable ASCII lines in newline mode
takes a line stream. The stream sends the lines that leave the screen to
history in one batch. The remaining rows point into one
copy of the input. Code that reads a row must convert an ASCII row first
(`screen_get_row`) or read its text, because its cells are stale.

## Screen and history

The screen is a circular ring of rows.
Rows that leave the screen enter the logical-line history store.
History stores UTF-8 text and delta-encoded attribute runs.
The alternate screen has no history.

A viewport anchor identifies a logical line and a cell offset before wrapping.
Output keeps the anchor on the same retained text.
Resize maps the cell offset to a segment at the new width.
Eviction clamps the anchor to retained history.
A long line can lose its prefix when it exceeds the history byte budget.
The anchor accounts for those removed cells.

Relative scroll walks from the anchor.
Absolute scroll uses a circular Fenwick index of wrapped row counts.
Snapshot capture builds this index when a history view first needs it.
The write path does not maintain the index.
A width change starts wide lines with estimated row counts.
Snapshot capture refines the counts for lines in the view.

## Snapshot publication

A snapshot owns copied cells, colors, selection spans, and grapheme data.
It does not borrow terminal storage.
The host must not update a snapshot while a reader uses it.
The host can use separate snapshot objects for separate readers.

Row keys identify row contents for incremental rendering.
A scroll result lets a renderer shift retained pixels and repaint changed rows.
The host shapes and draws text. The core does not skip shaping under load.

Snapshot capture can update private history query metadata.
It therefore takes the terminal writer lock.
Getters read the completed snapshot without that lock.
See [api.md](api.md) for pointer lifetimes and allocation-failure rules.
