# Imported test suites

This directory holds test suites imported from external projects. These are
kept for reference and cross-checking against our own terminal emulator's
behavior. Every unsanitized Linux build runs the adapted Kitty parser
suite with the unit tests. `-Dkitty_tests=true` makes a missing suite an
error.

## kitty/

Vendored from <https://github.com/kovidgoyal/kitty>, commit `f13c8cd4`.

### Running the adapted suite

```sh
meson test -C build --suite kitty
```

This builds `kitty_bridge` (a shared library compiled from
the project's terminal sources), then runs the
Kitty parser test suite against it via a Python compatibility layer.

### Files

- `kitty/parser.py` — Kitty's VT/CSI parser test suite (`kitty_tests/parser.py`),
  with imports patched to use the local compatibility modules below.
- `kitty_parser.py` — Pure-Python VT parser that emits Kitty-style callback
  events while feeding every byte to the C terminal via `screen.write()`.
  Maintains state across calls for split-write (threading) tests.
- `screen.py` — `ctypes` wrapper around the bridge `libkitty_bridge.so` providing the Kitty
  `Screen`, `Cursor`, `Line`, `CellCursor`, and `Callbacks` API.
- `fast_data_types.py` — Pure-Python replacements for Kitty's C-extension
  helpers (base64, UTF-8 decode, constants).
- `kitty_harness.py` — `BaseTest`, `CmdDump`, and `parse_bytes` re-export.
- `__init__.py` — Package marker.

### Test status (14 tests)

All 14 tests pass. Features implemented for Kitty parity include DEC
Special Graphics charset translation (G0/G1, SO/SI),
LNM-default-off linefeed semantics, CSI insert-character and REP edge
cases, DECRQM mode reports, DECRQSS (DCS `$q`) and XTGETTCAP (DCS `+q`)
responses, DECCARA/DECSACE (CSI `$r` / `*x`), secondary device attributes
(CSI `=c`), XTWINOPS cell-size report (CSI `14t`), underline styles 0–5,
and SGR underline color (58/59) including colon-form subparameters.

### Deliberate differences from Kitty

The terminal follows xterm in two places. `parser.py` checks the xterm
result there, with a comment at each check.

- A cursor position report (CSI 6n) with a pending wrap reports the last
  column. Kitty reports the first column of the next line.
- HT only moves the cursor. Kitty writes a tab cell and blanks the cells
  it skips, so a tab erases the text under it. `spec-test` checks that a
  tab does not erase.
