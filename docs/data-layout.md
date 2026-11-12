# Data layout

## Inspect a type

To inspect structure layout, compile debug binaries and run `pahole`:

```sh
meson configure build -Ddebug=true
meson compile -C build
pahole -C cell build/bin/wsap_term
pahole -C pty_ring build/bin/wsap_term
```

## Cells

`struct cell` is private to the terminal core in `subprojects/term/src/cell.h`. The public snapshot type is `struct wsap_vtcore_cell`.

| Field | Type | Byte offset |
| --- | --- | ---: |
| `c` | `uint32_t` | 0 |
| `fg` | `uint32_t` | 4 |
| `bg` | `uint32_t` | 8 |
| `flags` | `uint16_t` | 12 |
| `pad` | `uint16_t` | 14 |

Each cell occupies 16 bytes. One 512-bit SIMD register stores four cells. Any change to this layout requires corresponding updates in SIMD kernels.

Color fields store ARGB values or palette indices directly without color conversion. The renderer resolves palette indices during frame painting. A palette change triggers a full canvas repaint.

## PTY ring

`src/pty/ring.h` defines `struct pty_ring`.
The worker thread reads PTY data into `buf` and parses the bytes directly. The ring stores no duplicate copy of terminal output.
The worker thread sets `wake_pending` before writing to `event_wr`. The event loop thread reads `event_fd` and clears `wake_pending`.
The atomic flags `end_requested` and `end_published` synchronize the stream termination state between threads.
These atomic variables do not require separate cache line alignment because event signaling occurs infrequently relative to PTY read parsing.

## Terminal storage

| Storage | Role |
| --- | --- |
| `scr`, `alt_scr` | Screen row rings, cell slabs, and text slabs. |
| `spare`, `resize` | Reusable resize storage. |
| `struct srow` | Row data, identity, version, length, kind, and flags. |
| `struct hist` | Logical lines stored in 16-byte parts on 64 KiB pages. |

Resizing the primary screen reflows logical lines. Resizing the alternate screen preserves the saved primary screen.
All storage structures require valid behavior when zero-initialized.

Benchmark storage modifications with `term-write-bench`.
See [development](development.md#benchmarks) for benchmark commands.
