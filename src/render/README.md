# Renderer

This directory turns a terminal snapshot and a UI scene into canvas pixels.
The CPU path is the reference. The GPU path draws the same cells.

The design is the render rewrite plan. Milestone 4 is the CPU renderer
behind the headers listed here. Every system builds `gpu/none.c` in this
milestone. Milestone 6 adds the Metal backend.

## Modules

| Path | Job |
|---|---|
| `render.h` | Public API: `renderer_create`, `render_draw`, `render_init`. |
| `scene_paint.h` | Public API: `ui_scene_paint`, `ui_scene_paint_clipped`. |
| `term_frame.h` | Public API: `struct terminal_frame` and its accessors. |
| `render.c` | Runs one frame. It is the only file that calls the others in order. |
| `surface.c/.h` | The state of one canvas, in `framebuffer_history.data`. |
| `plan.c/.h` | The pure planner: keep, move, or paint each row; UI regions; damage. |
| `frame/term_frame.c` | The snapshot adapter. |
| `frame/walk.c/.h` | The row walk: all cell policy, segments, shaping, glyph ids. |
| `cpu/isa.c/.h` | The ISA level and the target macros. |
| `cpu/kernels.c/.h` | The pixel kernels, one variant per level. |
| `cpu/raster.c/.h` | The two-pass row raster. |
| `cpu/cell_cache.c/.h` | The composited cell cache and its L0 tables. |
| `cpu/pair_lut.c/.h` | The two-hit (fg, bg) blend table cache. |
| `cpu/decor.c/.h` | Decoration and cursor masks. |
| `cpu/pool.c/.h` | The worker pool of one renderer. |
| `cpu/scene.c` | The UI scene painter behind `scene_paint.h`. |
| `gpu/gpu.h`, `gpu/none.c` | The GPU interface, and the build without a GPU. |
| `gpu/metal/` | The Metal backend. It does not exist yet. It arrives in milestone 6. |

## Ownership

- The app owns one renderer. Only the app calls `render_draw`.
- The renderer owns its pool, its cell cache, its decor cache, one
  `raster_worker` for each thread, a scratch surface, the planner arrays,
  and its GPU.
- The window owns each `framebuffer_history`. The renderer owns what
  `history.data` points to: one `struct render_surface`.
- A draw without history uses the scratch surface. It allocates nothing.
- The font owns every glyph. A glyph pointer is valid only inside one walk
  callback. No module keeps one.
- The terminal owns the snapshot. `terminal_frame` only points into it.

## Frame order in `render_draw`

1. Resolve the fonts. Call `glyph_cache_refresh` on the grid font and the
   UI font, in serial mode, before any fetch.
2. Get the surface. Build its key, the new row keys, and the new UI ink
   (`plan_ui_ink`). The frame is invalid when the surface is not valid,
   `fb->fresh` is set, `frame->repaint` is set, `WSAP_RENDER_FULL=1` is
   set, or the key changed.
3. Plan (`plan_frame`).
4. With a drawable and without `cpu_only`, try the GPU (`render_gpu_draw`).
   PRESENTED or DROPPED ends the frame here.
5. Moves: memmove each move block, in plan order. When the measured paint
   cost says that the pool paints the moved rows faster, they paint
   instead. The pixels and the damage are the same.
6. Raster rows: `decor_get`, `cell_cache_begin`, then the walk scratch
   of every thread (`pool_each`, only for a terminal wider than before),
   then `glyph_cache_set_parallel(true)`, then `pool_run` over the paint
   rows and the overlay windows, then the join, then
   `glyph_cache_set_parallel(false)`, then `raster_workers_balance`. Fill
   the bottom margin for an invalid frame and for its windows.
7. Publish the cache (`cell_cache_publish`).
8. UI regions: fill `base` in the part of each region outside the grid,
   then `ui_scene_paint_clipped` on the region.
9. Flush: `glyph_cache_flush_frame`.
10. Commit: `render_surface_commit` with the new keys, ink, and reach.
11. With a drawable, present the CPU pixels (`render_gpu_present`).

## Thread rules

- `render_draw`, the planner, the surface, the moves, `cell_cache_begin`,
  `cell_cache_publish`, `decor_get`, the UI pass, and the GPU run on the main
  thread.
- The parallel phase is step 6 only. Inside it run the walk, the raster,
  `cell_cache_find`, `cell_cache_claim`, the pair LUTs, and the glyph fetch
  and shape calls.
- The phase always runs with `glyph_cache_set_parallel(true)`, also with 0
  workers. So 0 and N workers take the same font path.
- During the phase these are read only: the `raster_frame`, the
  `terminal_frame`, the snapshot, the cell cache index and its published
  tiles, and the decor set.
- Each thread writes only its own `raster_worker`, the slots that it
  claimed, and the canvas pixels of its row windows. The free list cursor is
  the only shared atomic.
- Pool workers follow the font protocol in `cpu/pool.h`.
- Timing decides which thread paints which row. So no per-thread buffer
  grows in the phase of a frame that the renderer painted before. Before
  the phase, every thread gets walk and font segment scratch for the
  terminal width (`render_walk_thread_reserve`). After the join, every
  raster worker gets the spill and scratch room of the largest one
  (`raster_workers_balance`).

## Pixel rules

- Pixels never depend on the cache state, the thread count, admission, or
  the ISA level. All pixel math is integer.
- The scene painter picks its ISA level once for the process. It does not
  use the level of a renderer. The same holds for `WSAP_RENDER_ISA`. This
  never changes pixels, because all levels give the same pixels.
- An incremental frame gives the same pixels as a fresh paint of that frame.
- The grid is opaque. The base and the UI can be translucent.
- Spills never reach the grid margin.

## Link contract of the scene painter

`cpu/scene.c` and the files that it pulls in may reference only
`glyph_cache_fetch`, `glyph_cache_refresh`, and the sRGB tables, plus libc,
`paint/` code, and the render files `cpu/isa.c`, `cpu/kernels.c`, and
`cpu/pair_lut.c`. `tests/ui/stubs/scene_paint_deps.c` stubs the font and
sRGB symbols, so a wider reference breaks those test links.

## Test strategy

Tests go through public behavior: `render_draw` pixels and damage,
`ui_scene_paint`, and, for the pure planner, `plan.h`.

- Incremental equals fresh. A seeded random walk over terminal operations:
  writes, SGR with every underline style, scroll, scroll regions, selection,
  cursor styles, DECSCNM, palette, resize, font change, tab switch, and UI
  overlays that appear, move, and vanish, plus the scrollbar during scroll.
  After each step, the persistent surface equals a fresh surface that a
  second renderer draws with `threads = RENDER_NONE` and
  `cache_bytes = RENDER_NONE`. The damage covers every changed pixel.
- Cross-ISA parity. A corpus gives the same pixels at SCALAR and at each
  level that `renderer_isa` confirms.
- Cache independence. A 64 KB budget, 0 or 3 threads, and a cold or a warm
  cache give the same pixels.
- Damage. An idle frame gives no damage. Typing damages one row. A scroll
  damages the right bands.
- Planner. Key and span arrays in, ops, moves, windows, regions, and damage
  out.
- ASan and TSan builds of `render-test` cover the pool and the cache.
