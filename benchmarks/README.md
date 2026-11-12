# Benchmarks

This directory contains benchmark programs, scripts, and investigation reports.

## Running benchmarks

Run the live terminal benchmark:

```sh
meson compile -C build term-live-bench
build/benchmarks/term-live-bench
```

Run headless terminal and renderer benchmarks:

```sh
meson test -C build --benchmark term-write-bench --verbose
meson test -C build --benchmark render-bench --verbose
```

Run input latency and transport benchmarks:

```sh
meson test -C build --benchmark input-latency-bench --verbose
meson test -C build --benchmark transport-bench --verbose
```

## Benchmark reports

### Terminal throughput

| Report | Topic |
| --- | --- |
| [termbench-throughput.md](termbench-throughput.md) | Full termbench throughput and comparisons. |
| [termbench-csi.md](termbench-csi.md) | CSI sequence parsing and branch cost reduction. |
| [termbench-investigation.md](termbench-investigation.md) | Profiler bottleneck investigation for termbench. |
| [term-write-30gib.md](term-write-30gib.md) | 30 GiB write stress test and memory usage. |
| [linefeed-bursts.md](linefeed-bursts.md) | Vectorized line feed processing. |
| [mixed-utf8.md](mixed-utf8.md) | Mixed ASCII and multi-byte UTF-8 parsing throughput. |

### Terminal model and storage

| Report | Topic |
| --- | --- |
| [term-redesign.md](term-redesign.md) | Terminal cell grid and memory model redesign. |
| [term-refactor.md](term-refactor.md) | Parser and model separation. |
| [term-line-streams.md](term-line-streams.md) | Line stream encoding in memory. |
| [term-ng-sync.md](term-ng-sync.md) | Terminal synchronization barrier performance. |
| [lazy-rows.md](lazy-rows.md) | Lazy row allocation and generation tracking. |
| [blank-rows.md](blank-rows.md) | Blank row detection and fast clears. |
| [layout.md](layout.md) | Cell geometry and glyph layout caching. |
| [history-descriptors.md](history-descriptors.md) | Scrollback page descriptor indexing. |
| [history-insertion.md](history-insertion.md) | Scrollback ring buffer append speed. |
| [cursor-erase.md](cursor-erase.md) | Fast erase paths for cursor commands. |
| [cursor-loop.md](cursor-loop.md) | Cursor position state hoisting. |
| [manyline-profile.md](manyline-profile.md) | CPU cache profile for large scrollback buffers. |

### Latency and input

| Report | Topic |
| --- | --- |
| [input-latency-bench.md](input-latency-bench.md) | End-to-end key press to presentation latency. |
| [input-queue-bench.md](input-queue-bench.md) | Lock-free input queue throughput and contention. |
| [resize-latency.md](resize-latency.md) | 1000 Hz resize drag latency and configure timing. |

### PTY and transport

| Report | Topic |
| --- | --- |
| [transport-bench.md](transport-bench.md) | PTY master-slave roundtrip bandwidth. |
| [pty-drain-pacing.md](pty-drain-pacing.md) | Output flood detection and drain pacing. |
| [pty-wake-batching.md](pty-wake-batching.md) | Reader worker wakeup batching. |

### UI and renderer

| Report | Topic |
| --- | --- |
| [render-rewrite.md](render-rewrite.md) | Two-pass row rasterizer and cell tile cache. |
| [ui-frame.md](ui-frame.md) | UI layout pass and scene render benchmarks. |
| [macos-metal.md](macos-metal.md) | Metal GPU backend throughput and resource binding. |
| [font-fallback-bench.md](font-fallback-bench.md) | Font fallback lookup cache performance. |
| [window-backend.md](window-backend.md) | Native window presentation timing. |

### Tooling and environment

| Report | Topic |
| --- | --- |
| [term-live-bench.md](term-live-bench.md) | Interactive live terminal benchmark setup. |
| [test-runtime.md](test-runtime.md) | Test suite execution time optimization. |
| [tool-cost.md](tool-cost.md) | Sanitizer, profiler, and tool overhead measurements. |
