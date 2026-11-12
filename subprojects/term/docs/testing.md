# Testing

The core includes multiple test layers.
Each layer detects a different class of defect.
Run all suites before each release.
CI runs all suites on each change to the core.

```sh
meson setup build -Dkitty=disabled
meson test -C build --print-errorlogs
```

## Test suites

| Suite | Contents |
| --- | --- |
| `unit` | Behavior tests, model tests, allocation-failure tests, symbol tests, and header tests. |
| `isa` | Re-runs SIMD-sensitive tests at each lower x86-64 level. |
| `amalgamation` | Runs the unit suite against the generated single-file source. |
| `portability` | Runs `term-cross-test` to compile for AArch64, macOS, and Windows with Zig. |
| `packaging` | Runs `term-package-test` to build C, C++, static, and amalgamation consumers. |
| `fuzz` | Runs `terminal-fuzz-smoke` for 1000 libFuzzer iterations (requires `-Dfuzzing=true`). |

## Chunking invariance

Input can arrive in chunks of any size.
The terminal state must remain identical for every split.
Three test targets check this invariant:

- `write-fuzz-test` feeds generated output twice: once in large chunks and once byte-by-byte.
  It compares every cell, the cursor position, and scrollback history between both runs.
  The generator tests numbers at parser limits, strings at buffer boundaries, and line feed streams.
  The `write_fuzz_seeds` option sets the seed count.
  The `isa` suite repeats this test at each lower x86-64 level.
- `model-test` feeds split input streams and compares state against a reference terminal model.
- `tests/fuzz/terminal_fuzz.c` feeds mutated input to whole and split streams.
  It verifies screen cells, history lines, replies, and host events.
  Inputs that begin with byte `0xff` execute an API sequence: resize, scroll, select, snapshot, and key encoding.

## Allocation failure injection

`alloc-failure-test` wraps `malloc`, `calloc`, `realloc`, and `free` with linker wrappers (`ld --wrap`).
It runs workloads under two failure modes for each allocation index N:

1. Fail only allocation N.
2. Fail allocation N and all subsequent allocations.

Each run must not crash or leak memory.
After memory returns, a hard reset (`ESC c`) must restore an empty terminal state.
Selections, links, and snapshots must work again after recovery.

The test provides two workloads:
- The small workload exercises common API operations.
- The large workload exercises wide screens (>512 columns, >256 rows), long OSC/DCS strings, many graphemes, and lines longer than one history page.

These workloads reach almost every allocation site in `src/`.
Make sure new allocation sites are reachable by one of these workloads.

The test requires GNU `ld --wrap`.
macOS and Clang coverage builds skip this test because their linkers or runtimes do not support function wrapping.

## Sanitizers and static analysis

CI runs the `unit` suite with AddressSanitizer and UndefinedBehaviorSanitizer on GCC and Clang.

To detect uninitialized reads with MemorySanitizer, configure with Clang:

```sh
CC=clang meson setup build-msan -Dkitty=disabled \
    -Db_sanitize=memory -Db_lundef=false -Db_lto=false
meson test -C build-msan --suite unit
```

GCC `-fanalyzer` reports false positives for screen slabs and the aligned allocator.
Check new analyzer reports against those known allocator patterns.

## Fuzzing

Configure the fuzzing build with Clang:

```sh
CC=clang meson setup build-fuzz -Dkitty=disabled -Dfuzzing=true
meson test -C build-fuzz --suite fuzz
python3 tools/fuzz_smoke.py build-fuzz/tests/fuzz/terminal-fuzz assets/fuzz \
    --artifacts build-fuzz/artifacts --seconds 600
```

The fuzz dictionary and seed corpus reside in `assets/fuzz/`.
The test script copies the corpus, so a run does not change original files.
CI fuzzes for ten minutes and preserves crash inputs as build artifacts.
Add each fixed crash to `tests/fuzz_gen.h` or as a unit test to prevent regressions.

## Code coverage

Measure test coverage with Clang source-based instrumentation:

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

Use coverage data to find behaviors that no test exercises.
Write new tests for those behaviors through the public API.
Do not write tests merely to touch lines.
