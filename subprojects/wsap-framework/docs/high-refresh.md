# High-refresh displays

> The examples name a host application. Use your own program in its place.


This guide explains how the window layer paces frames and reports presentation. The display refresh rate sets the frame interval; it does not guarantee that each input appears within one interval.

## Frame stages

The frame pipeline has four distinct stages.
Do not mix these stages in debug reports, tests, or benchmarks.

| Stage | Meaning |
| --- | --- |
| Draw | The loop builds and encodes a frame. |
| Submission | The GPU receives the frame. The frame is not yet on screen. |
| GPU completion | The GPU finishes the frame. The frame slot becomes free. |
| Presentation | The display shows the frame. Only the display reports this stage. |

The `--stats-ms` option prints one line for each interval:

    [stats   7.52s] frames=752 drop=2 submit=750 present=748 fps=99.8 dt(ms) ...

- `frames` counts draw passes, including passes that could not start drawing. Those passes do not count as submitted or dropped.
- `submit` counts passes accepted by the GPU or the surface.
- `drop` counts passes that did not submit. A busy GPU or a missing drawable causes a drop. The window then requests the next frame.
- `present` counts frames reported by the display. This count is zero on platforms without presentation feedback. The count updates when read, even when the window draws nothing. A report does not wait for a draw pass.

The display presents a submitted frame one or more frames later.
The display does not count discarded frames as presented.

## Pacing

A window uses one of two pacing modes:

- With vsync enabled, the window requests a frame from the surface.
  The window layer answers at the next display callback.
  On Cocoa, a fallback timer can answer when no display tick arrives.
  Drawing waits for the display.
  The window cannot draw faster than the display refresh rate.
  The request only schedules that callback.
  It does not draw immediately.
  An animation cannot outrun the display by requesting frames continuously.
- With `vsync = false`, drawing starts immediately.
  The display does not pace the frame.
  Screen tearing can occur.
  If no drawable is free, acquisition can block or fail, depending on the backend.

The display link of a macOS window measures its own refresh period.
On a ProMotion display, this period varies with the system refresh rate.
A frame request uses this period for its fallback timer.

## Cocoa pacing by refresh rate

The table describes Cocoa behavior. The tick intervals are nominal, not measured frame times.

| Case | 60 Hz fixed | 120 Hz fixed | 240 Hz fixed | 120 Hz variable (ProMotion) |
| --- | --- | --- | --- | --- |
| vsync on | One frame per 16.7 ms tick. | One frame per 8.3 ms tick. | One frame per 4.2 ms tick. | One frame per tick. The tick matches the active system rate. |
| vsync off | Draws immediately. Screen tearing is possible. | Draws immediately. Screen tearing is possible. | Draws immediately. Screen tearing is possible. | Draws immediately. Screen tearing is possible. |
| Monitor move | The link tracks the new display and measures its period. | Same. | Same. | The link measures the new refresh rate after the move. |
| Scale change | `drawableSize` changes to points × scale. A frame with a mismatched drawable size drops and redraws. | Same. | Same. | Same. |
| Live resize | The frame draws immediately and presents in the Core Animation transaction without waiting for a tick. | Same. | Same. | Same. |
| Idle, then wake | The link pauses after 8 idle frames. A wake event triggers an immediate draw. | Same. | Same. | Same. |
| Occluded | The window suspends drawing while occluded. It resumes drawing when visible. | Same. | Same. | Same. |

Metal has three frame slots. When all are busy, the renderer drops the frame and the window requests another. In `wsap_term`, a separate worker parses PTY output. Snapshot capture can wait for that worker to release the terminal writer lock.

## Presentation feedback on macOS

The Metal backend attaches a presentation handler to the drawable of each frame.
The handler reads `presentedTime` and the execution timestamp.
It writes both timestamps to the latency probe.
The frame carries the request identity into the handler.
Out-of-order feedback cannot complete a different sample.

- `presentedTime` is zero when the display does not show the frame.
  In this case, the probe records a discard instead of a presentation.
- A valid presentation sets the supported flag of the probe.
  This includes startup frames without a request sample.
  The benchmark reads this flag before start.
  The benchmark does not depend on a finished sample.
- The system publishes one record per sample.
  Only the first presentation of a sample counts.
  A duplicate report cannot modify an existing record.
  A delayed report for an old sample cannot complete a newer sample.
  The probe checks the request identity under the record lock.
  This lock protects record fields only.
  No renderer work or callback runs while holding the lock.
- A frame retains its sample until presentation occurs or until the next sample begins.
  Frame submission is independent of sample retirement.
  A discarded frame does not retire the sample.
  A later frame for the same request carries the sample and can publish it.
- The probe sets the vsync flag only for paced frames.
  Frames drawn with `vsync = false` omit the vsync flag.
  Frames presented during live resize Core Animation transactions omit the vsync flag.
- The handler executes on a display thread.
  It retains only a reference to the presentation count.
  It does not read window, surface, or layer pointers.
  Closing a window causes no memory leak.
- `presentedTime` uses the Mach host clock.
  The Mach host clock stops during system sleep.
  The probe uses the monotonic loop clock.
  The loop clock continues counting during sleep.
  The handler computes the clock offset and maps `presentedTime` to the loop clock.

## Measurement

- `wsap_term --stats-ms 1000` prints the four frame counters.
- `WSAP_LATENCY_PROBE` with `benchmarks/input-latency-bench` measures input-to-presentation latency.
  This benchmark requires a physical display that presents output.
  See [input latency measurements in `wsap_term`](../../../benchmarks/input-latency-bench.md).
- `render-bench` measures the CPU render path only.
  It reports no presentation data.

## Limitations

- The project measured 120 Hz on physical hardware.
  No 240 Hz test hardware was available.
  The project does not claim 240 Hz performance.
- A display that does not present output provides no presentation feedback.
  Remote or occluded sessions report submissions only.
- The Wayland window backend provides no presentation feedback to the probe.
  Wayland sessions report submissions only.
- `presentedTime` measures display commit time.
  It does not measure panel scanout or physical pixel response time.

## References

- [Apple: MTLDrawable.addPresentedHandler](https://developer.apple.com/documentation/metal/mtldrawable/addpresentedhandler(_:))
- [Apple: MTLDrawable.presentedTime](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime)
