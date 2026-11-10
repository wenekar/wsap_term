# High-refresh displays

A terminal on a 120 Hz or a 240 Hz display must show each change in one frame
period. It must also keep input and output moving between frames.

## Frame stages

Four stages stay apart. A debug report, a test, or a benchmark must not mix them.

| Stage | Meaning |
| --- | --- |
| Draw | The loop builds and encodes a frame. |
| Submission | The GPU took the frame. The frame is not on screen yet. |
| GPU completion | The GPU finished the frame, so the frame slot is free again. |
| Presentation | The display showed the frame. Only the display reports this. |

`--stats-ms` prints one line for each interval:

    [stats   7.52s] frames=752 drop=2 submit=750 present=748 fps=99.8 dt(ms) ...

- `frames` counts the draw passes. It is the loop rate.
- `submit` counts the passes that the GPU or the surface took.
- `drop` counts the passes that did not submit. A busy GPU or a missing
  drawable gives a drop, and the window asks for the next frame.
- `present` counts the frames that a display reported. It is zero on a
  platform without presentation feedback. The count is current when it is read,
  also while the window draws nothing: a report does not wait for a draw.

A submitted frame can be presented one or more frames later. A frame that the
display discards is never counted as presented.

## Pacing

A window takes one of two paths:

- With vsync on, the window requests a frame from the surface. The window layer
  answers the request at its next display callback, or after one refresh
  interval when it has no callback. The draw therefore waits for the display,
  and the window cannot draw faster than the display. The request wakes that
  answer only. It never draws at once, so an animation cannot outrun the
  display by asking for a frame in each refresh.
- With `vsync = false`, the draw starts at once. The display does not pace the
  frame, so a frame can tear. A present that finds no free drawable blocks
  until the display frees one.

The display link of a macOS window measures its own period. On a ProMotion
display that period follows the rate that the system chose, so it varies. A
frame request uses that period for its fallback timer.

## Behaviour by refresh rate

The table states the behaviour of the code. It is not a measurement.

| Case | 60 Hz fixed | 120 Hz fixed | 240 Hz fixed | 120 Hz variable (ProMotion) |
| --- | --- | --- | --- | --- |
| vsync on | One frame per 16.7 ms tick. | One frame per 8.3 ms tick. | One frame per 4.2 ms tick. | One frame per tick; the tick follows the rate that the system chose. |
| vsync off | Draws at once; tearing possible. | Draws at once; tearing possible. | Draws at once; tearing possible. | Draws at once; tearing possible. |
| Monitor move | The link follows the new display and measures its period. | Same. | Same. | The link measures the new rate after the move. |
| Scale change | `drawableSize` changes in points × scale; a frame whose drawable has another size is dropped and drawn again. | Same. | Same. | Same. |
| Live resize | The frame draws at once and presents in the Core Animation transaction. No tick wait. | Same. | Same. | Same. |
| Flood output | The worker thread parses, so a frame gets loop time at every rate. | Same. | Same. | Same. |
| Idle, then wake | The link pauses after 8 idle frames; the wake draws at once. | Same. | Same. | Same. |
| Occluded | The window is suspended and draws nothing. It draws again when it is visible. | Same. | Same. | Same. |

Two bounds do not depend on the rate:

- The GPU holds three frame slots. With no free slot the frame is dropped at
  once, and the window asks for the next frame.
- The PTY parse runs on the worker thread, so the main thread holds no budget.
  The worker parses one read in pieces of 32 KiB, so a snapshot waits for one
  piece at most.

## Presentation feedback on macOS

The Metal backend attaches one presented handler to the drawable of each frame.
The handler reads `presentedTime` and the time it runs, and it writes both to
the latency probe. The frame carries the identity of its request into the
handler, so late or out-of-order feedback cannot complete another sample.

- `presentedTime` is zero when the display did not show the frame. The probe
  then records a discard, not a presentation.
- A real presentation also sets the supported flag of the probe, including the
  frames of the startup screen, which carry no request sample. The benchmark
  reads that flag before it starts, so it must not depend on a finished sample.
- One record per sample is published. The first presentation of a sample is the
  one that counts, so a second report cannot change a record that a reader
  already took. A report of an older sample cannot complete a newer one, also
  when the report was delayed across the start of the newer request: the probe
  checks the request again under the lock that publishes a record. The fields of
  one record are written under a small lock that covers the record only; no
  renderer work and no callback runs while it is held.
- A frame carries the sample until a display presents that sample, or until the
  benchmark starts the next one. The submission of a frame is separate. A frame
  that the display discards retires nothing, so a later frame of the same
  request carries the sample again and can publish it.
- The probe sets the vsync flag only for a paced frame. A frame that
  draws with `vsync = false`, and a frame that presents inside the Core
  Animation transaction of a live resize, carry no vsync flag.
- The handler runs on a thread of the display. It holds one reference of the
  presentation count, so it reads no window, no surface, and no layer. A window
  that closes leaks nothing.
- `presentedTime` uses the Mach host clock, which stops while the machine
  sleeps. The probe uses the loop clock, which keeps counting. The two clocks
  differ, so the code maps `presentedTime` through the offset that they have
  when the handler runs.

## Measure it

- `wsap_term --stats-ms 1000` prints the four counts.
- `WSAP_LATENCY_PROBE` with `benchmarks/input-latency-bench` measures input to
  presentation. The benchmark needs a real display that presents to the
  session. See [input latency](../benchmarks/input-latency-bench.md).
- `render-bench` measures the CPU render path only. It reports no presentation.

## Limits

- No 240 Hz hardware was available for the work in this repository. 120 Hz was
  measured on a live display. No 240 Hz frame rate is claimed.
- A display that does not present to the session gives no presentation
  feedback. A remote or occluded session then reports submissions only.
- The window layer of Wayland reports no presentation feedback to the probe, so
  a Wayland run reports submissions only.
- `presentedTime` measures the display commit, not the panel response. It does
  not include the scanout of the panel or the physical response time.

## References

- [Apple: MTLDrawable.addPresentedHandler](https://developer.apple.com/documentation/metal/mtldrawable/addpresentedhandler(_:))
- [Apple: MTLDrawable.presentedTime](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime)
- [Apple: CACurrentMediaTime](https://developer.apple.com/documentation/quartzcore/cacurrentmediatime())
- [Apple: CADisplayLink](https://developer.apple.com/documentation/quartzcore/cadisplaylink)
- [Apple: CADisplayLink.targetTimestamp](https://developer.apple.com/documentation/quartzcore/cadisplaylink/targettimestamp)
- [Apple: CADisplayLink.duration](https://developer.apple.com/documentation/quartzcore/cadisplaylink/duration)
- [Apple: CAMetalLayer.displaySyncEnabled](https://developer.apple.com/documentation/quartzcore/cametallayer/displaysyncenabled)
- [Apple: CAMetalLayer.presentsWithTransaction](https://developer.apple.com/documentation/quartzcore/cametallayer/presentswithtransaction)
- [Apple: CAMetalLayer.allowsNextDrawableTimeout](https://developer.apple.com/documentation/quartzcore/cametallayer/allowsnextdrawabletimeout)
