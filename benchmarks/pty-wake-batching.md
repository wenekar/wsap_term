# PTY Wake Batching

This report covers the signal that the PTY ring worker gives the loop thread.

The worker parses each read into the terminal at once. Before this pass it woke
the loop thread after every read. The kernel pty of macOS returns about 1 KiB
per read, so a flood woke the loop thread for almost every 2 KiB.

The worker now parses 32 KiB before it wakes the loop thread. A burst that
stops sooner wakes the loop thread at once, when the source runs dry.

## What changed

- `src/pty/ring.c`: `pump()` counts the bytes of the reads that it parsed. It
  calls `wake_consumer()` when the count reaches `PTY_WAKE_BYTES`, which is
  32 KiB. It calls `wake_consumer()` again when the source runs dry, so a small
  burst of output keeps its prompt wake.
- `wake_pending` and `event_signal` are unchanged. The worker signals at most
  once for the content that waits, and the loop thread clears `wake_pending`
  when it drains the signal.

## Method

The harness is a temporary copy of `tests/session/drain_flood_test.c`. It reads
`WSAP_FLOOD_LINES` from the environment and prints the time and the count of
the wake callbacks. The flood is 600 000 lines of 32 bytes, 18.3 MiB. The child
writes to the kernel PTY, because `WSAP_FAST_PIPE` is not set.

Host: macOS 27, arm64, Homebrew LLVM, `-O3 -march=native -flto`. Seven runs of
each build, interleaved.

## Results

| Build | Time | Throughput | Wakes |
| --- | ---: | ---: | ---: |
| before | 189.7 ms | 96.5 MiB/s | 10 776 |
| after | 158.9 ms | 115.3 MiB/s | 606 |

The medians of seven runs. The before build ran at 95.8 to 99.0 MiB/s. The
after build ran at 114.8 to 116.4 MiB/s in that series, and at 119.4 to
120.6 MiB/s in a later series. The change gives about 19 percent more
throughput and 18 times fewer wakes.

A batch of 64 KiB gave the same result, because a burst on this host ends at
about 64 KiB. The batch stays at 32 KiB, which bounds the wait of the loop
thread during a long burst.

## Wake latency

`tests/pty/ring_wake_test.c` covers the prompt wake. It reads a pipe as the
transport of a `struct pty`, so it controls every byte and every boundary. It
checks these cases:

- A burst of 64 bytes wakes the loop thread. Without the signal at the end of
  the burst, the test stops at its 2 s timeout.
- A burst after a wake wakes the loop thread again.
- A stream of 8 KiB bursts wakes the loop thread for each part of it.
- A small burst at the end of a stream wakes the loop thread, and that wake
  sees every byte.

## Not measured

The live app does not run here. Each frame and each wake share the loop, so a
live run of `benchmarks/run-termbench.sh` is the next measurement. The Linux
fast pipe is not measured: a read of that transport can cross the batch alone,
so the change stays close to one signal per read there.

## Validation

```sh
meson test -C build-homebrew pty-ring-wake-test session-drain-flood-test
python3 tools/dev/dirgraph.py build-homebrew
```
