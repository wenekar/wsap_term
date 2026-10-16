# Cursor and erase throughput

## Scope

The baseline includes the existing lazy row changes.
The benchmark driver is unchanged by this pass.
The requested delete case is named `erase` in this driver.
It sends ED and EL commands. It does not send DCH commands.

## Changes

The CSI fast path now writes the ASCII text that follows a command.
It uses the existing SIMD cell writer.
It also handles ED commands without a second parser pass.

ED 2 scans row lengths in groups to skip empty rows.
The scan has AVX-512, AVX2, NEON, and scalar paths.
Each load stays within one span of the row ring.
The scan still checks stored cells before it adds rows to history.
Rows that contain only spaces do not extend that history.

## UTF-8 experiment

A trial wrote four ASCII cells at a time within mixed UTF-8 runs.
It reduced mixed UTF-8 throughput by about 11% in the paired test.
The change was removed.
The mixed case has short ASCII spans and frequent changes in byte width.
The existing block decoder needs 16 consecutive three-byte scalars.
It cannot process most of this mixed input as a block.
No UTF-8 decoder change remains in this pass.

## Measurement

Host: AMD Ryzen 7 9700X. Compiler: GCC 16.2.1.
Build flags: `-O3 -march=native -flto`.
Both binaries use the same benchmark driver.
Each sample writes 1024 buffers. Each invocation takes seven samples.
The process is pinned to CPU 4.
Before and after runs alternate order.
The whole-buffer and 4096-byte cases run separately.

The table uses the median of three invocation medians.

| Case | Chunk | Before MiB/s | After MiB/s | Change |
| --- | ---: | ---: | ---: | ---: |
| utf8 | 65520 | 1115.38 | 1124.79 | +0.8% |
| utf8 | 4096 | 1088.02 | 1101.04 | +1.2% |
| colors | 65527 | 553.61 | 680.07 | +22.8% |
| colors | 4096 | 551.60 | 676.59 | +22.7% |
| fg-char | 65535 | 3299.93 | 3201.43 | -3.0% |
| fg-char | 4096 | 3186.30 | 3099.76 | -2.7% |
| cursor | 65505 | 651.33 | 714.42 | +9.7% |
| cursor | 4096 | 646.94 | 703.07 | +8.7% |
| erase | 65520 | 290.84 | 422.49 | +45.3% |
| erase | 4096 | 291.29 | 420.10 | +44.2% |
| random | 65536 | 209.83 | 204.33 | -2.6% |
| random | 4096 | 208.32 | 201.50 | -3.3% |

Mixed UTF-8 remains near the baseline.
The small gain does not establish a decoder speedup.
Per-character foreground colors lose about 3%.
Random-byte input loses about 3%.
The retained change trades those small losses for the cursor and erase gains.
A trial checked for the next CSI before the text span.
It did not recover the losses and reduced the cursor gain. It was removed.

These measurements cover parsing and screen updates.
They do not measure end-to-end terminal throughput.

## Checks

- Terminal and Unicode tests pass with native x86, AVX2, and baseline x86 builds.
- Regression tests cover each input split, wide cells, colors, wrapping, and history.
- Erase tests check history retention at every screen row.
- AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer tests pass.
- The render benchmark completes.
- The live random-input profile completes.
- The NEON path has not been tested on an Arm host.

## Commands

```sh
meson compile -C build term-write-bench
taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench all 42 0 1024 7
meson test -C build term-test unicode-test --print-errorlogs
meson test -C build/sanitize term-test unicode-test --print-errorlogs
meson test -C build --benchmark term-write-bench render-bench --verbose
meson compile -C build term-live-bench
```
