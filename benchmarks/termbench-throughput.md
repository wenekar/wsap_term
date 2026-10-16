# TermMark Throughput: Second Optimization Pass

## Scope and baseline

Ryzen 7 9700X, GCC 16.2.1, `-O3 -march=native -flto`.

This pass focuses on bulk text throughput, line scrolling efficiency, attribute
caching, and vector scanning paths across sustained input streams.

## Headless results

Five interleaved before/after runs, CPU 4, 4096 iterations per case (about
256 MiB), median MiB/s. The same benchmark driver was linked against both
snapshots. Warmup and data generation are outside the timer.

| Case | Before | After | Throughput gain |
| --- | ---: | ---: | ---: |
| longline | 2236.65 | 5571.75 | 149.1% |
| manyline (raw LF) | 800.28 | 1131.24 | 41.4% |
| ascii (fixed CRLF lines) | 2534.08 | 4691.90 | 85.2% |
| utf8 | 1112.34 | 1228.67 | 10.5% |
| cjk | 2410.57 | 2799.12 | 16.1% |
| colors | 661.71 | 715.07 | 8.1% |
| fg-char | 1015.47 | 1027.82 | 1.2% |
| fgbg-char | 1120.40 | 1219.13 | 8.8% |
| cursor | 601.27 | 679.44 | 13.0% |
| erase | 311.28 | 342.74 | 10.1% |
| random | 137.28 | 157.31 | 14.6% |

LongLine and ManyLine use a deterministic 64-KiB letter buffer; ManyLine selects
LF with probability approximately 1/27.

## Live terminal results

Three interleaved runs of each executable, pinned to CPUs 4 and 5, Wayland,
default dimensions (1056x714), `--no-blink`, fast pipe enabled.

Median TermMarkV2 Small reported seconds:

| Case | Before | After |
| --- | ---: | ---: |
| ManyLine | 0.1233 | 0.1092 |
| LongLine | 0.0448 | 0.0279 |
| FGPerChar | 0.0250 | 0.0244 |
| FGBGPerChar | 0.0432 | 0.0427 |
| Total | 0.2369 | 0.2039 |

Total time fell 13.9%. Live LongLine throughput rose from 1.3960 to 2.2417 GiB/s.

## Validation and reproduction

```sh
meson test -C build
meson test -C build --benchmark term-write-bench --verbose
taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench all 42 65536 4096
taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench longline 42 65536 16384
```
