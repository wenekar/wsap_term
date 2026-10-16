# macOS Metal backend

The renderer uses native Metal commands. Cocoa owns the window and input.

## Event loop

The main loop waits in the Cocoa run loop. One kqueue holds the fd
interest. One run-loop source watches the kqueue. One run-loop timer
tracks the next loop deadline. These objects live for the whole run,
so a wait does not create or release run-loop objects.

The source, the timer, and the display link are in the common modes.
Live resize and menu tracking run modal run loops inside `sendEvent:`.
Inside such a loop, the fds, the timers, and the display link run loop
iterations that do not block. So output, cursor blink, and frames
continue during a resize.

The app pumps Cocoa events once per iteration, in the prepare hook.

## Frame pacing

Each window has a `CADisplayLink` on macOS 14 and later. The link sets
the refresh period from the display, so ProMotion displays run at
120 Hz. A frame starts when a vsync passed since the last frame. The
first frame after an idle period starts at once. The link pauses after
eight idle frames and while the window is occluded. An occluded window
is suspended and draws no frames. Earlier macOS versions draw without
pacing.

In a live resize, the frame draws at once in the resize callback. It
presents in the same Core Animation transaction as the new window size.
The layer is opaque, uses sRGB, and keeps its content at the top left.

## Renderer

Each quad uses one instance record. The shader builds its six vertices.
Three buffer sets hold frames in flight. A completion callback releases
each set for reuse. All windows share one shader library and one set of
pipelines.

Terminal text and UI text use separate atlases. The GPU owns each atlas
texture. New glyph pixels wait in a CPU list. The next submit copies
them to a staging buffer and blits each cell. The atlas has no CPU copy.

Each atlas cell records the frame that last drew it. Every frame draws
all cached terminal rows. A cell that missed this frame and the last
one is free for a new glyph. Metal orders that write after the GPU
reads of earlier frames. A full atlas grows. At the maximum size it
clears and rescans every row.

Alpha packing uses NEON, AVX2, or AVX-512. Scalar code is the fallback.
Unchanged terminal rows keep their draw records.

### ASCII atlas

Each atlas has a second texture in `R8Unorm` for plain ASCII. It
holds '!' through '~' in regular, bold, italic, and bold italic: 376
cells with the cell size of the main atlas. The lookup table has 512
entries of 8 bytes, indexed by `cp | style << 7`. One unsigned compare
checks the code point and the style together. The mode of the entry is
its state, so a hit is one load. A cell gets its pixels at the first
lookup after a reset. Later lookups do not call the glyph cache. These
cells are never evicted, so ASCII does not use space in the main atlas.

A main atlas lookup calls the glyph cache only on a miss. A hit reads
only the probed slots. The eviction victim is found on a miss, in a
cold function. The fill paths are cold and not inlined.

Cached draws keep the texel origin of their atlas cell. The frame
loop opens one command for the whole terminal and writes the quads
directly into the vertex array, with no division and no command check
per quad. An ASCII quad marks no main atlas cell. A store to a local
value replaces that branch, so mixed text does not mispredict. Any
atlas reset changes the atlas generation, and the next frame rescans
every row.

A glyph that has color or no ink stays in the main atlas. Shaped runs
also stay in the main atlas, because their glyphs depend on the run.
An upload copies one byte per pixel for an ASCII cell, not four.

A glyph command binds the main atlas at texture index 0 and the ASCII
atlas at index 1. Solid quads bind no texture. So backgrounds, ASCII,
other glyphs, and decorations share one draw. Atlas quads use texel
coordinates, so the texture size does not enter the vertex data.

A throwaway driver measured the changes on an Apple M1 Pro on
2026-10-02. It drew a 200 × 60 grid of random printable ASCII, with
one cell in eight bold. In the mixed grid, one cell in four is from
U+00C0–U+00FF or U+03B1–U+03C9. The font was the default instance
(8 × 17 cells), and `fast_stream` was set. Each case ran 3000 frames
after 100 warm-up frames. The values are the time in
`metal_render_terminal`, as the mean of five interleaved runs. "Main
atlas only" is the renderer before the ASCII atlas. "First ASCII
atlas" is the ASCII atlas with a split state array, the glyph cache
call on every main atlas lookup, and one command check per quad.

| Case | Main atlas only | First ASCII atlas | Now |
| --- | ---: | ---: | ---: |
| ASCII, every row changes | 250 µs | 184 µs | 113 µs |
| ASCII, no row changes | 120 µs | 115 µs | 48 µs |
| Mixed, every row changes | 274 µs | 252 µs | 152 µs |
| Mixed, no row changes | 120 µs | 134 µs | 60 µs |

The first ASCII atlas made the mixed frame without changes slower.
Each quad chose its texture with a branch, and that branch followed
the random mix of ASCII and other text. The single command removed it.

A frame without changes costs about 4 ns per cell now. Each quad
instance is 64 bytes, so that frame writes 768 KB of vertices, and
`metal_render_submit` copies them again. This is probably the next
limit, but no profile measured it.

A second driver fed real escape sequences to a `struct term`. The
sequences had plain, bold, italic, faint, and decorated text, colors,
CJK, emoji, box drawing, and ligatures. The driver drew three frames
through `renderer_draw_terminal_frame`: a shaped frame, the same
snapshot again, and new output with `fast_stream` set. The renderer
without the ASCII atlas and the renderer with it gave bit-identical
pixels in all three frames. The inspector screenshot cannot show this,
because it uses the CPU path.

The CPU frame is the fallback and the source of inspector screenshots.
It uploads to one texture that lives with the renderer.

Menus use the inline UI path. Native popup drawing is not implemented.
Window drag transport is not implemented.

## Input

Trackpad scroll deltas are points. Ten points make one line, as with
Wayland touchpad pixels. Vertical scroll reports button 64 or 65.
Horizontal scroll reports button 66 or 67.

Key events take `codepoint` and `shifted` from the current keyboard
layout, and `base` from the US ANSI key position. Modifier keys report
press and release. The numeric-pad flag does not set Num Lock. A key
that the input method composes does not reach the terminal. Key-up
events with Command held reach the view.

## Checks

The checks ran on an Apple M1 Pro with macOS 27.0 on 2026-10-02.

- All 64 tests passed.
- The Metal, Cocoa, and loop tests passed under AddressSanitizer.
- Metal API and shader validation passed the GPU pixel tests.
- The inspector reported no findings for the terminal.
- An inspector screenshot showed the terminal.
- The refresh estimate was 8.33 ms on the 120 Hz display.

The window was occluded during the `cat /dev/random` run, so that run
measured no frame rate. No comparison with the old backend was
measured.

Run the GPU checks outside a sandbox that blocks the Metal device:

```sh
meson test -C build metal-render-test darwin-native-app-test loop-test --print-errorlogs
env MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/metal-render-test
meson compile -C build term-live-bench
```

`--stats-ms 1000` prints the frame rate and the refresh estimate.
