# Render rewrite

This file records `render-bench` before and after the render rewrite.

## Method

- The build is the project default: release, LTO, and `isa=native`.
  A second build uses `-Disa=x86-64`.
- The CPU is an AMD Ryzen 7 9700X. The compiler is GCC 16.2.1.
- Each run is `FLOOD_FRAMES=1000 build-rel/tests/render-test --bench`.
- The baseline has seven runs at `isa=native` and one run at
  `-Disa=x86-64`. The runs did not overlap.
- No other build or benchmark ran during a sample.
- The samples did not use CPU affinity, because the renderer uses a
  thread pool.
- Each value is the mean frame time in milliseconds.
  The native columns give the median, minimum, and maximum of the
  seven runs.

The flood cases and the new cases draw to one persistent surface.
`BENCH_CASE=<name>` runs one case.

## Cases

| Case | Workload |
|---|---|
| flood-bytes, flood-printable, flood-code, flood-log | About 8 KiB of input per frame, from `tests/helpers/flood.h` |
| flood-color | Printable text with a 256-color SGR every few characters |
| flood-truecolor | A 24-bit foreground SGR before every character |
| typing | One character per frame on a full screen of code |
| idle | No input; one draw per frame |
| scroll-region | One new line per frame in a DECSTBM region |
| overlay-scroll | One new line per frame under a scrollbar-shaped UI overlay. The overlay changes only in the first frame. |
| italic | About 4 KiB of italic code text per frame |
| cjk-emoji | About 4 KiB of CJK, emoji, and ASCII per frame |
| fresh-every-frame | One character per frame, with the target marked stale. The window marks every frame stale today. The case draws no UI scene. |
| plain, ligatures, mixed, random, scroll | The old cases |

## Baseline

The baseline renderer is the renderer of commit `b5798f2`. The bench cases come from the first commit of the rewrite series.

| Case | Native median | Native min | Native max | x86-64 |
|---|---:|---:|---:|---:|
| flood-bytes | 0.357 | 0.349 | 0.363 | 0.295 |
| flood-printable | 0.335 | 0.328 | 0.337 | 0.340 |
| flood-code | 0.031 | 0.030 | 0.032 | 0.039 |
| flood-log | 0.033 | 0.032 | 0.033 | 0.044 |
| flood-color | 1.246 | 1.226 | 1.288 | 1.107 |
| flood-truecolor | 0.426 | 0.417 | 0.479 | 0.249 |
| typing | 0.0025 | 0.0022 | 0.0033 | 0.0037 |
| idle | 0.0003 | 0.0002 | 0.0003 | 0.0003 |
| scroll-region | 0.0291 | 0.0286 | 0.0298 | 0.0352 |
| overlay-scroll | 0.0648 | 0.0638 | 0.0669 | 0.0672 |
| italic | 0.0325 | 0.0312 | 0.0331 | 0.0394 |
| cjk-emoji | 0.1101 | 0.1091 | 0.1107 | 0.1035 |
| fresh-every-frame | 0.0305 | 0.0297 | 0.0315 | 0.0455 |
| plain | 0.024 | 0.016 | 0.024 | 0.046 |
| ligatures | 0.020 | 0.019 | 0.033 | 0.052 |
| mixed | 0.053 | 0.052 | 0.077 | 0.076 |
| random | 0.736 | 0.719 | 0.764 | 1.166 |
| scroll | 0.033 | 0.032 | 0.037 | 0.033 |

Observations:

- The two builds give different pixels. At `isa=native`, the hashes are
  plain `66e883dd4967e08e`, ligatures `07863d50ddaf2d45`, mixed
  `6510b5dde2559808`, random `f74a499a7822dbb1`, and scroll
  `5294a81127feeb6f`. At `-Disa=x86-64`, all five hashes are different.
  The old renderer picks its SIMD path at compile time, and that path
  changes what a cell shows.
- flood-bytes, flood-color, and flood-truecolor are faster in the
  x86-64 build than in the native build.
- The scroll case is not faster than a full repaint.
- overlay-scroll was measured again after its fix, in a later session
  with the same method. The first version of the case repainted every
  frame in full.

## After

The final milestone adds the numbers for the new renderer.
