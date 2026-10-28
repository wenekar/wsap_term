# Testing

The core has several test layers. Each layer finds a different kind of fault.
Run all of them before a release. CI runs all of them on each change to the
core.

```sh
meson setup build -Dkitty=disabled
meson test -C build --print-errorlogs
```

## Suites

| Suite | Contents |
| --- | --- |
| `unit` | Behavior tests, model tests, allocation failures, symbols, headers |
| `isa` | The SIMD-sensitive tests again, at each lower x86-64 level |
| `amalgamation` | The unit tests against the generated single-file build |
| `portability` | `term-cross-test`: compiles for AArch64, macOS, and Windows with Zig |
| `packaging` | `term-package-test`: installs, then builds C, C++, and static consumers |
| `fuzz` | `terminal-fuzz-smoke`: 1000 fixed libFuzzer runs (needs `-Dfuzzing=true`) |

## Same result for every chunking

Output can arrive in chunks of any size. The terminal must reach the same
state for every split. Three tests check this:

- `write-fuzz-test` feeds generated output twice: once in large chunks and
  once byte by byte. It then compares every cell, the cursor, and the history.
  The generator includes numbers at the parser digit limits, strings at the
  buffer edges, and runs of plain LF and CRLF lines for the line streams. The
  `write_fuzz_seeds` option sets the seed count. The `isa` suite runs it again
  at each lower x86-64 level.
- `model-test` compares the terminal with a simple reference model.
- The libFuzzer target (`tests/fuzz/terminal_fuzz.c`) does the same
  whole-versus-split comparison on mutated input. It also compares replies
  and host events. Inputs that start with `0xff` instead run a sequence of
  API calls: resize, scroll, select, snapshot, and key encoding.

## Allocation failures

`alloc-failure-test` follows the SQLite method. It wraps `malloc`, `calloc`,
`realloc`, and `free` with linker wrappers. It runs a workload once for each
allocation N:

1. Fail allocation N only.
2. Fail allocation N and every allocation after it.

Each run must not crash or leak. When memory returns, a full reset (`ESC c`)
must give the same screen as a new terminal. Selection, links, and snapshots
must work again. There are two workloads. The small one covers the common
API. The large one uses more than 512 columns and 256 rows, long OSC and DCS
strings, many graphemes, and a line longer than one history page. Together
they reach almost every allocation site in `src/`. When you add an
allocation, make sure that a workload reaches it.

The wrappers need GNU `ld --wrap`. The test is not built on macOS or in
Clang coverage builds, because their linkers or runtimes do not allow it.

## Sanitizers and static analysis

CI runs the `unit` suite with AddressSanitizer and UndefinedBehaviorSanitizer
for GCC and Clang. To check uninitialized reads, build with Clang and
`-Db_sanitize=memory -Db_lundef=false -Db_lto=false`.

GCC `-fanalyzer` reports false positives for the screen slabs and the aligned
allocator. Check a new analyzer report against those before you act on it.

## Fuzzing

```sh
CC=clang meson setup build-fuzz -Dkitty=disabled -Dfuzzing=true
meson test -C build-fuzz --suite fuzz
python3 tools/fuzz_smoke.py build-fuzz/tests/fuzz/terminal-fuzz assets/fuzz \
    --artifacts build-fuzz/artifacts --seconds 600
```

The source corpus and dictionary are in `assets/fuzz/`. The script copies the
corpus, so a run does not change it. CI fuzzes for ten minutes and keeps any
crash input as a build artifact. Add each fixed crash as a regression case to
the generator in `tests/fuzz_gen.h` or to a unit test.

## Coverage

```sh
CC=clang meson setup build-cov -Dkitty=disabled --buildtype=debug -Db_lto=false \
    -Dc_args='-fprofile-instr-generate -fcoverage-mapping' \
    -Dc_link_args=-fprofile-instr-generate
LLVM_PROFILE_FILE="$PWD/cov/%p-%m.profraw" \
    meson test -C build-cov --suite unit --no-suite amalgamation
llvm-profdata merge -sparse cov/*.profraw -o cov/all.profdata
llvm-cov report $(for t in build-cov/tests/*-test; do printf -- '-object %s ' "$t"; done) \
    -instr-profile=cov/all.profdata -ignore-filename-regex='(tests/|unicode/)'
```

Use coverage to find behavior that no test exercises. Then write a test for
that behavior through the public API. Do not write tests that only touch
lines.
