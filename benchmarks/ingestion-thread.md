# Ingestion Thread Benchmark: Baseline Metrics

AMD Ryzen 7 9700X, 8-Core Processor, Linux x86_64, GCC, `-O3 -march=native -flto`.
Baseline recorded on the `transport-rewrite` branch before moving `wsap_vtcore_feed()` off the main UI loop.

## Summary

Today the main thread drains the PTY ring, parses terminal output, and draws frames.
A dense escape-sequence flood in one tab drains for up to 16 ms per loop turn (`PTY_DRAIN_BUDGET_NS`).
During a background random-byte flood in Tab A, foreground key-to-commit latency in Tab B jumps from 1.76 ms (p50) to 61.67 ms (p50) and 71.51 ms (p99).
Primary Device Attributes (DA1) round-trip latency at p50 increases from 28.4 µs to 508.4 µs (17.9x) under a flood.

## Phase 0 Instrumentation Fixes

Phase 0 fixed five baseline measurement and locking defects before recording these numbers:

1. `terminal_prepare` and `terminal_frame_source` both called `wsap_vtcore_snapshot_update` and `frame_stats_snapshot_wait` on each frame. The second call was a ~2.5 µs no-op that skewed `snap_wait` downward by 50%. `terminal_frame_source` now reuses the snapshot prepared in `terminal_prepare`.
2. Main-thread frame preparation took the `wsap_vtcore` lock 9 times per frame (`root_prepare`, `blinking`, `terminal_prepare`, `update_ime` x3, `cursor_visible` x2, and `terminal_frame_source`). Adding `history_lines` to `struct wsap_vtcore_view` and reading cursor, scrollback, and blink state from the snapshot reduced main-thread lock acquisitions to 1 per frame.
3. Compound main-thread reads in `src/terminal/` (`draw.c`, `input.c`, `selection.c`, `view.c`, `menu.c`) now batch their `wsap_vtcore_*` calls under one `wsap_vtcore_lock` / `wsap_vtcore_unlock` critical section.
4. `latency_probe_scan_output` previously used non-atomic static state and cleared `pending` on any single-byte chunk. A background flood in Tab A wiped Tab B's pending probe state and caused false timeouts. The probe now uses atomic state, thread-local parser state, and `memchr` to skip non-ESC flood runs at SIMD speed.
5. `frame_stats` previously used 1 ms and 10 µs buckets and did not report p50 or p95. It now uses 2000 buckets at 0.1 ms (`dt`) and 1.0 µs (`snap_wait`) resolution and prints `avg`, `min`, `p50`, `p95`, `p99`, and `max`.

## Metrics Baseline (M1 – M6)

### M1 & M4: Frame Interval and Snapshot Wait Under Flood

Measured during `cat /dev/urandom | base64` with `--stats-ms 4000` (5 runs, median run by max `dt`):

| Metric | Min | Avg | p50 | p95 | p99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Frame Interval `dt(ms)` | 0.46 ms | 0.59 ms | **0.50 ms** | 0.60 ms | **0.80 ms** | **18.52 ms** |
| Snapshot Wait `snap_wait(us)` | 6.6 µs | 35.1 µs | **34.0 µs** | 37.0 µs | **63.0 µs** | **515.2 µs** |

All 5 runs:

| Run | `dt` avg | `dt` p50 | `dt` p95 | `dt` p99 | `dt` max | `snap_wait` avg | `snap_wait` p50 | `snap_wait` p95 | `snap_wait` p99 | `snap_wait` max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.58 ms | 0.50 ms | 0.60 ms | 0.80 ms | 17.56 ms | 34.8 µs | 34.0 µs | 36.0 µs | 58.0 µs | 141.3 µs |
| 2 | 0.59 ms | 0.50 ms | 0.60 ms | 0.90 ms | 4.42 ms | 35.0 µs | 34.0 µs | 36.0 µs | 57.0 µs | 438.9 µs |
| 3 | 0.59 ms | 0.50 ms | 0.60 ms | 0.90 ms | 4.55 ms | 35.0 µs | 34.0 µs | 37.0 µs | 60.0 µs | 649.7 µs |
| 4 | 0.59 ms | 0.50 ms | 0.60 ms | 0.80 ms | 18.52 ms | 35.1 µs | 34.0 µs | 37.0 µs | 63.0 µs | 515.2 µs |
| 5 | 0.60 ms | 0.50 ms | 0.60 ms | 0.90 ms | 14.08 ms | 35.2 µs | 34.0 µs | 38.0 µs | 62.0 µs | 366.5 µs |

Notes:
- The maximum frame interval spikes up to 18.52 ms when a drain cycle hits the 16 ms budget before a frame commit.
- `snap_wait` measures the lock acquisition and incremental snapshot copy time on the main thread (34.0 µs at p50, 63.0 µs at p99).

### M2: Key-to-Commit Latency (Idle)

Measured with `input-latency-bench` (50 samples, 10 warmup, median of 5 runs):

| Stage | Min | Mean | p50 | p95 | p99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `injection -> key callback` | 0.098 ms | 0.124 ms | 0.121 ms | 0.156 ms | 0.170 ms | 0.170 ms |
| `key -> input queued` | 0.015 ms | 0.017 ms | 0.016 ms | 0.021 ms | 0.023 ms | 0.023 ms |
| `queued -> echo parsed` | 0.026 ms | 0.041 ms | **0.040 ms** | 0.056 ms | **0.061 ms** | 0.061 ms |
| `parsed -> render start` | 0.001 ms | 0.002 ms | 0.001 ms | 0.002 ms | 0.002 ms | 0.002 ms |
| `render -> submission` | 1.423 ms | 1.613 ms | 1.581 ms | 1.772 ms | 2.333 ms | 2.333 ms |
| `injection -> submission` | 1.604 ms | 1.797 ms | **1.764 ms** | 2.007 ms | **2.496 ms** | 2.496 ms |

### M3: Key-to-Commit Latency in Tab B While Tab A Floods

Measured with `input-latency-bench --bg-flood` (50 samples, 10 warmup, median of 5 runs).
Tab A runs an unthrottled pseudo-random byte flood in the background while Tab B echoes injected keys in the foreground:

| Stage | Min | Mean | p50 | p95 | p99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `injection -> key callback` | 0.388 ms | 7.795 ms | **6.865 ms** | 16.332 ms | **17.176 ms** | 17.176 ms |
| `key -> input queued` | 17.127 ms | 17.567 ms | **17.544 ms** | 17.889 ms | **18.026 ms** | 18.026 ms |
| `queued -> echo parsed` | 34.496 ms | 35.065 ms | **35.074 ms** | 35.541 ms | **35.595 ms** | 35.595 ms |
| `parsed -> render start` | 0.002 ms | 0.002 ms | 0.002 ms | 0.003 ms | 0.003 ms | 0.003 ms |
| `render -> submission` | 1.687 ms | 1.868 ms | 1.846 ms | 2.094 ms | 2.654 ms | 2.654 ms |
| `injection -> submission` | 54.596 ms | 62.297 ms | **61.669 ms** | 70.585 ms | **71.511 ms** | 71.511 ms |

Breakdown of the ~62 ms main-thread stall:
- `injection -> key callback` waits up to one 16 ms Tab A drain turn (6.87 ms at p50, 17.18 ms at p99) before the event loop polls the debug socket.
- `key -> input queued` waits one full 16 ms Tab A drain turn (17.54 ms at p50) before the scheduled input flush (`window_drain_input`) writes the key to Tab B.
- `queued -> echo parsed` waits two full 16 ms Tab A drain turns (35.07 ms at p50) before Tab B's echo is drained and rendered.
- Note: Under a pure ASCII line flood (`manyline`, parsed at 22 GB/s), `on_ring_event` drains the 4 MiB ring in under 0.2 ms and never reaches `PTY_DRAIN_BUDGET_NS`. Under that workload, Tab B key-to-commit latency is 2.172 ms at p50 and 2.495 ms at p99. The stall occurs when Tab A parses dense escape sequences or random bytes (~71 MiB/s) and exhausts the 16 ms main-thread drain budget on every turn.

### M5: Flood Throughput

#### Live Termbench (`run-termbench.sh normal`, 5 runs, median [min–max])

| Case | Median Throughput | Range |
| --- | ---: | ---: |
| `ManyLine` | **9.256 GB/s** | [8.443 – 9.492] |
| `LongLine` | **4.025 GB/s** | [4.005 – 4.091] |
| `FGPerChar` | **1.366 GB/s** | [1.350 – 1.374] |
| `FGBGPerChar` | **1.530 GB/s** | [1.524 – 1.537] |
| `termbench-v3 normal` | **2.292 GB/s** | [2.280 – 2.311] |

#### Parser Core Throughput (`term-write-bench`, chunk = 64 KiB)

| Workload | Throughput (MiB/s) | ns / Byte |
| --- | ---: | ---: |
| `lf-burst` | 173,620 MiB/s | 0.005 ns |
| `longline` | 44,145 MiB/s | 0.022 ns |
| `manyline` | 22,148 MiB/s | 0.043 ns |
| `ascii` | 7,124 MiB/s | 0.134 ns |
| `fgbg-char` | 2,832 MiB/s | 0.337 ns |
| `cjk` | 1,684 MiB/s | 0.566 ns |
| `colors` | 711 MiB/s | 1.341 ns |
| `utf8` | 731 MiB/s | 1.303 ns |
| `random` | 71 MiB/s | 13.356 ns |

### M6: Primary Device Attributes (DA1) Round Trip

Measured with `da1-bench` (50 samples, 5 runs median):

| Condition | Min | Avg | p50 | p95 | p99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Idle | 21.3 µs | 60.4 µs | **28.4 µs** | 36.9 µs | 1591.8 µs | 1591.8 µs |
| Under Flood | 83.6 µs | 522.2 µs | **508.4 µs** | 891.4 µs | **1723.0 µs** | 1723.0 µs |

Notes:
- Under a same-tab flood, p50 DA1 response latency increases from 28.4 µs to 508.4 µs (17.9x).
- p95 DA1 response latency increases from 36.9 µs to 891.4 µs (24.2x).

## Post-Implementation Results (Step B)

Step B moved `wsap_vtcore_feed` to `pty_ring_worker`.
It added the turnstile contention API (`wsap_vtcore_lock_contended`, `wsap_vtcore_yield_to_waiters`).
It made `session_write_queue` thread-safe with non-blocking deferred syscall flush.
It routed host callbacks through the session mailbox eventfd.

### Summary of Improvements (AMD Ryzen 7 9700X)

| ID | Metric | Baseline (Phase 0) | Step B Result | Change |
| --- | --- | ---: | ---: | --- |
| M1 | Frame interval max under flood | **18.52 ms** | **4.73 ms** | **3.9x lower** max frame spike |
| M2 | Key-to-commit latency (idle) p50 | **1.764 ms** | **1.593 ms** | 10% faster |
| M3 | Foreground key latency under flood p50 | **61.669 ms** | **1.669 ms** | **37.0x faster** (stall eliminated) |
| M3 | Foreground key latency under flood p99 | **71.511 ms** | **2.651 ms** | **27.0x faster** |
| M4 | Snapshot wait p99 under flood | **63.0 µs** | **49.0 µs** | Target was ≤ 500 µs |
| M4 | Snapshot wait max under flood | **515.2 µs** | **418.8 µs** | Target was ≤ 2000 µs |
| M5 | Live termbench (`normal`) | **2.292 GB/s** | **3.203 GB/s** | **+39.7% higher** throughput |
| M5 | Live termbench (`LongLine`) | **4.025 GB/s** | **8.705 GB/s** | **+116.3% higher** throughput |
| M6 | DA1 round-trip (idle) p50 | **28.4 µs** | **20.5 µs** | 28% faster |
| M6 | DA1 round-trip (under flood) p50 | **508.4 µs** | **173.3 µs** | **2.9x faster** |
| M6 | DA1 round-trip (under flood) max | **1723.0 µs** | **398.8 µs** | **4.3x lower** max latency |

