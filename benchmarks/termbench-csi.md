# CSI Parameter Run Benchmark

Measured on a Ryzen 7 9700X, GCC 16.2.1, `-O3 -march=native -flto`.

This pass measures parameter-heavy escape sequences, such as truecolor commands
emitted before each character (two for FGBGPerChar).

## Results

Five interleaved before/after runs of `term-write-bench`, pinned to CPU 4.
Medians in MiB/s (higher is better):

| Case | Before | After | Change |
| --- | ---: | ---: | ---: |
| fg-char | 787.65 | 933.48 | +18.5% |
| fgbg-char | 836.78 | 1013.31 | +21.1% |
| colors | 638.65 | 668.52 | +4.7% |
| cursor | 611.67 | 586.71 | -4.1% |
| erase | 316.58 | 305.10 | -3.6% |
| ascii | 2484.19 | 2486.32 | +0.1% |
| utf8 | 1113.48 | 1108.15 | -0.5% |
| cjk | 2409.52 | 2418.72 | +0.4% |
| random | 134.61 | 134.79 | +0.1% |

Headless TermMark run through pipe test, pinned to CPUs 4 and 5. Three
interleaved runs, median reported seconds:

| TermMarkV2 Small | Before | After |
| --- | ---: | ---: |
| FGPerChar | 0.0282 | 0.0247 |
| FGBGPerChar | 0.0499 | 0.0410 |
| Total | 0.2453 | 0.2273 |

These pipe measurements include producer and transport costs, but exclude
rendering and compositor work.

## Validation and reproduction

```sh
meson test -C build term-test
meson test -C build --benchmark term-write-bench --verbose
taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench fg-char
taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench fgbg-char
```
