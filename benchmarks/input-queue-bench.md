# Input Queue Benchmark: Event Round-Trip and Paste Drain

Run with:

```sh
meson test -C build --benchmark input-queue-bench --verbose
```

This benchmark exercises the event queue across two operations:

- **Key / motion round-trip:** push one event, pop it, repeat. Measures the
  queue crossing cost paid by every keystroke and mouse event.
- **Paste drain:** streaming multi-kilobyte payloads through the queue, measuring
  chunking and drain throughput.

Arguments: `input-queue-bench [iterations] [paste_bytes]`, defaulting to
5,000,000 and 64 KiB.

## Numbers (Linux, x86-64, GCC, `-O3 -march=native -flto`)

Measured on a Ryzen 7 9700X, pinned with `taskset -c 2`.

| Payload Layout | Event Size | Key push+pop | Motion push+pop |
| --- | ---: | ---: | ---: |
| Inline paste payload | 4112 B | ~42–47 ns | ~42 ns |
| Heap paste payload | 120 B | ~11.4 ns | ~11.4 ns |

Paste drain: ~1.05 µs per 64 KiB (~58 GiB/s).

## Interpretation

Reducing event structure size from over 4 KiB to 120 B lowers the queue crossing
latency by ~3.7x and reduces total event ring buffer footprint significantly.
Paste operations remain bounded by transfer and terminal write speeds rather
than queue throughput.
