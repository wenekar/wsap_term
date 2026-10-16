# History descriptors, bulk eviction, and short line copies

Ryzen 7 9700X, GCC, `-O3 -march=native -flto`, Linux.

## Goal

Raise ManyLine throughput. The old high water mark was 6.17 GiB/s live,
from `benchmarks/transport-bench.md`. The headless case sat near 3.7 GiB/s.
The old design was less safe, so this pass keeps the current design and
makes it cheaper instead.

## Finding

The case is instruction bound, not memory bound. A run of the headless
ManyLine case reports:

| Counter | Value |
| --- | ---: |
| IPC | 4.9 |
| Branch misses | 0.02% of branches |
| Last level cache misses | 9.2 K |

An IPC of 4.9 out of 6 leaves little room. Only fewer instructions help.

Instruction counts per symbol, from `perf record -e instructions:u`,
show where the work sat:

| Symbol | Share |
| --- | ---: |
| `hist_push_ascii_lines` | 51% |
| `sc_bulk_ascii_lines` | 26% |
| `hist_push_blank` | 13% |
| `bytes_copy_small` | 11% |
| `page_drop_ref` | 6% |

ManyLine writes about 26 byte lines. Each line costs about 157
instructions.

Two plausible causes were checked and ruled out:

- Eviction. A run with no eviction at all was slower, at 3336 MiB/s. The
  larger ring lost cache locality. Eviction is not the limit.
- Cache misses on the descriptor store. `perf` put 75% of the samples in
  `hist_push_blank` on one wide store, which looks like a miss. A
  scrollback sweep of 4096, 2048, and 1024 measured the same. It was
  sample skid.

## Change

### Descriptor stores

A narrow line that fits one row has the same `wide`, `attr_len`, `flags`,
and `wrap_rows` in every descriptor. Only `off`, `text_len`, and `cells`
change. The write loop builds the 16 bytes in two 64-bit halves and
stores them with one pair of stores. The compound literal form costs a
16-byte zero store plus four field patches. The loop also keeps the ring
pointer, the ring mask, and `head` in registers.

`hist_push_blank` gets the same treatment. Every blank part is the same
16 bytes, so one constant descriptor serves all of them. Its loop now
takes ring room for the whole run once, before it starts. Growing the
ring later would move the buffer that the loop holds in registers.

### Blank lines join the batch

A blank line ended the batch. The scan stopped, the batch closed, and the
next line started a new batch with the whole setup again. A run of blank
lines cost one full setup each.

The average batch was 18.6 lines. A blank line now joins the batch: it
takes no page slot, so the batch tracks text lines separately for
`page_parts`, and it leaves `all_blank` alone. It writes the same constant
descriptor and adds its one row.

The average batch is now 70.8 lines, and all 28831 blanks of a 256
iteration run go through the batch instead of `hist_push_blank`.

`hist_push_blank` drops out of the profile for this case. It is still the
entry path for a blank that arrives while a line is open, so it stays.

### Eviction

A batch pushes about 70 lines and evicts about 70. Each eviction read a
descriptor, took one page reference, and stepped `tail`. The lines of one
batch usually sit in one page, so all those references can go at once.

`evict_simple_run()` walks the tail, and drops the page reference once per
page rather than once per line. It stops at the first part that is open,
that continues the line before it, or that holds an inexact row count,
and `evict_for()` hands those back to `evict_line()`.

The helper guards the count before it subtracts:

```c
uint64_t have = h->lines;
if (have + n <= (uint64_t)h->line_limit)
    return;
uint64_t need = have + n - h->line_limit;
```

An earlier version wrote `lines + n - line_limit` at the call site
directly. That wraps to about 4 billion when there is nothing to evict and
spins forever. The guard belongs inside the helper, not at the call site.

Eviction is bandwidth bound, not instruction bound. Hoisting the counted
line limit out of the loop was worth 0.3%, which is noise. The ring is
64 KiB and every line is written once at the head and read once at the
tail.

### Inline and split calls

`bytes_copy_small` gets the 16 to 32 byte case first, as two overlapping
16-byte copies. That is the usual history line length. Both loads stay
inside the source range. The function is now `always_inline`; the old
version was a real call with stack spills on the write path.

The split mattered. Without `always_inline` the compiler left it out of
line again and the UTF-8 gains vanished. That is worth knowing: the
`always_inline` is load bearing, not decoration.

`page_drop_ref` is `always_inline`. It is a six instruction body that ran
once per evicted line.

`ring_fit` is split. The ring space check is inline. Only the rehash is a
call.

## Headless results

`tools/ab.sh` runs the cases interleaved and reports medians of 7 rounds,
`taskset -c 6`:

| Case | Before | After | Change |
| --- | ---: | ---: | ---: |
| utf8 | 666.7 | 901.8 | +35.3% |
| manyline | 3734.0 | 4363.3 | +16.9% |
| cjk | 1470.5 | 1652.4 | +12.4% |
| ascii | 6707.6 | 7185.6 | +7.1% |
| colors | 718.6 | 736.5 | +2.5% |
| erase | 438.9 | 442.3 | +0.8% |
| longline | 34873 | 34529 | -1.0% |
| lf-burst | 164216 | 161577 | -1.6% |
| dch | 614.8 | 601.7 | -2.1% |

ManyLine instruction count fell from 6.249 to 5.816 per byte. It is now
4.5 times the cost per byte of LongLine at 0.510.

The UTF-8 and CJK gains come from the copy change, not the history change.
The short losses come from the same change. Cases that copy outside 16 to
32 bytes now test one extra branch. The trade is worth it: `utf8` and
`cjk` are real workloads and `dch` is a narrow case.

## Why the history is not made asynchronous

Two threads take the writer side of the sequence lock. The pty ring
worker calls `term_feed()`, and the render thread calls
`term_snapshot_update()`. The read side is the interesting half: a
viewport snapshot walks the history and copies rows.

The snapshot cannot be a plain read. `term_snapshot_update_inner()` calls
`hist_prepare_rows()`, `hist_view_sync()`, and `hist_view_refine()`, and
those write into `hist` and into the view anchor. They maintain the
Fenwick index of line row counts and the exact row counts. That is the
whole reason the snapshot takes the writer side, and it is deliberate:
the write path does not maintain that index, so parsing stays cheap.

Measured cost of one snapshot, 120 by 40, scrollback 4096:

| Case | Mean | Max |
| --- | ---: | ---: |
| viewport moved by a feed | 7.05 us | 13.65 us |
| viewport unchanged | 0.13 us | 0.51 us |

One `term_feed()` of 4 KiB costs 1.33 us. At 60 to 120 frames per second
the render thread holds the writer 60 to 120 times, so it occupies it for
0.04% to 0.08% of the time. The reader loses about that much.

A live profile agrees. `term_snapshot_update_inner` is 0.26% of the
instructions in a live ManyLine run, and no render or paint symbol reaches
the top of the profile at all.

So an RCU style history would buy well under 1%. It would also move the
page reference counting, the Fenwick index, and the exact row counts onto
a new reclamation scheme, which is the largest correctness surface in the
tree. The trade is bad.

One cost is real, but it is latency, not throughput. The reader holds the
writer for a whole `term_feed()` call, and one 64 KiB read is about 21 us.
A scalar getter on the render thread spins for at most that long. That
bounds frame jitter at about 20 us. A double buffered copy of the
scalars, published at frame rate, would remove it without touching the
history at all.

## Where the live ManyLine number goes

Live ManyLine is not a parse limit. Two ceilings sit above it.

termbench writes a pre-generated buffer, so the producer tops out at
2.28 gb/s on this machine:

```sh
env -u LD_PRELOAD taskset -c 5 build/benchmarks/termbench normal ManyLine > /dev/null
```

Live ManyLine through wsap_term is about 1.38 gb/s, which is 60% of that.
The 6.17 GiB/s in `transport-bench.md` is a different program, not
termbench.

The gap is the pipeline, not the parse path. The terminal runs the same
instructions per byte live as headless, but at about half the rate:

| | instructions per byte | IPC |
| --- | ---: | ---: |
| headless | 6.25 | 4.86 |
| live | 6.3 | about 2.6 |

The main thread is 61.4% busy over a 7.0 s window and handles about
2.21 GB/s per CPU second, against 4.25 headless. Frequency is not the
cause: 5409 MHz idle against 5323 MHz under load.

Ruled out by experiment. The pre-sequence-lock parent commit measures the
same. Sway and headless weston measure the same. Raising the wakeup
threshold in `PTY_SIGNAL_BYTES` from 128 KiB to 4 MiB made it slightly
worse. More cores buys 6%. Nothing in `subprojects/term/src/` signals the event loop,
and rendering is not per newline: one run carried 222 million newlines
against 282 frames.

Newline density is the one thing that matters. Same 6.44 GB, same build:

| | ManyLine | LongLine |
| --- | ---: | ---: |
| live throughput | 1.34 gb/s | 4.87 gb/s |
| drain events | 2205 | 5483 |
| mean work per event | 1765 us | 32.7 us |
| CPU in the drain path | 3.893 s | 0.179 s |
| cost per byte | 0.604 ns | 0.028 ns |

ManyLine spends 21.7 times more CPU per byte. A newline leaves the bulk
path in `sc_bulk_ascii_lines`. `mask == 0` sends a whole run of wrapped
lines into history in one `hist_push_ascii()` call, which emits about two
descriptors per 64 KiB. `mask != 0` takes the per-line path: each newline
is staged into `q[]`, the queue is rescanned, then re-read to copy and to
write a descriptor. A newline every 27 bytes means the per-line path
forever.

The queue cannot simply go away. It is an ordering buffer, and it holds
the last `rows - 1` lines on the screen while older lines go to history.
But its `orig_rem` counter reaches zero after the first screen fill and
stays there, so in sustained output the ordering job is empty and only the
staging remains.

## Measurement

A single run of this benchmark moves by 3% on this host. Do not compare
two runs taken at different times. Keep two binaries and run them
alternately, then compare the medians.

```sh
tools/ab.sh manyline 7 build/ab/base build/ab/full
```

Instruction counts are exact and need no repetitions:

```sh
perf stat -e cycles,instructions,branch-misses,cache-misses \
    build/ab/full manyline 42 65536 2048 3
```

## Validation

```sh
make test
env -u LD_PRELOAD meson test -C build-asan --setup=asan -t 8
./tools/analyze.sh
```

`make test` gives 88 of 89. The `kitty-test` failure is a cursor position
report mismatch and it fails the same way on an unmodified tree.

The Address Sanitizer build needs `LD_PRELOAD` cleared. The preload shim
loads ahead of the sanitizer runtime, so every binary in `build-asan`
aborts at startup with "ASan runtime does not come first in initial
library list". `tests/meson.build` clears it for the test list, but the
`asan` setup does not, so the variable has to go on the command line:

```sh
env -u LD_PRELOAD meson test -C build-asan --setup=asan -t 8
```

With that, the build gives 84 of 84 under AddressSanitizer and
UndefinedBehaviorSanitizer. `model-test` and `write-fuzz-test` are the
gate for the eviction change. They compare the terminal against a
reference model, and they pass.

`./tools/analyze.sh` is clean for `src/`. It still reports findings in
`tests/`, which are pre-existing.

## Left undone

The intermediate queue in `sc_bulk_ascii_lines` costs about 16 bytes of
stack store and load per line. Its entry is 16 bytes wide and holds only a
pointer, a 16-bit length, and a wrapped flag. A packed 8-byte entry with
the length and the flag in one word would remove one load from both the
scan loop and the write loop. That is worth about 3%.

Removing the queue entirely is worth more, near 10%. It would mean writing
history pages straight from the parse loop. The screen core and the
history store would have to share the page reservation. That is a large
change to the most delicate code in the tree, and it was not worth it
next to the low hanging fruit taken here.
