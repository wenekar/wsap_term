# Session ingestion benchmark

Build and run the benchmark:

```sh
meson compile -C build session-ingest-bench
./build/benchmarks/session-ingest-bench all 64 5
```

The arguments select the case, source size in MiB, and sample count.
The defaults are `all`, 64 MiB, and five samples.
Each case also runs one warmup sample.
The output gives the median rate and its minimum and maximum.
It also gives wall time and session CPU time per source byte.
CPU time includes the main thread and the reader worker.
It excludes the child writer.

Use separate CPUs for the session and the writer:

```sh
WSAP_BENCH_WRITER_CPU=5 taskset -c 4 \
    ./build/benchmarks/session-ingest-bench all 256 5
```

The session and its worker inherit the `taskset` mask.
The helper uses `WSAP_BENCH_WRITER_CPU` when it is set.
Use the same CPU placement for each build.

## Measured path

The benchmark creates a real session without a display.
The session starts its child through `pty_spawn`.
The reader worker reads the pipe or PTY.
The session feeds these bytes to the terminal core.
The main loop receives output through the session mailbox.
The benchmark fails if the worker does not start.
It also fails if the child gets the wrong transport.

The child creates its payload before the timer starts.
A separate Unix socket controls the start and end.
The child waits for the start message before it writes.
It writes a parser completion marker after the payload.
The timer stops when the main thread receives that marker as a title.
The child keeps its output descriptors open until the parent sends an acknowledgement.
Thus a PTY close cannot discard the last bytes.
The socket does not carry the measured payload.
The helper uses one transport for the whole sample.
The routing tests cover moves between transports.

The timer includes the start message, child scheduling, transport, parsing,
and final mailbox delivery.
It excludes setup, payload generation, result checks, and session teardown.
The parent reaps the child before the next sample starts.
It excludes drawing and presentation.
The main thread does not take snapshots during the timed interval.
Use the live benchmarks to measure snapshot contention and drawing.

## Cases and result checks

| Case | Transport | Source |
| --- | --- | --- |
| `pipe/manyline` | Fast pipe | Letters with LF every 80 bytes |
| `pipe/longline` | Fast pipe | Letters without line breaks |
| `pipe/fgbg` | Fast pipe | Truecolor foreground and background changes |
| `pipe/random` | Fast pipe | Bytes from a fixed random seed |
| `pty/cooked` | PTY with `OPOST` and `ONLCR` | Same text as `pipe/manyline` |
| `pty/raw` | PTY without output processing | Same text as `pipe/manyline` |
| `pipe/mixed` | Fast pipe | Alternating MiB of long lines and color changes |

Rates count source bytes.
They exclude the extra CR bytes that the cooked PTY adds.
The output reports the actual pipe capacity.

After each sample, the benchmark compares the visible cells and cursor with
a terminal core fed directly from the same payload.
It also checks the common suffix of the last 1000 history rows.
The first retained row can be a partial wrapped row.
The check permits that boundary to differ by less than one row of UTF-8 text.
It does not require equal history counts.
The current core can retain different history lengths at different feed sizes.
These checks do not replace the byte fidelity and routing tests.

Each timed sample has a 30 second limit.
Use a smaller source size for slow cases.
A failed writer, timeout, missing marker, or failed result check returns failure.
Do not use partial results from a failed run.

## Comparison method

Keep binaries from both builds.
Alternate them with the same case, source size, and CPU placement.
Use at least five samples per build.
Record the CPU, build flags, pipe capacity, and source changes.
Check throughput and CPU time together.
Also run the routing, input queue, exit, and flood close tests.


## Worker pass: 2026-10-19

The reference includes the ingestion thread work already in the worktree.
Both builds use the same final benchmark source.
This pass makes these changes:

- Replace the shared ring state with one buffer owned by the worker.
- Reduce the mapped buffer from 4 MiB to 256 KiB.
- Retry full batches before polling again.
- Check a 100 microsecond poll deadline between batches.
- Combine frame notices during full drains at a one millisecond interval.
- Flush pending notices before waiting, at routing barriers, and at shutdown.
- Keep reply flushes after each feed.
- Bind the input queue before the reader starts.

Each read batch still gets fresh newline settings.
The parser consumes every byte.
The final policy keeps 256 KiB batches.
Smaller batches improved some isolated runs.
They reduced live ManyLine throughput.

The host was an AMD Ryzen 7 9700X on Linux x86-64.
The compiler was GCC 16.2.1.
Both builds used `-O3 -march=native` and LTO.
The CPU governor was `powersave`.
The pipe capacity was 1024 KiB.

### Session path

The session and worker ran on CPU 4.
The child writer ran on CPU 5.
Each run used one warmup and three timed samples.
The table gives medians of run medians.
Most cases used five alternating runs per build.
Random and cooked PTY used three runs per build.

| Case | MiB | Before MiB/s | After MiB/s | Change | Before CPU ns/B | After CPU ns/B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `pipe/manyline` | 256 | 12640.2 | 12620.8 | -0.2% | 0.0747 | 0.0751 |
| `pipe/longline` | 256 | 12310.1 | 12043.0 | -2.2% | 0.0767 | 0.0783 |
| `pipe/fgbg` | 256 | 3123.2 | 3093.5 | -1.0% | 0.3042 | 0.3079 |
| `pipe/random` | 64 | 82.8 | 83.0 | +0.2% | 11.5069 | 11.4709 |
| `pty/cooked` | 64 | 157.5 | 158.7 | +0.8% | 4.6619 | 4.6525 |
| `pty/raw` | 256 | 510.1 | 514.3 | +0.8% | 1.7713 | 1.7815 |
| `pipe/mixed` | 256 | 3451.3 | 3492.6 | +1.2% | 0.2759 | 0.2726 |

### Live terminal

A private headless Sway used the pixman renderer.
The app and worker used CPUs 4 and 6.
The writer used CPU 5.
Each case wrote 1 GiB.
One warmup run preceded three alternating runs per build.
The rates below use GiB/s.
The helper labels this unit `gb/s` in its output.

| Case | Before GiB/s | After GiB/s | Change |
| --- | ---: | ---: | ---: |
| ManyLine | 7.5126 | 8.7893 | +17.0% |
| LongLine | 10.3263 | 10.2135 | -1.1% |
| FGPerChar | 1.9998 | 1.9366 | -3.2% |
| FGBGPerChar | 2.3162 | 2.3160 | 0.0% |
| termbench-v3 normal | 3.4657 | 3.4399 | -0.7% |

Live ManyLine ranged from 7.15 to 7.99 GiB/s before the change.
It ranged from 8.58 to 8.88 GiB/s after the change.
The aggregate changed by -0.7%.
The gain is specific to line floods.
These results do not show a general throughput gain.

### Input latency

The same private compositor used the Wayland virtual keyboard injector.
The benchmark, app, and helpers used CPUs 4, 5, and 6.
Each condition used one run with 100 samples after 20 warmup samples.
The probe measured injection to frame submission.
Presentation feedback was unavailable.

| Condition | Before p50 ms | After p50 ms | Before p99 ms | After p99 ms |
| --- | ---: | ---: | ---: | ---: |
| idle | 0.298 | 0.282 | 0.360 | 0.400 |
| flood | 0.200 | 0.197 | 0.343 | 0.301 |

The flood ran in a background tab.
The foreground tab ran the echo helper.
All four runs completed all samples.

### Checks

- All 138 tests passed.
- Eight PTY and session tests passed under AddressSanitizer and UndefinedBehaviorSanitizer.
- The same eight tests passed under ThreadSanitizer.
- The parser and renderer benchmarks passed.
- The include graph had no layer violations or cycles.

The new idle output test checks a large burst while the child stays open.
The final marker must reach the visible screen before the child exits.
The routing test now stops the loop on its main thread.
ThreadSanitizer also found the input queue startup race.
The queue now binds before the reader thread starts.
