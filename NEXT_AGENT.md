# Next agent

Read this file before you start. It lists the open work from the session of
2026-10-28. Do the tasks in order. Remove each task when it is done. Delete
this file, and its line in `AGENTS.md`, when no task is left.

## State

The terminal core (`subprojects/term`) had a hardening and performance pass.
Two commits on `main` hold it:

- `cc73e31` Harden the terminal core and fix faults found by fuzzing
- `c877fbe` Stream CRLF lines

Read these first: `subprojects/term/docs/testing.md` (test layers),
`subprojects/term/docs/correctness-audit.md` (faults found and fixed), and
`benchmarks/term-line-streams.md` (the performance pass and its method).

## 1. Fix the margin-mode chunking fault

With left and right margins (DECLRMM, `CSI ? 69 h`), insert mode, and wide
or joined characters together, two splits of the same output can give two
different screens. That breaks the rule that a cell must not depend on how
the output arrives.

Reproduce it. Add this case to `gen()` in `subprojects/term/tests/fuzz_gen.h`,
and change `rn(41)` to `rn(42)`:

```c
        case 41:
            if (!gen_unmodeled)
                break;
            if (rn(3)) {
                int left = 1 + rn(cols), right = left + rn(cols + 1 - left);
                putf("\033[?69h\033[%d;%ds", left, right);
            } else {
                put("\033[?69l");
            }
            break;
```

Then `write-fuzz-test 50000` fails at seed 11 (18 by 3 screen, step 0). In
that input, bytes 751 and 752 (`1I`) fed in one call, with all other bytes
fed one at a time, give a different screen from all bytes fed one at a time.
The screens first differ after byte 1507. Byte 1507 ends a CJK character in
insert mode, after a ZWJ cluster. At that point the cursor row of the
byte-fed terminal still holds old cells (`RYd;+`) after its length. A step
then makes the row full width without blanking those cells, and they appear
again. The fix in `wsap_vtcore_sc_print_glyph` (the `srow->len <= right`
check) is the same kind of fault.

Find each place that makes a row longer. Make sure that the cells from the
old length to the new length are blank, unless the row has `RF_TAIL_PADDED`.
`wsap_vtcore_row_fill_gap` does this for one case. Also check the cluster
code in `text.c` (`emit_decoded`) with insert mode and margins.

To find a fault fast, write a small tool. Feed the input one byte at a time,
but feed one window of 2 to 64 bytes in one call. Report the smallest window
that changes the final screen or the history text. Then compare the two
terminals after each later byte to find the first byte that differs. Inspect
`t->scr.slot[]` (`kind`, `len`, `flags`, `cells`) with gdb on a debug build.
Use a gdb command file for loops; `-ex` cannot hold a `while`.

Keep the generator case. Done when `write-fuzz-test 50000` passes at each
level (`WSAP_VTCORE_ISA=base`, `avx2`, and the default), and `model-test`
still passes. Add a short `term-test` case for each fault you fix.

Separate question: `wsap_vtcore_screen_insert_chars` (ICH) ignores the left
and right margins. xterm limits ICH to the margins. Check the other edit
operations (DCH, ECH, IL, DL) too.

## 2. Review the old subagent worktrees

The earlier session left four worktrees in
`../worktrees/wsap_term/pi-worktree-*` (branches `pi-subagents/*`) with
changes that are not committed. Most of the changes are now in `cc73e31`.
One is not: `parser-safety` changes the digit saturation in `src/csi.h` and
`src/escape.c` so that values from 2147483640 to INT_MAX stay exact. This is
optional. If you take it, keep the fast CSI path and the base parser equal,
and run `write-fuzz-test`. Then ask the user before you remove the worktrees.

## 3. Run NEON code in CI

The NEON paths have a compile check only (`term-cross-test`). Add an
`ubuntu-24.04-arm` entry to the `core` matrix in `term.yml` so that the unit
suite runs on AArch64.

## 4. Performance follow-ups

Measure each change as `benchmarks/term-line-streams.md` describes.

- `benchmarks/history-descriptors.md` names a packed 8-byte queue entry for
  `bulk_ascii_lines_body`, worth about 3%.

## 5. Coverage gaps

The unit suite covers 91% of lines and 82% of branches (Clang source-based
coverage; `docs/testing.md` has the commands). The weakest files are
`term.c`, `selection.c`, `gstore.c`, and the OSC handlers. Write behavior
tests through the public API for the gaps that matter. The allocation sweep
does not reach `hist.c` exact-row cache regrowth or the region scroll in
`screen.c` that wraps the ring with more than 256 rows.
`wsap_vtcore_hist_pop_tail_cells` has no caller outside `hist_test.c`.
Remove it only together with `wsap_term-ng`.

## Method notes

- Build the core alone: `meson setup build subprojects/term -Dkitty=disabled`.
  Run `nice -n 10` for builds and tests. The user games on this machine.
- Benchmark only on a quiet machine. Check `top` first. Other agents can run
  benchmarks in sibling repositories, for example `run_split_bench` from
  `../wsap_term-ng`. Alternate the two builds case by case on one CPU
  (`taskset -c 4`). A difference below 3% is noise: two builds of the same
  source differ by that much.
- The field layout of `struct wsap_vtcore` is part of the hot path. Put a
  new flag in the existing bit group, and measure `utf8` and `cjk`.
- `perf` works with `cycles:u` only. Call graphs do not work. Use gdb on a
  debug build (`--buildtype=debug -Db_lto=false`) to find callers.
- `meson compile -C DIR TARGET` does not reconfigure. After a change to a
  `meson.build`, run `meson compile -C DIR` first. Meson prints `ERROR` in
  capitals, so a `grep error` misses it.
- Do not run `pkill -f` with a pattern that matches your own command line.
