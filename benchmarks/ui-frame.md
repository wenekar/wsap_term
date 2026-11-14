# UI frame budget

This file holds the baseline for `ui-frame-bench`. The target is 540 Hz,
which gives 1.85 ms per frame.

## What the bench measures

Each timed frame runs these steps, in order:

1. `ui_surface_tick`
2. A full scene build, with `ui_scene_init` and `ui_surface_build_scene`
3. `ui_damage_track`
4. A CPU paint of the whole buffer

Input between frames runs untimed.

The bench does not measure the whole window frame. It skips the content
draw, the accessibility publish, the cursor update, the menu sync, the
timer re-arm, the debug hooks, and the band bookkeeping. The paint is an
upper bound for the paint step only. The app paints only the damaged
bands, and it can use the GPU path.

Scenarios:

- `idle`: a 12-tab bar with no input.
- `hover`: the same bar. The pointer moves to the next tab before each frame.
- `tab-open`: 50 settled tabs. The 51st tab opens and animates for 200 frames.
  No labels are drawn at that width.
- `menu-20`: a list of 20 rows, repainted each frame.

## Baseline

Build: `debugoptimized`, with no LTO. Pinned to one core with
`taskset -c 4`. Each value is the median of one run of 5000 frames, after
50 warm-up frames.

| Scenario | Baseline median (us) | Repeat run median (us) |
| --- | --- | --- |
| idle | 60.96 | 63.45 |
| hover | 60.73 | 63.52 |
| tab-open | 44.78 | 46.24 |
| menu-20 | 148.83 | 148.94 |

The two runs differ by 2% to 4% on the tab bar. The menu scenario showed
one slow run of 162.95 us. The spread is noise, so a gate needs margin.

The repo default build (`release`, LTO) is about 11% faster on idle and
hover than this baseline. The baseline uses `debugoptimized` by decision.

### After the CSS index (Phase 1, commit aaddde3)

The indexed matcher cut the menu-20 median from about 148 us to about
86 us. The other scenarios did not change. The baseline table above is
the pre-index record. These are the current medians, with the same build
and method:

| Scenario | Median (us) |
| --- | --- |
| idle | about 60.5 to 60.9 |
| hover | about 60.6 to 60.8 |
| tab-open | about 44.6 to 44.8 |
| menu-20 | about 86.1 to 86.8 |

The part-handle cache (commit 974473e) did not move these numbers. No
production code calls it yet.

### After the damage key index

`ui_damage_track` now finds a record by key through an open-addressed
table. A cheap positional hint stays first. The four scenario medians did
not move, because the hint hits in these scenes. The table removes the
quadratic cost when a scene shifts the order of its scopes.

A harness of 1024 scopes that shifts the scope order by one each frame
measures the lookup alone. The records have empty ink, so no pixel work
counts. Each value is the median of three runs:

| Scopes | Linear scan (us) | Key index (us) |
| --- | --- | --- |
| 64 | 1.8 | 1.1 |
| 256 | 14.6 | 4.4 |
| 512 | 48.0 | 8.9 |
| 1024 | 168 | 18.5 |

An unchanged scene stays flat in both cases, at about 14 us for 1024
scopes.

## Running the gate

```sh
build/benchmarks/ui-frame-bench \
    --limit idle=67 --limit hover=67 --limit tab-open=49 --limit menu-20=164
```

The environment works too. Arguments win over the environment:

```sh
UI_FRAME_BENCH_LIMIT_IDLE_US=67 UI_FRAME_BENCH_LIMIT_HOVER_US=67 \
UI_FRAME_BENCH_LIMIT_TAB_OPEN_US=49 UI_FRAME_BENCH_LIMIT_MENU_20_US=164 \
    build/benchmarks/ui-frame-bench
```

Exit codes: 0 when every limited scenario passes or none has a limit, 1
when a median is above its limit, and 2 for bad input or a failed frame.

## Limits

The first limits sit about 10% above the baseline. They are a starting
point. Phase 1 onward lowers them as the code gets faster. They are not
the 540 Hz target. Only a real session shows whether the full frame fits
1.85 ms.

| Scenario | Limit (us) |
| --- | --- |
| idle | 67 |
| hover | 67 |
| tab-open | 49 |
| menu-20 | 164 |

## Not measured yet

- Palette typing, the settings tree, and input dispatch
- Tab drag and tab close
- The content draw, accessibility publish, and cursor update
- The damage-limited paint, and the GPU path
- Presentation on a real Wayland session

Cursor blink causes about two idle frames per second. The bench does not
count those frames.

## Phase 1 gate (commit eafb4d3)

Three serialized runs, one core, load below 2.0 at each run. Each value is
the median of the three per-run medians:

| Scenario | Median (us) | Limit (us) | Margin (us) |
| --- | --- | --- | --- |
| idle | 61.03 | 67 | 5.97 |
| hover | 60.77 | 67 | 6.23 |
| tab-open | 45.02 | 49 | 3.98 |
| menu-20 | 79.48 | 164 | 84.52 |

The menu-20 median is about 7 us lower than the pre-index baseline. The
other scenarios match the earlier numbers. All four limits pass in all
three runs.

### After the sorted rule table

A sheet now sorts its rule table once, when parsing ends. The sort order is
tier, then class rank, then source order. A style lookup walks the table and
applies each match. The key groups, the candidate buffer, and the merge walk
are gone. `tests/ui/css/lookup_test.c` covers the order.

Per-lookup cost went up, because a lookup scans every rule. The style
context caches a resolved style per widget generation, so a frame does not
pay it twice. The four scenario medians did not move outside noise, and the
`render-bench` sum over all cases improved by 0.7%:

| Case | Before (ms) | After (ms) |
| --- | --- | --- |
| flood-bytes | 0.2960 | 0.2880 |
| flood-printable | 0.2800 | 0.2640 |
| flood-color | 0.4930 | 0.4880 |
| flood-truecolor | 0.3300 | 0.3310 |
| random | 0.4570 | 0.4630 |
| sum of all cases | 2.1739 | 2.1583 |

Every `pixels=` hash was identical before and after. A differential dump of
`struct css_style` over 60 generated sheets and the default sheet, 11 200
queries in all, matched byte for byte.
