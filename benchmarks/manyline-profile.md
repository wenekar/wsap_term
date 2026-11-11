# ManyLine profile

Recorded: 2026-11-11 (Europe/Istanbul)

This document measures where a ManyLine flood spends its time. It uses the
headless core (`term-write-bench`) and `sample(1)`. It also uses a scratch copy
of the core with cycle counters. The scratch copy is not in the tree.

## Setup

- Host: Apple M3 Pro, macOS 27.0.1, arm64. Six P-cores and six E-cores.
- Build: `build-homebrew`, release, `-O3`, LTO, NEON kernels.
- Feed shape, `manyline`: 65536 bytes from seed 42 with `% 27` choices. One byte
  in 27 is `\n`. For seed 42 one 64 KiB chunk holds exactly 2490 lines, so the
  mean line is 26.3 bytes with the terminator. The bench also sets
  `WSAP_VTCORE_FEED_ONLCR`, so the feed takes the LF stream path.
- Scrollback: 4096 lines. Screen: 120 by 40 cells.

Reproduce the throughput numbers:

```sh
ninja -C build-homebrew
build-homebrew/subprojects/term/benchmarks/term-write-bench manyline 42 65536 40000 1
build-homebrew/subprojects/term/benchmarks/term-write-bench manyline 42 4096 300000 1
sample term-write-bench 12 1 -file sample.txt   # while a long run is active
```

## Throughput

Median of 5 runs. The term-write-bench timing excludes the pty.

| Case | Chunk | MiB/s | ns/B |
| --- | ---: | ---: | ---: |
| manyline | 65536 | 7997 | 0.119 |
| manyline | 4096 | 5344 | 0.178 |
| ascii (57-byte lines) | 65536 | 9559 | 0.100 |
| longline (no newlines) | 65536 | 32193 | 0.030 |
| lf-burst (all blank lines) | 65536 | 36149 | 0.026 |

The host runs at 3.92 to 4.02 GHz, measured with a dependent add chain. The
tables below use 3.96 GHz. A byte costs 0.45 to 0.47 cycles at 65536-byte
chunks. A 26-byte line costs 12 to 13 cycles.

`termbench ManyLine` does not measure this path. termbench writes to stdout. The
consumer does the terminal work. The same 64 MiB run reported 0.063 GB/s into a
pipe and above 30 GB/s into `/dev/null`. Both numbers measure the transport, not
the core.

## Method

1. `sample <pid> 12 1` against a run of 3 million 64 KiB chunks. This gave
   10029 samples. The shares below use the parent chain, so a sample counts for
   the function that owns the hot instruction.
2. A copy of `subprojects/term` in a temporary directory holds timers. Each
   timer reads `cntvct_el0`, which ticks at 1 GHz on this host. The timers
   bracket the scan, the length check, the line walk, `flush_orig_rows`, the
   tail copy, the history push, and the screen row writes. More timers bracket
   the three parts of `wsap_vtcore_hist_push_ascii_lf`. The copies were
   discarded after the run.
3. The timer build measured 8339 MiB/s and the release build measured 7997
   MiB/s, so the timers cost less than 5% of the wall time. The two methods
   agree within about 2 points on every phase, except the descriptor stores.
   `sample` puts 23.7% on that loop and the timers put 18.8%. The samples land
   on the wide stores, so that row skews.

## Where the time goes at 65536-byte chunks

Median of 3 runs of 60000 iterations. Total: 7495 ns per 64 KiB chunk,
0.114 ns/B, 8339 MiB/s.

The phases below do not overlap. Each line of the chunk counts once.

| Phase | ns/chunk | ns/B | cycles/B | ns/line | cycles/line | Share |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| LF scan (`scan_ascii_lf`) | 4171.0 | 0.0636 | 0.252 | 1.68 | 6.6 | 55.7% |
| History push, from the stream | 2432.1 | 0.0371 | 0.147 | 0.98 | 3.9 | 32.5% |
| Line length check loop | 695.2 | 0.0106 | 0.042 | 0.28 | 1.1 | 9.3% |
| `flush_orig_rows` (39 screen rows) | 127.0 | 0.0019 | 0.008 | — | — | 1.7% |
| Screen row writes and cursor update | 37.2 | 0.0006 | 0.002 | — | — | 0.5% |
| Line walk and trailing-fragment scan | 25.7 | 0.0004 | 0.002 | — | — | 0.3% |
| Tail copy (39 rows) | 10.4 | 0.0002 | 0.001 | — | — | 0.1% |

The chunk holds 2490 lines and keeps 39 rows on the screen. The last four
rows above work on those 39 rows or run once per chunk, so their cost is not
comparable with the cost of a history line.

Inside the history push:

| Part | ns/chunk | ns/B | cycles/B | ns/line | cycles/line | Share of chunk |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Descriptor stores (`stream_lines`) | 1408.4 | 0.0215 | 0.085 | 0.57 | 2.2 | 18.8% |
| Page copy (`memcpy`) | 640.0 | 0.0098 | 0.039 | 0.26 | 1.0 | 8.5% |
| Eviction (`evict_for`) | 380.2 | 0.0058 | 0.023 | 0.15 | 0.6 | 5.1% |
| Push tail loop (non-stream rows) | 51.0 | 0.0008 | 0.003 | 0.02 | 0.1 | 0.7% |

The `sample` data gives the same picture:

| Leaf symbol | Samples | Share |
| --- | ---: | ---: |
| `scan_ascii_lf` | 5441 | 54.3% |
| `wsap_vtcore_hist_push_ascii_lf` (own code, mostly descriptor stores) | 2376 | 23.7% |
| `stream_ascii_lines` (length check loop) | 874 | 8.7% |
| `_platform_memmove` (page copy) | 796 | 7.9% |
| `evict_simple_run` and `evict_for` | 421 | 4.2% |
| `flush_orig_rows` and the rest | 107 | 1.1% |

Feed entry, the mutex, and the snapshot counter together hold about 0.1% of
the samples. They are not a factor at this chunk size.

## What each hot loop costs

Instruction counts come from `llvm-objdump` on the release binary.

### LF scan, 55.7%

`scan_ascii_lf_neon` reads 64 bytes per iteration. The compiled loop body holds
120 instructions, and it contains four bit-extraction tails of 10 instructions
each. A tail runs once per newline.

- 4 loads and 4 compares for `\n`, plus 12 instructions for the control-byte
  check. With the loop control this costs about 30 instructions per 64-byte
  block.
- Each 16-byte lane packs its mask with `and`, three `addp`, and `fmov`, then
  reads the popcount with `cnt`/`addv`. This costs about 14 instructions per
  lane that holds a newline. About 1.8 lanes per block hold one.
- Each newline costs 10 serial instructions: `rbit`, `clz`, `orr`, `add`,
  `strh`, and the `m &= m - 1` chain.

So one 64-byte block costs about 75 to 105 instructions for this feed. That is
1.2 to 1.6 instructions per byte. The measured cost per newline is 6.6 cycles.
The measured cost of the block loop alone is 0.029 ns/B. The regular-line cases
below give that number.

### History descriptor stores, 18.8%

`stream_lines` writes four `hline` entries (16 bytes each) per iteration. It
builds the entries in NEON registers and stores 64 bytes with two `stp` pairs.
The compiled loop holds about 40 instructions per 4 lines, or 10 instructions
per line. A vectorized divide (`umull2`/`uzp2`) computes `rows_narrow` inside
the same loop.

The measured cost is 2.2 cycles per line, or 0.57 ns. The stores are not
memory-bound: the ring stays in cache.

### Line length check, 9.3%

`stream_ascii_lines` walks every line once and compares
`ends[j + 1] - ends[j]` with the page limit. The loop is scalar and costs 7
instructions per line. It runs for all 2490 lines, so it costs 1.1 cycles per
line. An AVX-512 version of this check exists. There is no NEON or scalar SIMD
version, so the aarch64 build pays the loop in full.

### History page copy, 8.5%

One `memcpy` of about 26 bytes per line moves the text into the history page.
The total per 64 KiB chunk is about 65 KiB, so the copy moves one byte per input
byte. The measured rate is about 100 GB/s, which is L1 speed.

### History eviction, 5.1%

The ring holds 4096 lines and the chunk holds 2490 lines, so most pushes evict.
`evict_simple_run` keeps a block path of 16 lines. With 64 KiB chunks the block
path runs, and the cost is 0.6 cycles per line. The page-drop bookkeeping runs
in the same bucket.

## Chunk size changes the mix

Median of 3 runs at 4096-byte chunks. Total: 704 ns per chunk, 0.172 ns/B,
5547 MiB/s.

| Phase | Share |
| --- | ---: |
| LF scan | 37.8% |
| History push, from the stream | 24.8% |
| Rest, including `flush_orig_rows` | 18.2% |
| Line length check loop | 7.3% |
| Line walk and trailing-fragment scan | 5.5% |
| Screen row writes and cursor update | 4.4% |
| Tail copy | 1.8% |

Inside the history push at this chunk size: descriptor stores 10.7%, eviction
10.4%, page copy 8.2%, and push tail loop 4.6%. The sum is larger than the share
of the direct push, because `flush_orig_rows` pushes the 39 rows through the
same code. `flush_orig_rows` alone is 17.2% of the chunk.

A 4096-byte feed holds about 156 lines. The 39 rows on the screen must go to
history on each feed, so 25% of the lines take the row-flush path instead of
1.6% at 64 KiB. That path costs about 3.1 ns per row against 1.0 ns per line in
the bulk stream. Eviction also costs more: smaller batches use the per-line path
of `evict_simple_run` more often.

## The scan cost splits into a per-byte and a per-line part

Five synthetic cases use regular lines of a fixed length. They ran in the
instrumented copy with the same 64 KiB chunks.

| Case | Bytes/line | Lines per chunk | Total ns/B | ns/line (all) | Scan ns/line | Scan ns/B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| manyline | 26.3, random | 2490 | 0.114 | 3.00 | 1.68 | 0.0636 |
| line27 | 27, regular | 2427 | 0.096–0.112 | 2.59–3.02 | 1.31–1.53 | 0.049–0.057 |
| line16 | 16 | 4096 | 0.138 | 2.21 | 1.07 | 0.0667 |
| line32 | 32 | 2048 | 0.089 | 2.85 | 1.46 | 0.0458 |
| line64 | 64 | 1024 | 0.066 | 4.22 | 2.43 | 0.0380 |
| line100 | 100 | 655 | 0.058 | 5.80 | 3.58 | 0.0358 |

A least-squares fit on the five regular cases gives `scan = 0.591 ns per
newline + 0.0287 ns per byte` (`2.34 cycles per newline + 0.113 cycles per
byte`). The fit is within 4% of each regular case. Almost all of the cost is the
serial ten-instruction tail per newline.

ManyLine pays more than the fit predicts. The fit gives 3349 ns for 2490
irregular lines, and ManyLine measures 4171 ns. Its newline positions are not
regular, so the trip count of the per-lane tail loop is not predictable. That
branch cost adds 0.33 ns per line, or 1.3 cycles.

## What this rules out

- Eviction is not the limit. It is 5% of a 64 KiB feed, or 0.6 cycles per
  pushed line.
- Memory bandwidth is not the limit. The whole path touches each input byte
  about once for the scan and copies it once to the history page. The copy runs
  at L1 speed.
- The feed entry path is not the limit at 64 KiB chunks. It is about 0.1% of
  the time. At 4096-byte chunks the entry, the mutex, and the snapshot counter
  reach about 1.4%.
- Blank-line bursts are cheap. `lf-burst` reaches 36 GiB/s. The cost of
  ManyLine comes from text bytes and the descriptor per line, not from newlines
  as such.

## Limits of this measurement

- macOS has no `perf`. The shares come from 1 ms `sample` runs and from
  in-process counters. Both are wall-clock based.
- The CPU Counters template of `xctrace` needs no root, but its CPU-bottleneck
  mode reports sampled fractions, not cycle and instruction counts. This
  document does not use it.
- The timers sit in a scratch copy of the core. The copy keeps the same
  sources, but LTO and layout differ, so the absolute rate of the timer build is
  up to 5% higher.
- The E-cores were idle. The feed thread stayed on one P-core.
- Only the NEON kernels were measured. The AVX-512 and AVX2 paths do not build
  on this host.
- The live app adds the pty read, snapshots, and rendering. Those parts were not
  measured. This document covers the core feed path only.
