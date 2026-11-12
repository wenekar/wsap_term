# Correctness audit

The 2026-10-15 audit found three defects that earlier tests missed.
The unit test suite showed 41 passing tests under AddressSanitizer and UndefinedBehaviorSanitizer.
The resize allocation-failure test skipped checks under sanitizers at that time.
A custom probe used linker wrappers to inject memory allocation failures into a sanitized build.
The main test suite now uses these wrappers and runs under sanitizers.

## Audit reproducer

Run the audit reproducer with Meson, a C compiler, and GNU `ld`:

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

`tests/correctness_probe.c` inspects internal state.
The probe includes `src/snapshot.c` to read private arena counters.
It returns a non-zero exit code if any finding reproduces.

The audit sandbox disabled LeakSanitizer.
Address and undefined-behavior checks remained active.
Run the unit suite in this configuration with:

```sh
ASAN_OPTIONS=detect_leaks=0 meson test -C build-audit --suite unit \
  --num-processes 4 --print-errorlogs
```

## Primary findings and fixes

### Combining mark loss on snapshot allocation failure

- **Fault**: `arena_add()` in `src/snapshot.c` returned only the base code point when `realloc` failed. The update reported success and cached the row key. Subsequent updates never restored the missing combining marks.
- **Observed**: The probe fed `e` followed by U+0301 and failed the next `realloc`. Two successive snapshot captures reported a cluster length of 1 instead of 2.
- **Fix**: Snapshot allocation failures now return false and preserve the previous snapshot. The snapshot allocates memory before mutating internal fields. A retry performs a normal incremental update.

### Snapshot arena record leak during scrolling

- **Fault**: Viewport scrolling moved `row_clusters` pointers without adding discarded row records to `arena_dead`. Memory compaction therefore missed discarded records.
- **Observed**: On an 8-column by 24-row terminal with history capped at 5 lines, the probe fed `e` with U+0301 and CRLF after each line:
  ```text
  after 1000 lines: arena_words=3000 dead_words=0 history_rows=5
  after 10000 lines: arena_words=30000 dead_words=0 history_rows=5
  ```
- **Fix**: Viewport scrolling now marks discarded cluster records as dead. Compaction reclaims this memory. Snapshot memory remains bounded by viewport dimensions and the compaction threshold.

### State corruption on failed height shrink

- **Fault**: `wsap_vtcore_resize_internal()` in `src/screen.c` scrolled content and moved the cursor before allocating replacement screen buffers. When this allocation failed, the function returned false after modifying the active screen state.
- **Observed**: An 8x4 terminal contained rows A through D. Shrinking height to 2 rows failed during screen buffer allocation:
  ```text
  before failed height shrink: top=A cursor_y=3 history=0
  after failed height shrink: resize=0 height=4 top=C cursor_y=1 history=2
  ```
- **Fix**: Height shrink allocates replacement screen buffers before moving rows or updating the cursor. A failed allocation returns false and leaves terminal state unchanged.

### Audit validation summary

Dedicated regression tests now verify these three behaviors:
- Prolonged scrolling maintains bounded snapshot memory.
- Snapshot allocation failures preserve state and allow recovery.
- Failed height shrinks preserve terminal state on normal and alternate screens.

All 43 unit tests and the standalone probe passed in debug builds and sanitized builds (ASan and UBSan).

## Follow-up audit: 2026-10-28

Fuzzing and systematic allocation failure tests found six additional faults:

| Fault | Root cause and fix | Regression check |
| --- | --- | --- |
| Fast CSI parser parsed only six digits. Truecolor values of 1,000,000 or greater produced incorrect colors. | The fast parser routes large color parameters to the base parser. | `write-fuzz-test` (large numbers) |
| A 255-byte OSC string or 256-byte DCS string dropped data when its terminator arrived in a later write. | The buffer marks strings truncated only when bytes are discarded. | `write-fuzz-test` (buffer edge strings) |
| `wsap_vtcore_scan_printable_span` produced inconsistent results across SIMD levels for `hi <= 0x20`. | The function returns 0 for this input range. | `public-helpers-test` (all SIMD levels) |
| Erase display (ED 2) read stale cells from an ASCII row and dropped history rows. | ED 2 now reads current text from ASCII rows. | `write-fuzz-test` (plain text lines) |
| Line streaming on a single-row screen passed a null pointer to `memcpy`. | The stream copies data only when the tail buffer is non-empty. | `model-test` (sanitized runs) |
| Insert mode with margins expanded rows past the right margin and exposed erased cells. | Row length now expands only to the right margin. | `term-test` (margin insert) |

## Follow-up audit: 2026-11-07 (Fast paths)

Integrating fast paths from wsap_term-ng uncovered six faults:

| Fault | Root cause and fix | Regression check |
| --- | --- | --- |
| Fast `CSI H`, `CSI 1;1H`, and short SGR forms (`CSI H m`, `CSI 1 H m`, `CSI d m`) ignored origin mode offsets. | Origin mode routes these sequences to the base parser. Only the general fast branch parses other final bytes with mode guards. | `spec-test` (origin mode), `term-test` (fast CSI) |
| `CSI 1;1H` read one byte past write buffer bounds when a write chunk ended in `1;1`. | The parser verifies four available bytes before reading. | `term-test` (split color writes) |
| Trailing semicolons in compound SGR sequences (`CSI 48;5;2;4m`) leaked `4m` as screen text. | The parser requires final character `m` on second color sequences. | `term-test` (fast CSI), `model-test` |
| Fast paths modified pen state before validating complete sequences. Cancel bytes after incomplete sequences left corrupt pen state. | The fast path validates complete sequences before changing pen state. | `term-test` (cancel bytes), `write-fuzz-test` |
| Staged snapshot updates reset arena lengths to zero and corrupted cluster text. | Both staged and direct snapshot paths read arena lengths from the plan. | `snapshot-failure-test` |
| Direct snapshot paths reserved arena capacity per unique cluster instead of per cell. Repeated clusters exhausted reserved space. | Direct snapshot paths reserve space for each cell in new rows. | `snapshot-failure-test` |

Modes 1, 7, 12, 25, 1004, 2004, and 2026 produce identical state in fast and base parsers.
Mode 7 clears pending line wraps in both parsers.
SGR shortcuts without measurable performance benefits were removed.

## Follow-up audit: 2026-11-07 (Left and right margins)

Testing left and right margins (DECLRMM and DECSLRM) against xterm uncovered nine faults:

| Fault | Root cause and fix | Regression check |
| --- | --- | --- |
| Wide characters at the right margin wrapped to full row width and exposed erased cells. | Row length extends only to the margin pad cell. | `term-test` (wide character wrap), `write-fuzz-test` |
| IL, DL, SU, SD, line feeds at bottom margin, and RI at top margin moved full screen rows. | Scrolling operations modify only cells between active margins. IL and DL outside margins do nothing. LF and RI stop at margin boundaries. | `term-test` (margin scrolling) |
| Line feed bursts in a single write scrolled the margin area by N rows at once. This produced mismatched row lengths and history reflow. | Margin areas scroll one row at a time. | `term-test` (burst line feeds), `write-fuzz-test` |
| Margin scrolling preserved row wrap flags. Blank scrolled rows joined following rows during reflow. | Margin scrolling clears wrap flags on modified rows. | `write-fuzz-test` |
| Cursor movement commands (CR, BS, CUF, CUB, HPR, HT, NEL, CNL) ignored margins. | Movement commands stop at margins when the cursor starts inside the margins. | `term-test` (margin motion) |
| Text written right of the right margin wrapped in some code paths. | Text written right of the right margin extends to the rightmost column. | `term-test` (margin motion) |
| With DECLRMM enabled, `CSI s` saved the cursor, ICH/DCH modified cells outside margins, and DECSTBM homed to column 0 in origin mode. | `CSI s` sets margins when DECLRMM is set. ICH and DCH outside margins do nothing. DECSTBM homes to the left margin. | `term-test` (margin commands) |
| DECSED and DECSEL (`CSI ? J`, `CSI ? K`) performed no action. | Without protected cells, DECSED and DECSEL execute standard ED and EL erasures. | `term-test` |
| DECRQM reported mode 1048 as unknown. | DECRQM reports mode 1048 as reset. | `term-test` |
