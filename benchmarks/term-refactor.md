# Terminal Modularization Benchmark

Recorded: 2026-09-10 21:10:58 +03:00 (Europe/Istanbul).

This report measures the performance impact of modularizing the terminal subsystem
into decoupled translation units with independent compilation boundaries.

## Measurement

GCC 16.2.1, `-O3 -march=native -flto`. Both baseline and refactored versions were
compiled with identical optimization flags.

Median MiB/s from three alternating runs after tests finished:

| Workload | Baseline | Refactor | Change |
| --- | ---: | ---: | ---: |
| ASCII | 2626.15 | 2530.84 | -3.6% |
| UTF-8 | 1225.00 | 1115.01 | -9.0% |
| CJK | 2590.12 | 2393.28 | -7.6% |
| Colors | 592.61 | 607.35 | +2.5% |
| Cursor | 540.17 | 582.12 | +7.8% |
| Erase | 187.37 | 189.16 | +1.0% |
| Random | 140.88 | 137.51 | -2.4% |

## Validation

- Full application test suite across terminal, font, clipboard, paste, UI, keyboard,
  PTY fallback, and renderer components.
- Boundary and round-trip tests covering every valid Unicode scalar and invalid sequences.
- Address and undefined-behavior sanitizers enabled for verification builds.
