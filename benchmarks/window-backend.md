# Linux Window Backend Benchmark

This report measures the frame loop and event handling behavior under Wayland.
Redraw and configure events are coalesced before polling, avoiding unnecessary
wakeups and rendering cycles.

## Measurements

Built with `-O3 -march=native -flto` and run under an isolated headless Sway
compositor with software rendering. Each run opened an interactive shell for
four seconds with cursor blinking enabled:

| Four-second run | Baseline backend | Refactored backend | Change |
| --- | ---: | ---: | ---: |
| Frames drawn | 252 | 13 | -95% |
| Poll calls | 514 | 34 | -93% |
| Frames added between second 1 and 3 | 124 | 4 | -97% |

This represents a reduction of ~95% in frames drawn and ~93% in poll calls during
idle workloads. Buffer preparation took 1.9 µs on average and 10.3 µs at maximum.

## Verification

```sh
meson test -C build window-idle-test window-resize-test window-state-test \
  window-multiwindow-test window-conformance-test
meson test -C build loop-test
```

There is no single `window-test` target; each window regression scenario is
its own named test in `tests/meson.build`.
