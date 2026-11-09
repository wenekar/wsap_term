# Next agent

Read this file before you start.
Do the tasks in order.
Remove each task when it is done.
Delete this file and its pointer in `AGENTS.md` when no task remains.

## State

The three feature worktrees are complete.
The core now supports DEC line sizes, shell OSC features, and character sets.
The app reports cell pixels and color scheme.
It draws blink, conceal, and underline colors.
Alternate scroll now obeys mode 1007 and handles large wheel deltas.
The core CI matrix now includes `ubuntu-24.04-arm`.
New behavior tests and allocation workloads cover the prior coverage gaps.

## 1. Draw double-size rows in the app

The core exposes `wsap_vtcore_snapshot_row_size`.
The app does not use it yet.
DECDWL and DECDHL text still uses normal glyph sizes in the left half.
Add horizontal scaling for double-width rows.
Add vertical scaling and clipping for the two double-height halves.
Keep cursor, selection, and pointer coordinates consistent with the display.
Add render behavior tests.
Check the result with the UI inspector.
Its lint must report no findings.

## 2. Run the Linux-only checks

The new ARM CI job has not run in this session.
Run the Linux unit and allocation-failure suites.
Check the history exact-row cache regrowth workload.
Check the region scroll that wraps a ring with more than 256 rows.
The new history workload can increase test time.
Check its 300-second timeout on CI.
Add OSC 133 input to the allocation sweep so it reaches mark storage.
Check failures in the new mark and snapshot row-size allocations.
Run the x86 ISA suites and the cross builds.
The macOS host has no Zig or GNU linker wrappers.

## 3. Keep the old model failure as a fixed input

An extended model run failed at seed 157406 on base `756c7b5`.
The failure involved line-size input and a cell color.
The generator now gives different bytes for that seed.
Recover the old input before deciding whether the issue remains.
Add a fixed regression case if it exposes a core or model fault.
Do not treat a passing new seed as proof that the old case is fixed.

## 4. Remove unused prompt state

`has_prompt` in `subprojects/term/src/internal.h` has no reader.
Remove the field and its assignments if it has no intended use.
Keep observable OSC mark behavior unchanged.

## Validation from this session

- Native macOS core suite: 35 passed, one cross-build check skipped.
- App unit suite: 108 passed.
- Core ASan and UBSan suite: 34 passed.
- Fixed libFuzzer smoke test: 1000 runs, no crash.
- UI inspector lint: no findings.
- Include graph: no layer violations or cycles.
- Clang coverage: 92.27% of lines and 87.64% of branches on macOS arm64.
- UTF-8 and CJK benchmarks stayed within the 3% noise threshold.

Use `nice -n 10` for builds and tests.
Set `WSAP_VTCORE_TEST_THREADS=2` for seed tests.
Benchmark only when the machine is quiet.
The unrelated macOS signing work in the main checkout is not part of this work.
