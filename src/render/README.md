# Renderer

This directory turns a terminal snapshot and a UI scene into canvas pixels. The CPU path provides the reference implementation. The GPU path renders identical cell output.

Each build includes one GPU backend: Metal on macOS, Vulkan on Linux, or `gpu/none.c` when building without GPU support.

## Modules

| Path | Job |
|---|---|
| `render.h` | Public API: `renderer_create()`, `render_draw()`, `render_init()`. |
| `scene_paint.h` | Public API: `ui_scene_paint()`, `ui_scene_paint_clipped()`. |
| `term_frame.h` | Public API: `struct terminal_frame` and its accessors. |
| `render.c` | Coordinates frame execution by invoking renderer components in sequence. |
| `surface.c/.h` | Manages canvas state inside `framebuffer_history.data`. |
| `plan.c/.h` | Plans row operations (keep, move, paint), UI regions, and damage bounds. |
| `frame/term_frame.c` | Adapts terminal snapshots for rendering. |
| `frame/walk.c/.h` | Traverses rows, applies cell policy, segments text, shapes glyphs, and assigns glyph IDs. |
| `cpu/isa.c/.h` | Detects CPU instruction set extensions and defines target macros. |
| `cpu/kernels.c/.h` | Implements pixel rendering kernels for each ISA level. |
| `cpu/raster.c/.h` | Implements the two-pass row rasterizer. |
| `cpu/cell_cache.c/.h` | Manages the composited cell cache and L0 lookup tables. |
| `cpu/pair_lut.c/.h` | Manages the color pair (foreground, background) blend cache. |
| `cpu/decor.c/.h` | Generates decoration and cursor masks. |
| `cpu/pool.c/.h` | Manages the renderer worker thread pool. |
| `cpu/scene.c` | Paints UI scene elements for `scene_paint.h`. |
| `gpu/gpu.h`, `gpu/none.c` | Defines the GPU interface and provides the no-GPU stub. |
| `gpu/build.c/.h` | Builds instance lists, draw commands, texture atlases, and UI image slots for GPU backends. |
| `gpu/metal/` | Implements the Metal backend (`render.m`, `shaders.metal`) using frame data from `gpu/build.c`. |
| `gpu/vulkan/` | Implements the Vulkan backend (`device.c`, `swapchain.c`, `frame.c`). |

## Ownership

- The application owns a single renderer instance. The application invokes `render_draw()`.
- The renderer owns its worker pool, cell cache, decoration cache, per-thread `raster_worker` instances, scratch surface, planner arrays, and GPU backend.
- The window owns each `framebuffer_history`. The renderer owns the `struct render_surface` referenced by `history.data`.
- Renders without frame history use the scratch surface without dynamic allocation.
- The font subsystem owns all glyphs. Glyph pointers remain valid only during a walk callback. Modules do not retain glyph pointers.
- The terminal subsystem owns the terminal snapshot. `terminal_frame` holds borrowed pointers into that snapshot.

## Frame order in `render_draw`

1. Font resolution: `glyph_cache_refresh()` refreshes the grid font and UI font in serial mode before any glyph fetch.
2. Surface preparation: The renderer reads the surface and builds its key, row keys, and UI ink (`plan_ui_ink`). A frame is marked invalid when the surface is invalid, `fb->fresh` is true, `frame->repaint` is true, `WSAP_RENDER_FULL=1` is active, or the surface key changes.
3. Frame planning: `plan_frame()` calculates row actions and damage bounds.
4. GPU rendering attempt: When a drawable target exists and `cpu_only` is false, `render_gpu_draw()` runs. A return status of PRESENTED or DROPPED completes the frame immediately without CPU canvas allocation.
5. CPU canvas moves: When the GPU does not handle the frame, `fb->acquire_pixels()` acquires the CPU canvas. Memory moves shift reused row blocks in plan order with `memmove`. When measured paint costs show that thread rendering is faster, rows repaint instead of moving. Output pixels and damage rectangles remain identical.
6. Row rasterization: The engine prepares masks (`decor_get`) and cell caches (`cell_cache_begin`). For terminals wider than previous frames, `pool_each()` expands walk scratch buffers. The engine enables parallel mode with `glyph_cache_set_parallel(true)`. `pool_run()` executes raster jobs across paint rows and overlay windows. After worker threads join, `glyph_cache_set_parallel(false)` disables parallel mode, and `raster_workers_balance()` balances scratch capacity. The renderer fills bottom margins on invalid frames.
7. Cache publication: `cell_cache_publish()` publishes updated cell cache entries.
8. UI rendering: The renderer clears regions outside the grid with the base color and renders UI elements with `ui_scene_paint_clipped()`.
9. Resource flush: `glyph_cache_flush_frame()` releases retired cache entries and deferred buffers.
10. Surface commit: `render_surface_commit()` commits updated keys, ink state, and row reach.
11. CPU presentation: When a drawable target exists, `render_gpu_present()` displays CPU pixels.

## Thread rules

- The main thread executes `render_draw()`, the planner, surface operations, row moves, `cell_cache_begin()`, `cell_cache_publish()`, `decor_get()`, UI painting, and GPU operations.
- The parallel phase runs only during step 6 (row rasterization). This phase executes row walks, rasterization, `cell_cache_find()`, `cell_cache_claim()`, color pair LUT lookups, and glyph fetch and shape operations.
- The parallel phase always sets `glyph_cache_set_parallel(true)`, even when using zero worker threads. Serial and multi-threaded configurations execute identical font code paths.
- During the parallel phase, threads treat the following structures as read-only: `raster_frame`, `terminal_frame`, terminal snapshot, cell cache index, published tiles, and decoration sets.
- Each worker thread writes only to its assigned `raster_worker`, its claimed cache slots, and the canvas pixels of its row windows. The free list cursor provides the only shared atomic variable.
- Worker threads follow the font thread protocol defined in `cpu/pool.h`.
- Dynamic timing determines row distribution across worker threads. Pre-existing frames do not trigger buffer growth during the parallel phase. Before the parallel phase, `render_walk_thread_reserve()` allocates walk and font scratch memory for the full terminal width across all threads. After worker threads join, `raster_workers_balance()` expands scratch and spill buffers to match the largest worker.

## Pixel rules

- Cache state, worker thread count, admission status, and CPU ISA levels never affect output pixels. All pixel calculations use integer mathematics.
- The scene painter selects its ISA level once per process. It does not inherit the ISA level from a renderer instance. The `WSAP_RENDER_ISA` environment variable controls this selection. Different ISA levels produce bit-identical pixel output.
- Incremental frame rendering produces pixels identical to a full repaint.
- Grid rendering is opaque. Base backgrounds and UI overlays support translucency.
- Glyph pixel spills never cross into grid margins.

## Link contract of the scene painter

The compilation unit for `cpu/scene.c` permits references only to `glyph_cache_fetch()`, `glyph_cache_refresh()`, sRGB color tables, standard C library symbols, `paint/` modules, and the render files `cpu/isa.c`, `cpu/kernels.c`, and `cpu/pair_lut.c`. Test stubs in `tests/ui/stubs/scene_paint_deps.c` define these font and sRGB symbols. Any additional dependency breaks the test link.

## Test strategy

Tests verify observable public behavior: output pixels, damage bounds from `render_draw()`, `ui_scene_paint()`, and planning outputs from `plan.h`.

- Incremental rendering parity: Tests execute seeded random sequences of terminal operations: text writes, SGR styles, underline variants, scrolling, scroll regions, selections, cursor styles, DECSCNM inverse video, palette modifications, resizes, font changes, tab switching, UI overlays, and scrollbar movement. After each operation, the test verifies that incremental surface pixels match a full repaint from a reference renderer configured with `threads = RENDER_NONE` and `cache_bytes = RENDER_NONE`. Reported damage rectangles cover all modified pixels.
- Cross-ISA consistency: Test suites verify that SCALAR and supported SIMD ISA levels generate bit-identical pixel results.
- Cache independence: Output pixels remain identical across cold caches, warm caches, a 64 KB cache budget, and zero or three worker threads.
- Damage precision: Idle frames generate no damage rectangles. Single-row text updates damage only the modified row. Scroll operations damage only exposed row bands.
- Planner correctness: Planner tests supply row key arrays and span arrays to verify resulting operations, row moves, window rects, UI regions, and damage bounds.
- Concurrency and memory safety: AddressSanitizer (ASan) and ThreadSanitizer (TSan) builds of `render-test` validate thread pool execution and cache access.
