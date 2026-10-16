# Data layout

## Inspect a type

Build with debug symbols before using `pahole`:

```sh
meson configure build -Ddebug=true
meson compile -C build
pahole -C cell build/bin/wsap_term
pahole -C pty_ring build/bin/wsap_term
```

## Cells

`struct cell` is private to the terminal core.
Its definition is in `subprojects/term/src/cell.h`.
The public snapshot type is `struct wsap_vtcore_cell`.

| Field | Type | Byte offset |
| --- | --- | ---: |
| `c` | `uint32_t` | 0 |
| `fg` | `uint32_t` | 4 |
| `bg` | `uint32_t` | 8 |
| `flags` | `uint16_t` | 12 |
| `pad` | `uint16_t` | 14 |

A cell occupies 16 bytes.
An AVX-512 register holds four cells.
Changes to the layout must also update the SIMD kernels.

Colors store ARGB values or palette slots.
Writers preserve these values without color conversion.
The renderer resolves palette slots when it paints.
A palette change causes a full repaint.

## PTY ring

`src/pty/ring.h` defines `struct pty_ring`.
The reader thread writes `head`.
The loop thread writes `tail`.
Each counter starts on a separate 64-byte cache line.
`tail_src` also has its own cache line.
Keep these alignment boundaries to prevent false sharing.

## Terminal storage

| Storage | Role |
| --- | --- |
| `scr`, `alt_scr` | Screen row rings, cell slabs, and text slabs. |
| `spare`, `resize` | Reusable resize storage. |
| `struct srow` | Row data, identity, version, length, kind, and flags. |
| `struct hist` | Logical lines stored in 16-byte parts on 64 KiB pages. |

Normal-screen resize reflows logical lines.
Alternate-screen resize preserves the saved normal screen.
All-zero values must remain valid.

Measure storage changes with `term-write-bench`.
See [development](development.md#benchmarks) for commands.
