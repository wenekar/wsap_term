# UI migration: status and next phases

This page is the handoff for the UI library work. It lists what is done,
what is next, and what needs a decision. Write new work in the same style.
Keep sentences short. Use one instruction per sentence.

## Goal

The UI runs one frame in at most 1.85 ms. That is the budget for 540 Hz.
Every change must keep the frame inside that budget. Every change must also
keep the observable behavior of the app.

## Status

The work is on `main`. The last gate passed on head `19b5c2f`.

Gate results (median of three serialized runs, debugoptimized build, one core):

| Scenario | Median (us) | Limit (us) |
| --- | --- | --- |
| idle | 60.90 | 67 |
| hover | 60.75 | 67 |
| tab-open | 44.85 | 49 |
| menu-20 | 79.59 | 164 |

The full numbers and the method are in `benchmarks/ui-frame.md`.

### Done

Phase 0: measurement.

- `ui-frame-bench` times one frame of four scenarios.
- Limits come from the command line or the environment. None are hard-coded.
- `stats` reports a windowed render maximum, not a lifetime one.

Phase 1: style cost.

- The CSS matcher uses an index keyed by class. It replaces the full rule scan.
- Widgets read part styles through interned handles, with a per-widget cache.
- Migrated to handles: list, tab bar, label, input, scrollbar, scrollview,
  tab preview, and the terminal view.
- Removed: the stored style copies and their hooks, the tab bar's theme cache,
  and the standalone scrollbar widget.
- The scrollview owns the scrollbar. The terminal, the settings window, and
  the dropdown all use it.
- Overlay auto-hide, hover widening, and a hover tint are the defaults.

Input routing: the UI runs first.

- The window core tracks the pointer and held keys, then calls the UI client.
- A shortcut goes to the inspector overlay key, then to the app's command hook,
  then to the UI.
- Keys, text, and paste go to the UI first. The key surface is the top menu
  popup, or else the main surface.
- The terminal view is the default focus. With nothing focused, keys and text
  go to the grid.
- The terminal root takes paste, file drops, shortcuts, and window focus.
- Settings uses a key capture on its root. Its root is also the default focus.
- A handled key requests no frame by itself. A widget that changes requests a
  frame through invalidation.

## Next phases

### Phase 2: frame pipeline

The frame still walks the whole widget tree several times. Start here.

1. Gate each tree walk on the dirty flags that already exist. The walks are
   prepare, style apply, modal sync, scene build, and paint-flag clear.
2. Add a measure cache per subtree. Today the root dirty flag opens the gate,
   and every leaf is measured again.
3. Keep the layout node array between solves, per surface. `layout.c` frees
   and reallocates it on every solve.
4. Fix the O(n²) worst case in `find_record` in `damage.c`.
5. Give popup surfaces the same damage path as the main surface. Popups repaint
   the whole surface today.
6. Rebuild the accessibility tree only when the generation changes. Remove the
   per-node string copies.

Do not add a retained scene without a decision. The scene replay cache was
deleted on purpose. Read that history first.

Measure each step with `ui-frame-bench`. Keep the gate limits.

### Phase 3: render path without locks

Attribute the cost first. Run `perf record --call-graph dwarf` on the app.
Read the caller view of `pthread_mutex_lock`. The earlier profiles disagreed:
one showed about 3 percent, another about 27 percent. Do not rewrite until the
cause is known.

If the grapheme table is the cause:

1. Make the grapheme table append-only, with segments that never move.
   Readers take acquire loads and no lock.
2. Reset the table only at the frame barrier, while the workers are parked.
   Bump the generation on reset, so the glyph cache flushes.
3. Add a per-frame memo from terminal handle to global handle.
4. Check the handle before the lock in `grapheme_expand`. This fix is small.

Then replace the glyph retire list lock with a CAS push and an atomic exchange.
Then move the one-shot init locks to startup or `call_once`.

Lock-free is not automatically faster. Measure with `render-bench`. Add a
cluster-heavy flood case first.

### Real-session measurement

The headless gate has no compositor and no GPU. Measure once in a real Wayland
session before you claim 540 Hz. Use `wp_presentation` and `wp_fifo_v1` to pace
frames. Also check that damage uses one rectangle per dirty run.

## Decisions needed

1. **Identity strings.** Widgets still give class names as strings. Keep them
   for now. Measure the cache-miss path first. The likely cost is the ancestor
   path string that the part resolve builds. Interning that path may help more
   than interning the identity. Decide after the measurement.
2. **Out-of-memory in part interning.** `ui_part_intern` aborts on
   out-of-memory. The UI layer usually returns a failure instead. Keep the
   abort, or change the API to return `bool`.
3. **Terminal track cursor.** Over the scroll track, the cursor is now an
   arrow, not an I-beam. The owner accepted this.

## Known gaps

These are open test or design gaps. Each one has a short fix.

- If the CSS index build fails, every lookup returns an empty style. The
  callers ignore the error, so the failure is silent.
- Several new tests also pass on the old code. They guard behavior, but they do
  not prove the change. Add a regression test where a bug was fixed.
- A wrong focus flag on the label is not caught. A wrong pseudo flag on the tab
  preview card is not caught.
- A label reads its direct-paint state without `prepare`. The old copy was
  stale there. Only internal tests reach this path.
- The tab bar's reveal can lag its height after `begin_open` and `begin_close`.
  This was already true before the migration.
- A selectable label directly under the scroll track has no cursor bounds check.
  No live widget does this today.
- Clearing a label's selection on focus loss is in progress. It is not merged.
  Check whether the text input has the same gap.

## Rules for work in this area

- Work on a branch. Merge only after a verifier passes the change.
- Run the test subset for `ui-*`, `css-*`, `window-ui-*`, `view-test`,
  `settings-test`, `settings-window-test`, `terminal-input-test`, and
  `drop-test`.
- Run `ui-frame-bench` only when no other process is benchmarking.
- Use `scripts/wsap-debug lint` for every UI change. See
  `docs/ui-debugging.md`.
- Never stop the user's own terminal process.
- Commit trailers name the model that did the work.

## Housekeeping

- Several worker worktrees under `.claude/worktrees/` stay locked by the
  harness. Remove them after the lock clears. Their branches are merged.
- Many `wt/*` branches are merged and can be deleted.
- Scratch build directories live under the session scratchpad. They are not
  part of the repository.
