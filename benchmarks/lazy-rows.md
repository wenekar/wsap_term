# Lazy Row Materialization: Third Throughput Pass

Ryzen 7 9700X, GCC, `-O3 -march=native -flto`, Linux.

## Finding

The live LongLine case was bound by cell stores, not by parsing. The bulk
ASCII path wrote every wrapped line into the 16-byte-per-cell ring. With the
default 4096-line scrollback the ring is far larger than L2, so the write
stream went to L3. Headless `longline` against scrollback size:

| `WSAP_SCROLLBACK` | GiB/s (before) |
| --- | ---: |
| 4096 (default) | 7.48 |
| 1000 | 8.01 |
| 100 (ring fits L2) | 13.78 |

Termbench itself caps FGPerChar at about 4.0 GB/s and FGBGPerChar at about
4.4 GB/s on this machine (measured against `/dev/null`), because its timer
includes its own escape-sequence generation. Those two tests cannot report
more than that in any terminal.

## Change

`stream_ascii_lines` now stores each printable ASCII line as bytes in
`term->raw_text` plus one `term_row_raw` attribute record, and marks the row
`TERM_ROW_RAW`. Cells are expanded on first read through `row_cells()`,
`term_ring_row()`, `term_cell()`, reflow, and the link GC. Lines that scroll
straight into history never touch the cell ring.

`run-termbench.sh` also pins `wsap_term` to two CPUs instead of one. Thread
affinity is inherited, so a single-CPU pin made the pipe reader thread and
the paint worker compete with the parser and hid the overlap the reader
thread exists to provide.

## Headless results

`taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench <case> 42 65536 2048`,
median MiB/s:

| Case | Before | After | Change |
| --- | ---: | ---: | ---: |
| longline | 7457 | 29801 | +300% |
| ascii | 7015 | 7535 | +7.4% |
| manyline | 4174 | 4004 | -4.1% |
| fg-char | 3283 | 3306 | +0.7% |
| fgbg-char | 4152 | 4119 | -0.8% |
| others | | | within noise |

ManyLine is bound by per-row bookkeeping, not by stores.

## Live results

`./benchmarks/run-termbench.sh normal`, termbench on CPU 5:

| Case | Before (1 CPU pin) | Pin fix only | Pin fix + lazy rows |
| --- | ---: | ---: | ---: |
| ManyLine | 5.32 | 5.13 | 5.82 |
| LongLine | 4.69 | 4.84 | 5.99 |
| FGPerChar | 2.53 | 3.01 | 3.00 |
| FGBGPerChar | 2.93 | 3.28 | 3.30 |
| **TermMarkV2 Normal** | **4.03** | **4.26** | **4.71** |

Live LongLine is now bound by the transport (pipe copy at about 13.8 GiB/s
plus termbench's own `write()` cost), not by the terminal.

## Tried and rejected: branchless truecolor parsing

`parse_color_sequence` parses `R;G;Bm` with one branch per digit. A
branchless SSE2 variant was tried: one 16-byte load, `;`/`m`/digit masks,
field lengths from `tzcnt`, and a shift-aligned SWAR multiply per field.

Isolated, on random colors, it is 2.1x faster (8.7 ns → 4.1 ns per color).
In the terminal it is slower on every measured input:

| Input | Scalar | Branchless |
| --- | ---: | ---: |
| headless fg-rand (MiB/s) | 2912 | 2338 |
| headless fgbg-rand | 3792 | 2797 |
| live FGPerChar (GB/s) | 3.00 | 2.27 |
| live FGBGPerChar | 3.30 | 2.61 |

Reason: the branchless form makes the next-sequence pointer a true data
dependency (load → compare → movemask → tzcnt, about 10 cycles) on every
sequence. The branchy form lets the core speculate across sequences whenever
digit counts are predictable, and they are: termbench emits gradients
(`FrameIndex + Y + X`), and real programs use small palettes. Only fully
random colors defeat the predictor, and that is not the target workload.

The `fg-rand` and `fgbg-rand` cases in `term-write-bench` were added during
this experiment and are kept for future attempts. Note that the 64 KiB
buffer repeats every iteration, so even these are partly learnable by the
branch predictor.

## Validation and reproduction

```sh
meson test -C build term-test
ninja -C build subprojects/term/benchmarks/term-write-bench
taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench longline 42 65536 2048
./benchmarks/run-termbench.sh normal
```

Note: `benchmarks/*` are not default ninja targets. Rebuild them explicitly
before measuring, or a stale binary will report old numbers.
