# Cursor Loop: Dedicated CSI Execution Loop

Ryzen 7 9700X, GCC, `-O3 -march=native -flto`, Linux.

## Change

`try_fast_csi` in `subprojects/term/src/text.c` is now a dedicated execution loop.
It keeps this state in locals across consecutive CSI commands and the
text spans between them:

- `cursor_x` and `cursor_y`
- the cursor row ring slot `out->p` and its cell pointer `out->row`
- `cur_fg` and `cur_bg`
- `pending_wrap`

Locals commit to `struct term` through `FAST_SYNC()` at defined points:
before any delegated call that observes screen state (erase, delete,
insert, restore, `line_feed`), and when the loop exits. A command that
is not recognized backs up to the last `ESC[` and returns, so the
generic escape parser sees the same bytes as before.

Cursor moves no longer call `term_screen_move_cursor`. They clamp in
registers, compute the new ring slot once, and call `row_cells()` once
per row change. The row pointer stays live in a local.

New coverage in the fast path:

- `DCH` (`P`), `ICH` (`@`), `ECH` (`X`), delegated after `FAST_SYNC()`.
- `DECSC`/`DECRC` (`s`, `u`).
- General `SGR` (`...m`): up to 12 params, standard and extended colors,
  attribute set/clear. Param parsing reuses the small digit loop.
- `CR` and `LF` between commands. `LF` calls `line_feed()`, which owns
  the scroll path. `pending_wrap` semantics are unchanged.

## Damage model constraint

`subprojects/term/tests/term_test.c` compares `row_gen` between a whole write and
every two-part split of the same stream. A split can push a command back
to the generic parser. So each command must produce the same `row_gen`
delta in both paths. Per-write-call dedup of row damage is not safe.
The loop keeps the exact per-command deltas: a move bumps the old and
new row even when they coincide, and a text span adds its length.

## Results

`taskset -c 6 ./build/subprojects/term/benchmarks/term-write-bench <case> 42 0 512 5`,
median MiB/s, larger chunk:

| case | before | after |
| --- | ---: | ---: |
| cursor | 672 | 774 |
| editor | 6049 | 5940 |
| dch | 466 | 934 |
| colors | 686 | 1074 |
| erase | 418 | 445 |
| ascii | ~7400 | ~7400 |

`dch` gains most because `4P` now stays in the loop instead of bouncing
to the generic parser. `colors` gains because `31m`/`0m` and friends no
longer leave the loop. `editor` is within noise of baseline.

## Not done

- No cross-command damage dedup (see the constraint above).
- `IL`/`DL`/`SU`/`SD`, `REP`, tab moves, mode sets, and queries still
  bail to the generic parser.
- `origin_mode` and `lr_margin_mode` still bail on entry.
