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

## 1. Finish three unmerged feature worktrees

Three agents stopped at a usage limit with work that is not committed. Each
worktree is based on `1cb21cd`; `main` is now ahead of it. Check each one,
finish it, run `meson test`, `write-fuzz-test`, and `model-test`, then rebase
or cherry-pick it onto `main`:

- `../wsap_term-wt/linesize`: double-width and double-height
  lines (DECDWL, DECDHL, DECSWL). Uses `srow.flags` bits 0x20 and 0x40.
- `../wsap_term-wt/osc`: OSC 133 prompt marks with a
  host API, OSC 9;4 progress, OSC 9;9, and OSC 1337 CurrentDir/SetUserVar.
  Uses `srow.flags` bits 0x02 and 0x80.
- `../wsap_term-wt/charsets`: G0 to G3, LS2/LS3, SS2/SS3,
  DEC Supplemental and Technical sets, and the NRCS sets.

## 2. App follow-ups for the new core features

- Call `wsap_vtcore_set_cell_pixels` and `wsap_vtcore_set_color_scheme`.
  Until then, `CSI 14 t` and `CSI 16 t` report zero pixels.
- Draw blink, conceal, and underline color
  (`wsap_vtcore_snapshot_row_underline_colors`).
- `src/terminal/view.c` sends arrow keys for the wheel on the alternate
  screen without checking mode 1007, and its 64-byte buffer is too small
  for more than 21 lines.

## 3. Run NEON code in CI

The `macos-14` entry of the `core` matrix in `term.yml` runs on arm64, so
the unit suite runs the NEON paths on macOS. Linux AArch64 has a compile
check only (`term-cross-test`). Add an `ubuntu-24.04-arm` entry to the
`core` matrix so that the unit suite also runs there.

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
