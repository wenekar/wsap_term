# Correctness audit

The 2026-10-15 audit reproduced three issues that the existing tests missed.
The unit suite reported 41 passing test entries under AddressSanitizer and
UndefinedBehaviorSanitizer. At that time, the resize allocation-failure test
skipped its checks under sanitizers. The probe below used linker wrappers to
inject failures in a sanitized build. The resize test now uses the same
wrappers, so it runs under sanitizers too.

## Run the reproducer

From the repository root, with Meson, a C compiler, and a linker supporting
GNU `--wrap`:

```sh
meson setup build-audit --buildtype=debug -Db_lto=false \
  -Db_sanitize=address,undefined -Dkitty=disabled
meson compile -C build-audit -j 4
cc -std=c11 -g -UNDEBUG -fsanitize=address,undefined \
  -Iinclude/wsap_vtcore -Isrc tests/correctness_probe.c \
  build-audit/src/libwsap_vtcore.a \
  -Wl,--wrap=realloc -Wl,--wrap=calloc -lm -o build-audit/correctness-probe
ASAN_OPTIONS=detect_leaks=0 build-audit/correctness-probe
```

`tests/correctness_probe.c` prints the observed behavior. It intentionally
includes `src/snapshot.c` to inspect its private arena counters and links
against the static library. It is a diagnostic, outside the default suite;
it returns a nonzero status if any of the three findings still reproduces.

Leak detection was disabled because LeakSanitizer could not run in the audit
sandbox. Address and undefined-behavior checks remained enabled. To repeat
the unit suite in that environment:

```sh
ASAN_OPTIONS=detect_leaks=0 meson test -C build-audit --suite unit \
  --num-processes 4 --print-errorlogs
```

Kitty compatibility and cross-platform tests were outside this sanitized run.

## Findings

### Snapshot allocation failure silently loses combining marks

In `src/snapshot.c`, `arena_add()` returns the first code point when its
allocation fails. The update reports success and caches the row key, so a
later update does not restore the omitted code points.

The probe prints an `e` followed by U+0301, fails the next `realloc`, and
captures twice. Both captures report a cluster length of 1 instead of 2.
An allocation failure should report false and leave the snapshot empty,
as documented. A successful retry should capture both code points.

### Scrolling leaks snapshot arena records

The scroll optimization moves `row_clusters` without adding the discarded
rows' records to `arena_dead`. Compaction therefore misses those records.

On an 8-column, 24-row terminal with history capped at five lines, the probe
feeds `e` plus U+0301 and CRLF, capturing after each line. The original
output is:

```text
after 1000 lines: arena_words=3000 dead_words=0 history_rows=5
after 10000 lines: arena_words=30000 dead_words=0 history_rows=5
```

Snapshot arena storage should remain bounded by the viewport and compaction
threshold, independently of how many lines have scrolled past it.

### Failed height shrink changes terminal state

In `src/screen.c`, `wsap_vtcore_resize_internal()` scrolls and adjusts the cursor
before allocating the replacement screen. If that allocation fails, it
returns false after changing the terminal, violating the documented promise
that a failed resize leaves it unchanged.

The probe fills an 8-by-4 terminal with rows A through D and fails the
replacement screen allocation while shrinking to two rows. The original
result is:

```text
before failed height shrink: top=A cursor_y=3 history=0
after failed height shrink: resize=0 height=4 top=C cursor_y=1 history=2
```

After failure, the screen should still start with A, the cursor should
remain on row 3, and history should remain empty.

## Checks to retain

- Inject allocation failures and verify each API's documented failure state.
- Compare incremental snapshots with fresh captures, including grapheme text.
- Assert bounded storage during prolonged scrolling with a fixed viewport.

## Fixes

- Snapshot scroll accounting now counts discarded cluster records as dead,
  allowing compaction to reclaim them.
- Snapshot allocation failures now report false and leave the previous
  snapshot unchanged. Every allocation happens before a field changes, so a
  retry is an ordinary incremental update.
- Height shrink allocates the replacement screen before moving rows or
  changing the cursor, preserving state if allocation fails.

Dedicated regression tests cover sustained scrolling, snapshot allocation
failure and recovery, and failed height shrink on both normal and alternate
screens. The output above records the original failures for reference.

Validation after the fixes: all 43 unit test entries passed in an
unsanitized debug build and all 43 passed with address and undefined-behavior
sanitizers. The standalone probe also passed in both builds. Leak detection
remained disabled in the sandbox.

## Follow-up: 2026-10-28

The libFuzzer target, a sweep that fails each allocation in turn, and new
generator cases found these faults. Each fix has a regression check.

| Fault | Fix | Check |
| --- | --- | --- |
| The fast CSI path kept six digits of a parameter. A truecolor component of a million or more gave a different color than the base parser. | The fast path sends such a color to the base parser. | `write-fuzz-test` big-number cases |
| An OSC string of 255 bytes, or a DCS payload of 256 bytes, was dropped when its terminator came in a later write. | A full buffer marks the string truncated only when bytes are dropped. | `write-fuzz-test` buffer-edge strings |
| `wsap_vtcore_scan_printable_span` gave different results at different SIMD levels for `hi` up to 0x20. | The function returns 0 for that range. | `public-helpers-test` at each level |
| Erase display (ED 2) read the stale cells of an ASCII row. It could drop rows from history. | ED 2 reads the text of an ASCII row. | `write-fuzz-test` plain-line runs |
| A line stream on a one-row screen called `memcpy` with a null buffer. | The stream copies only a non-empty tail. | Sanitizer runs of `model-test` |
| Insert mode with left and right margins grew the row past the margin and showed erased cells. | The row length grows only up to the right margin. | `term-test` margin insert case |

One fault is still open. With left and right margins, insert mode, and
grapheme clusters together, different write splits can give different
screens. Add margin sequences to `tests/fuzz_gen.h` to reproduce it.
