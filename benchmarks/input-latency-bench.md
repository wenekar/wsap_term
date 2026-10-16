# Input latency benchmark

```sh
meson compile -C build input-latency-bench wsap_term
./build/benchmarks/input-latency-bench 300 20 /tmp/latency.csv
```

Arguments are sample count, warmup count, and an optional CSV path. Defaults
are 300 and 20. CSV files must not already exist. By default, the benchmark
uses `bin/wsap_term` beside its own build directory. `WSAP_TERM_BIN` overrides
that path. The binary must use the same probe ABI. The benchmark requires a
live desktop session, keyboard focus, and access to `/dev/uinput` on Linux (or
Accessibility permission on macOS). Avoid typing or changing focus during a
run. Input uses physical A, so the active layout must produce lowercase `a`.

### Headless compositor

Set `WSAP_LATENCY_INJECTOR=wayland` to type through the
`zwp_virtual_keyboard_v1` protocol instead of uinput. The keys go only to the
compositor named by `WAYLAND_DISPLAY`, so the bench can run against a private
headless Sway without touching the desktop session:

```sh
XDG_RUNTIME_DIR=$(mktemp -d) WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_RENDERER=pixman sway --unsupported-gpu -c /dev/null &
WSAP_LATENCY_INJECTOR=wayland ./build/benchmarks/input-latency-bench 300 20
```

This path skips evdev and libinput, so its injection stage is shorter than
with uinput. Compare builds with the same injector only. The protocol XML is
vendored in `benchmarks/protocols/`, and meson runs `wayland-scanner` on it.

## Measurements

All timestamps use CLOCK_MONOTONIC nanoseconds. No TSC calibration, CPU
frequency estimates, or inferred core-cycle counts are involved.

- **Injection → submission:** immediately before the first synthetic input
  event write until the present call for the matching frame returns.
- **Injection → presentation:** on Wayland, the matching commit's
  `wp_presentation_feedback.presented` timestamp when the compositor reports
  vsync. In async mode, the benchmark uses the local time when it receives the
  feedback event. That time is an upper bound on presentation completion. It
  can overstate latency by the time spent waiting for and dispatching the event.
  Neither path measures panel response or physical key-to-photon latency.
  Hardware keyboard scanning and USB transport are outside the interval.
- **Stages:** input delivery to the terminal key callback; encoding and input
  queue submission; PTY transport plus helper scheduling, echo and VT parsing;
  wait before rendering; rendering and frame submission. The five stage
  intervals sum to injection → submission. Input queue submission may include
  an immediate write or leave bytes queued, so it is not labelled PTY write.
- **Submission → presentation:** compositor/display delay after submission.
- **Input → presentation frames:** measured time divided by the reported
  refresh period. For async presentation, this uses feedback receipt time, so
  it is a conservative upper bound. Non-warmup samples above two frames fail
  the run. The zero-copy presentation count reports whether the compositor
  confirmed that path. The benchmark requests fullscreen and waits for the
  compositor to confirm it in `xdg_toplevel.configure`. Fullscreen does not
  guarantee direct scanout. The compositor also checks the buffer and output.

The output reports min, mean, nearest-rank p50/p95/p99, and max in milliseconds.
The window layer is yasl, and yasl reports no presentation feedback. The probe
therefore never reports presentation, and the benchmark prints
`presentation: unavailable; submission only`. The presentation measurements
above apply only when a window layer reports feedback. Missing feedback or
refresh data then fails the run instead of producing an unverified frame count.

## Workload and attribution

A built-in helper (`--echo`) runs inside the terminal in raw mode, disables
line-discipline echo, and signals readiness before injection starts. It writes
an ignored OSC containing a monotonic response sequence number followed by
one printable `a`. Thus this measures a small userspace echo workload, including
helper scheduling; it is not directly comparable with the previous `cat`
benchmark's line-discipline echo workload or an interactive shell prompt.

The probe recognizes the complete sequence marker and its following printable character
after VT parsing, including markers split across PTY reads, on both direct-read and ring-buffer paths.
Only a render following that response can publish the sample. Each Wayland
feedback object carries the corresponding sample number. Unrelated redraws,
old responses, and delayed feedback from earlier samples cannot complete the
current request. Release/acquire publication makes each result immutable until
the next request. One request is outstanding at a time.

The terminal is 800×600 with cursor blinking disabled. Characters accumulate,
so sufficiently long runs include line wraps and eventually scrolling. Each
sample follows a deterministic pseudorandom 30–50 ms pause, reducing fixed
phase alignment with display refresh. This is an idle typing workload, not a
continuous flood benchmark. The reader sleeps while waiting; observation delay
does not enter the recorded event timestamps. Probe clocks, OSC parsing, and
helper scheduling add overhead, which is included in the result.

Missing replies or presentation feedback time out after one second. Discarded
frames, nonmonotonic/missing timestamps, focus loss, and failed injection stop
the run with nonzero exit status. Partial valid statistics remain visible;
never treat a partial run as a successful comparison. Stopping on the first
failure avoids losing sequence alignment or silently omitting slow samples.

## CSV and comparisons

CSV includes warmup rows, sequence number, status, seven absolute nanosecond
timestamps, and Wayland presentation flags. The presentation timestamp column
uses feedback time for vsync and local event receipt time for async mode. Zero
timestamps denote unavailable stages in failed rows, or unsupported presentation. Subtract adjacent
columns to analyze individual stage delays. Presentation flags are the
protocol bitmask: 1 = vsync, 2 = hardware clock, 4 = hardware completion,
8 = zero-copy. These describe the compositor's reported timing quality.

For useful before/after comparisons, retain the CSV and record the git revision,
build options, CPU, compositor/version, refresh rate, display configuration,
power policy, background load, and active keyboard layout. Repeat runs under
matched conditions; examine distributions and outliers rather than just means.
Use larger sample counts for tail estimates, remembering that long runs change
the amount of terminal content and eventually scroll. The benchmark does not
capture this environment metadata automatically.

## Validation

`meson test -C build latency-probe-test --print-errorlogs` exercises matching
responses, unrelated frames, fragmented markers, stale markers, and immutable
completed samples. A live run is still required to validate the compositor
path.

A previous synchronized Vulkan run on KWin confirmed fullscreen, but it did
not meet the frame limit. It stopped at sample 37 after 16 valid samples. The
failed sample took 19.643 ms, or 3.54 frames at 180 Hz.

The latest asynchronous run completed 300 samples in fullscreen immediate mode.
It measured at most 2.489 ms, or 0.45 frames at 180 Hz, using feedback receipt
as a conservative upper bound. The benchmark reported no zero-copy
presentations, even after Vulkan surfaces declared an opaque region. These
results describe one desktop setup. They are not a general performance claim.

The two runs above came before the yasl port. The first run after the port
used the Wayland injector on a private headless Sway with the pixman renderer,
on an AMD Ryzen 7 9700X. It completed 100 samples after 20 warmup samples.
Injection → submission measured p50 0.356 ms, p99 0.469 ms, and max 0.487 ms.
Presentation was unavailable.
