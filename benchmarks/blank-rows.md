# Blank rows as a storage state

Ryzen 7 9700X, GCC, `-O3 -march=native -flto`, Linux.

## Goal

A blank row stored a full width of cells before this pass.
Scroll cleared the recycled row with cell stores.
A whole-row erase did the same.
ED 2 scanned stored cells to find the last used row.

Blank is now a metadata state, like the raw rows of the lazy-row pass.

## Design

`term_row_meta.pad` has two new bits:

- `TERM_ROW_BLANK`: the cells of the row are stale. The row reads as blank
  cells. `raw_attr[slot]` holds the erase fg and bg for the slot.
- `TERM_ROW_TEXT`: the row can hold a glyph above U+0020. Write paths set
  the bit. ED 2 uses it to find the last used row. It does not scan cells.

`term_row_settle` expands a blank row on first access. It fills the row
with the recorded erase colors and sets `TERM_ROW_TAIL_PADDED`.

These paths mark a row blank and store no cells:

- `term_screen_scroll_up` for the recycled bottom row.
- `erase_span` for a pristine whole-row erase. The shrink case writes no
  cells either. Cells past the new `len` are invisible.
- `term_screen_erase_all`, `term_screen_erase` modes 0 and 1, EL 2, ED 3,
  and the grow-rows path of `term_resize` go through `erase_row`.
- `term_screen_set_alternate` marks the fresh alternate rows blank.
- `term_create` marks the initial rows blank.

Blank rows have `len` 0, so `reflow` reads no cells for them. The text bit
rides into the produced meta, so ED 2 history survives a resize.

An erase with non-default colors makes a full-width colored row. It is not
a blank row. `len` becomes `cols` and the cells are written. A non-pristine
erase on a blank row writes only the erased span plus the blank-color
prefix. The tail stays stale past `len`.

## The text bit

ED 2 pushes rows 0 through the last used row to scrollback. Rows that
contain only spaces do not count. The old code scanned stored cells for
`c > ' '`. The new code scans `TERM_ROW_TEXT` in the meta dwords.

The write paths set the bit when they store a glyph above U+0020:

- `put_glyph`, for printable codepoints.
- The ASCII chunk writers, through `ascii_span_has_text`.
- `store_raw_row`, for the bulk stream path.
- `term_text_run` callers, through `cells_have_text` on the written cells.
- DECALN.

The bit is conservative. A partial erase keeps the bit when a prefix stays.
A spaces-only prefix can then extend history once. Whole-row erases and
`{0}` meta stores clear the bit exactly.

## Writers and stale cells

A blank row must become concrete before a writer touches it. The cursor
row goes through `row_cells`, so `sync_output_cursor` expands it. Erase
commands on the cursor row resync `out->row` after they run. Delete and
insert chars materialize the row on entry. `term_screen_scroll_region`
moves blank rows as metadata and copies `raw_attr` with the meta.

`raw_text` and `raw_attr` are keyed by ring slot and shared by the two
screens. `term_screen_set_alternate` settles all deferred rows of the
saved screen on entry. The alternate screen then owns that storage.

## Measurement

`term-write-bench`, 512 iterations, 7 samples, pinned to CPU 5.
MiB/s median of the whole-buffer case.

| case | before | after |
| --- | ---: | ---: |
| erase | 420.8 | 421.7 |
| editor | 5867 | 5941 |
| dch | 460.7 | 461.3 |
| cursor | 715.2 | 713.2 |
| manyline | 4035 | 4018 |
| ascii | 7700 | 7734 |

The headless cases show no large change. Their erase loops already skipped
`len` 0 rows, and the cursor row is materialized again on the next write.
The gain is in the removed work: full-screen erases and scroll recycles no
longer touch the cell ring. That helps cases the fixed driver does not
isolate, such as `clear` on a large screen with content.

## Risks

- A writer that touches stale cells under a blank meta loses the write.
  All erase entry points resync the cursor row. Tests cover split writes,
  scroll reads, colored erases, and resize after erase.
- `TERM_ROW_TEXT` is conservative after partial erases. A prefix of spaces
  can keep a row eligible for ED 2 history. This over-preserves at most
  blank-looking rows.
- Blank rows on the saved normal screen are flushed on alternate entry.
  `raw_attr` slots are then free for the active screen.
