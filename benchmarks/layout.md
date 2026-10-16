# Library layout checks

Date: 2026-10-11.
Baseline: `e214767`.
Host: Linux on an AMD Ryzen 7 9700X.
Compiler: GCC 16.2.1.
Build: release, `-O3`, LTO, debug symbols, and `isa=native`.
CPU boost stayed enabled.

The baseline source was copied to `/tmp` with `git archive`.
It used the same build options as the final build.
The existing `build/` directory was kept.

## Render

The comparison used seven pairs of runs.
The baseline and final builds ran in turn.
The first build in each pair alternated.
No build or other benchmark ran during these samples.
These samples did not use CPU affinity.
Each table value is the median frame time in milliseconds.

| Case | Baseline | Final |
|---|---:|---:|
| flood-bytes | 0.591 | 0.590 |
| flood-printable | 0.256 | 0.255 |
| flood-code | 0.031 | 0.031 |
| flood-log | 0.035 | 0.034 |
| plain | 0.019 | 0.019 |
| ligatures | 0.024 | 0.023 |
| mixed | 0.053 | 0.052 |
| random | 0.822 | 0.818 |
| scroll | 0.034 | 0.034 |

The ranges overlap for every case.
These samples show no clear render slowdown.
The small changes do not prove a speed gain.
The final flood and scroll cases use `render_draw()` with output history.
One more pair after the final scanner change confirmed the same hashes.
Its flood-bytes times were 0.590 ms and 0.593 ms.
Its mixed-text times were both 0.051 ms.

Every pair produced the same five pixel hashes:

| Case | Hash |
|---|---|
| plain | `66e883dd4967e08e` |
| ligatures | `07863d50ddaf2d45` |
| mixed | `6510b5dde2559808` |
| random | `bced82804c352be6` |
| scroll | `5294a81127feeb6f` |

## Terminal writes

The final comparison used three pairs of full `term-write-bench` runs.
The baseline and final builds ran in turn.
The first build in each pair alternated.
All six runs used CPU affinity on CPU 0.
No build or other benchmark ran during these samples.
Each run used the default seed, 512 writes, and seven samples per case.
It tested both whole-buffer chunks and 4096-byte chunks.
The terminal size was 120 columns by 40 rows.
The main parser loops keep their SIMD implementations.

Each table value is the median throughput in MiB/s.
The table includes the largest measured drops.

| Case | Chunk | Baseline | Final |
|---|---:|---:|---:|
| ascii | 65493 | 7436.23 | 7692.95 |
| utf8 | 65520 | 885.01 | 883.05 |
| utf8var | 65527 | 186.05 | 186.92 |
| utf8var | 4096 | 184.51 | 187.10 |
| cjk | 65533 | 1588.45 | 1584.01 |
| colors | 65527 | 733.24 | 721.53 |
| cursor | 65505 | 799.73 | 769.61 |
| cursor | 4096 | 790.47 | 751.52 |
| erase | 4096 | 451.90 | 437.00 |
| lf-burst | 4096 | 47242.23 | 44406.14 |
| random | 65536 | 55.90 | 55.53 |

The first scan export reduced the mixed UTF-8 rate by about 14%.
CPU counters showed the same instruction count with more cycles.
The public wrapper now keeps the private SIMD helper separate.
Its hot-code hint restores the mixed UTF-8 rate with GCC LTO.
The final paired medians differ by 0.5% and 1.4% for its two chunk sizes.

Some other parser cases remain slower in this comparison.
The 4096-byte LF burst is 6.0% slower.
The cursor cases are 3.8% and 4.9% slower.
The 4096-byte erase case is 3.3% slower.
Their sample ranges do not overlap.
The parser algorithms and SIMD loops did not change.
Further parser throughput work is outside this layout plan.

## Live terminal

`meson compile -C build term-live-bench` completed on headless Weston.
Its default perf event produced no samples on this host.
That run does not provide a useful CPU profile.

A second check ran `wsap_term --command 'cat /dev/random' --stats-ms 1000`.
It used the same headless compositor.
`perf record -e cpu-clock -F 1000 --call-graph dwarf` sampled the app for two seconds.
The decoded profile contained about 2.8 MB of sample text.
The app drew 122 frames before it stopped.
Its final mean frame rate was 25.7 FPS at 1056 by 714 pixels.
This check proves that the live flood path runs and can be profiled.
It is not a before-and-after throughput comparison.

## Other checks

The regular Linux suite passed 101 of 101 tests.
The address and undefined-behavior sanitizer suite passed 101 of 101 tests.
The layer gate reported zero violations and zero cycles.
The UI inspector reported no lint findings on five screens.

macOS build checks, native tests, and Metal benchmarks are deferred.
The user will run them after implementation.
