# Mixed UTF-8 SIMD Decoder

Recorded: during the `perf/simd-utf8` work
Hardware: AMD Ryzen 7 9700X (Zen 5), AVX-512 F/BW/VL/VBMI/VBMI2
Compiler: GCC, `-O3 -march=native -flto`

## Summary

An AVX-512 decoder for mixed UTF-8 text was implemented in
`term_text_run()` and then removed. The decoder was correct, but it did
not beat the existing scalar loop on the target CPU. The scalar loop is
already strong, and the vector path could not recover its fixed costs.
This document records the investigation and the result.

## Goal

`term_text_run()` decodes most terminal text. Its AVX-512 path handled
runs of three-byte characters. Mixed text with ASCII and 2-, 3-, and
4-byte characters used the scalar loop. The goal was to decode such
mixed blocks with SIMD: batch validation, code point decoding, width
lookup, and cell writes.

## Design that was tested

The decoder scanned a 64-byte block once. It built masks for
continuation bytes, printable ASCII, and 2-, 3-, and 4-byte leads. From
the masks it derived the expected continuation structure, character
start positions, and a block limit at the first malformed or
unsupported byte. It then decoded groups of up to 16 characters with
AVX-512 byte permutes into temporary code point and position buffers.
Cells were emitted from the buffers. Narrow groups used vectorized
quad stores. Wide groups used vectorized head-trail pairs. Other
characters used scalar stores.

BMP widths came from `unicode_fast_class` through scalar loads. Astral
widths and cluster checks stayed scalar. The decoder stopped before
controls, malformed UTF-8, unsupported cluster characters, width
overflow, and incomplete sequences. It returned the exact byte and
column position for the scalar loop to resume.

A probe at the call site rejected unsuitable input before the decoder
call. A retry limit stopped repeated probes on lines the vector path
could not take.

## Correctness

A randomized differential harness compared `term_text_run()` with a
scalar reference over ASCII, valid 2-, 3-, and 4-byte characters,
invalid leads, stray continuations, overlong encodings, surrogates,
out-of-range code points, controls, and incomplete sequences. The
final implementation reported zero mismatches. Two real bugs were
found and fixed this way: a consumed-byte count that swallowed a stray
continuation byte, and an uninitialized output-column result on early
return.

## Performance results

Measurements used `term-write-bench` (median of several runs, 4096-byte
chunks) and a direct `term_text_run` microbenchmark.

| Workload | Scalar loop | SIMD decoder |
| --- | ---: | ---: |
| utf8 | ~1100 MiB/s | ~910-950 MiB/s |
| utf8var | ~445 MiB/s | ~420-440 MiB/s |
| cjk | ~2240-2300 MiB/s | ~2030-2170 MiB/s |
| ascii | ~7300 MiB/s | ~7300 MiB/s |
| random | ~205 MiB/s | ~204 MiB/s |
| Dense long mixed line | ~187 MiB/s | ~182 MiB/s |

The decoder matched scalar on the dense long line but lost 5-15% on
the standard cases. Probe and call overhead dominated short terminal
lines. Gating by run length and character density removed most of the
loss but left a residual cost and removed the benefit as well.

## Why it lost

- AVX-512 gathers are expensive on Zen 5. A gather-based BMP width
  lookup dominated the decoder. Removing it helped, but the remaining
  scalar table loads still cost per character.
- Per-call costs did not amortize. Probe, call, block scan, and
  position-buffer setup cost roughly what the scalar loop needs to
  decode a short line.
- The scalar loop is already fast: about 4-5 cycles per byte on mixed
  text on this CPU. The SIMD decoder reached the same per-character
  cost without the fixed overhead.
- Short terminal lines made the problem worse. Most lines end at a
  control byte after tens of characters, so the decoder rarely saw a
  long contiguous run.

## Rejected designs

- Gather-based width lookup: gather latency dominated on Zen 5.
- Fully vectorized emission with per-width branches: slower than the
  scalar emit loop.
- Always-on probing at every non-ASCII byte: probe cost exceeded the
  savings.

## Outcome

The decoder was removed. `term_text_run()` keeps the scalar loop and
the existing three-byte AVX-512 path. No regressions remain.

## Methodology

```sh
meson test -C build --print-errorlogs
taskset -c 4 ./build/subprojects/term/benchmarks/term-write-bench <case> 42 0 512 15
```

A/B builds toggled the decoder gate. `perf record`/`perf annotate`
identified gather and emit costs.
