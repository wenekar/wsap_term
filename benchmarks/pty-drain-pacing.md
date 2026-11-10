# PTY Drain Pacing

> Superseded. The main thread no longer parses PTY output. The ring worker
> thread parses each read into the terminal at once, so the drain budget, the
> read chunk, and the wakeup threshold no longer exist. Keep this report as the
> record of the pacing pass.

This report covers the drain budget, the read chunk, and the reader wakeup
threshold of the PTY path.

## What changed

- `PTY_DRAIN_BUDGET_NS` stays at 2 ms, and `PTY_DRAIN_BUDGET_MAX_NS` is 10 ms.
  `pty_drain_budget_next()` doubles the pass budget while a pass stops on its
  budget with output still to read. A stream that runs dry returns the base
  budget. Input and a frame keep the 2 ms bound unless output is streaming.
- `pty_chunk_next()` takes the budget of the pass. A chunk that parses in less
  than a quarter of the budget doubles. A chunk that parses in more than half
  of the budget halves. A chunk between those times keeps its size. A stream at
  speed then keeps `PTY_CHUNK_MAX`, and a 500 µs parse no longer shrinks it.
- `PTY_SIGNAL_BYTES` falls from 128 KiB to 32 KiB. The reader wakes the
  consumer with less data in the ring, so the ring and the kernel PTY buffer
  stay empty.

## Method

The harness is a copy of `tests/session/drain_flood_test.c` with a larger flood
and a timer print. It links the session and pty libraries of one snapshot, so a
run covers one build:

- Line flood: 600 000 lines of 32 bytes, 18.3 MiB.
- Random flood: 600 000 blocks of 64 KiB from a fixed xorshift seed, 18 MiB.
  This payload parses slowly, so the consumer is the limit.

Both runs use `WSAP_FAST_PIPE=0`, so the child writes to the kernel PTY.

Host: macOS 27, arm64, Homebrew LLVM, `-O3 -march=native -flto`. Medians of
interleaved runs of two snapshots.

## Results

### Line flood through the kernel PTY

| Build | Time | Throughput | Passes |
| --- | ---: | ---: | ---: |
| before | 165.2 ms | 110.8 MiB/s | 291 |
| after | 165.2 ms | 110.8 MiB/s | 291 |

The host limits this case, not the drain. The macOS PTY master delivers 1 KiB
per read at 172 to 200 MiB/s, and the consumer drains each ring in less than
100 µs. A drain pass ends on an empty ring, so the budget does not bind.

### Random flood through the kernel PTY

| Build | Time | Throughput | Passes |
| --- | ---: | ---: | ---: |
| before | 2027 ms | 8.9 MiB/s | 790 |
| after | 2122 ms | 8.5 MiB/s | 173 |

The consumer limits this case, so every pass stops on its budget. The new
pacing cuts the passes by 4.6 times. The throughput of the two builds is the
same within the spread. The before build reads 6.2 to 9.2 MiB/s, and the after
build reads 8.0 to 8.8 MiB/s, because the random bytes make variable escape
sequences. The pass overhead is small against the parse of the bytes.

### Traced pass and chunk

A temporary trace of the random flood shows the two changes in the pass:

| Build | Pass time | Chunk |
| --- | ---: | ---: |
| before | 2.0 to 2.8 ms | 8 KiB |
| after | 10.0 to 13.9 ms | 32 KiB |

The chunk of the before build falls to `PTY_CHUNK_MIN` and stays there. The
after build settles at a chunk whose parse takes about a quarter of the pass
budget.

## Not measured

`benchmarks/termbench` does not run here. The live run needs a desktop session,
and `benchmarks/run-termbench.sh` uses `taskset`, which is Linux only.

The gain belongs to the live app, where each frame and each drain pass share
the loop. `history-descriptors.md` records 2205 drain events at 1765 µs of mean
work for a live ManyLine run of 6.44 GB. Those passes stopped on the 2 ms
budget. The new pacing takes about 650 events for the same bytes. Add a Linux
live run before you rely on this change for throughput.

## Validation

```sh
meson test -C build session-drain-flood-test
python3 tools/dev/dirgraph.py build
```
