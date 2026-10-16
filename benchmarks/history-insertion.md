# Short-line history insertion

## Changes

- Scan ASCII and LF input in 64-byte blocks.
- Keep an unfinished line in the input buffer.
- Resume the block scan after a long line uses the normal path.
- Copy contiguous short lines to a history page in one batch.
- Keep separators in the page. Descriptors exclude them.
- Check 16 same-page rows with SIMD before eviction.
- Use AVX-512, AVX2, or NEON. Keep a scalar fallback.

The local direct-stream experiment failed the terminal, model, and write-fuzz tests.
It changed history before it rejected input.
Its screen split also counted newlines rather than wrapped screen rows.
This change removes that path and its unused insertion API.
The other local page metadata changes remain.

## Measurements

CPU: AMD Ryzen 7 9700X.
Build: release, `-O3 -march=native`, LTO enabled.
History limit: 4096 lines. Screen: 120 by 40 cells.

The reference disables the invalid direct-stream path.
It also disables the new block scan, contiguous copy, and SIMD eviction check.
Both binaries retain the other local changes.
Do not compare the invalid direct-stream result with these numbers.

Seven alternating runs with `tools/ab.sh` gave these results:

| Case | Chunk | Reference MiB/s | New MiB/s | Change |
| --- | ---: | ---: | ---: | ---: |
| manyline | 65536 | 4444.39 | 6943.83 | +56.24% |

Separate runs used seed 42, 4096 iterations, and seven samples:

| Case | Chunk | Reference MiB/s | New MiB/s | Change |
| --- | ---: | ---: | ---: | ---: |
| manyline | 4096 | 2957.98 | 3986.07 | +34.76% |

Five alternating runs showed longline at -0.25% and ascii at +4.23%.
The final benchmark suite measured manyline at 6922.96 MiB/s with 65536-byte chunks.
It measured 3991.59 MiB/s with 4096-byte chunks.
These rates measure the complete feed path, not insertion alone.

## Checks

- All 84 tests passed.
- The term-write-bench and render-bench benchmark runs passed.
- A new history test checks text and blank lines after repeated eviction and page reuse.
- AVX2 and scalar builds of core.c and hist.c compiled.
- The NEON path was not built or run on this host.
- The term-live-bench target compiled. No live session was measured.
