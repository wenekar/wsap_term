# Font API

The font API is in `src/font/font.h` and `src/font/glyph_cache.h`.

## Vocabulary

| Word | Meaning |
|---|---|
| lookup | A cache read. It returns a hit or NULL. It never builds. Only internal code has one (`cache_slot_lookup()`). The public API has no lookup function. `src/font/glyph/layout.h` also uses "lookup" for an OpenType lookup. That is a different sense. |
| fetch | A cache read that builds on a miss. The result is borrowed. `glyph_cache_fetch()` and `glyph_cache_fetch_tile()` are fetches. |
| acquire | Returns an owned reference. Pair each acquire with one `font_instance_unref()`. `font_instance_ref()` adds one more reference. |
| render | Always rasterizes. It reads and writes no cache. `font_instance_render_glyph()` renders into a `struct glyph` that the caller owns. |
| rebuild | `glyph_cache_rebuild()`. It reopens the face when the font generation changed. Then it drops every cached glyph, tile, and run of the instance and fills ASCII and Latin-1 again. |
| thread_attach, thread_detach | Per-thread backend state. `font_thread_attach()` loads it ahead of use. `font_thread_detach()` frees it. `glyph_cache_thread_detach()` frees the per-thread buffers of the glyph cache. |
| `font_handle_t` | An opaque face handle of one size. The backend makes and owns it (`src/font/backend.h`). Each instance holds one in `face`. The handle can be NULL. |
| slot | A storage place. The word has three senses. A cache slot (`struct cache_slot`) holds one glyph or tile. It publishes the buffer with one atomic pointer, and the buffer header holds the key. A face slot (`struct ft_face_slot`, FreeType) holds one font file and its four variants. An array of variants is indexed by `enum font_variant`, not by a slot. |
| variant | One of four faces of a family: plain, bold, italic, bold italic. `enum font_variant` (`src/font/font.h`) names them. Its value is `style & FONT_STYLE_VARIANT_MASK`: plain 0, bold 1, italic 2, bold italic 3. `font_variant_of()` gives it. Every array of variants uses this value as the index: `ft_face_slot.styles[]`, `hb[]`, `layout_gids[]`, `font_instance.layouts[]`, and the CoreText `hb_face[]` and `hb_font[]`. |
| style | A set of `enum font_style` bits: bold, italic, emoji, text, and wide. It is not a set of raw cell attributes. `render_cell_style()` makes a style from a cell. |
| `_px` | A length in device pixels. |
| `_pt` | A size in points. `FONT_PT_TO_PX()` converts points to pixels at scale 1. |
| `_120` | A display scale in 1/120 units. 120 is scale 1. A zero `scale_120` means 120. |
| `built_*` | A snapshot that the last rebuild stored in the instance. `built_gen` is the font generation. `built_fg` and `built_bg` are the tile colors. A stale check compares a snapshot with the current value. |

## Ownership and lifetime

| Call | Result | Lifetime |
|---|---|---|
| `font_instance_acquire()` | Owned reference, or NULL | Until you call `font_instance_unref()`. |
| `font_instance_ref()` | The same pointer, one more reference | Until the matching `font_instance_unref()`. |
| `font_instance_unref()` | Nothing | The last unref frees the instance, its caches, and its face. |
| `font_instance_default()` | Borrowed, can be NULL | From `font_system_init()` to `font_system_shutdown()`. It is NULL before init and when init failed. |
| `font_instance_metrics()` | Borrowed | While the instance lives. A rebuild can change the values in place. |
| `glyph_cache_fetch()` | Borrowed glyph | See [Glyph and tile pointers](#glyph-and-tile-pointers). |
| `glyph_cache_fetch_tile()` | Borrowed pixels, or NULL | See [Glyph and tile pointers](#glyph-and-tile-pointers). |
| `glyph_cache_shape_segment()` | A `struct glyph_span` passed to `emit` | Only during that `emit` call. |
| `font_instance_render_glyph()` | Fills a caller-owned `struct glyph` | The caller owns the planes. Free them with `glyph_release()`. A later render into the same glyph reuses its planes. |
| `font_list_families()` | Owned array of owned strings | Free each string and the array. |

`font_instance_acquire()` interns instances. The same size and scale give the
same instance while one reference is alive. A hit can rebuild a stale
instance, so the hit invalidates earlier glyph pointers of that instance.

`font_instance_default()` can also rebuild a stale instance. Do not call it
while you hold glyph pointers of the default instance.

### Glyph and tile pointers

A fetch result has one of these sources:

- Thread scratch. An allocation failure gives scratch.
  In `glyph_cache_fetch()`, a code point above U+10FFFF also gives scratch.
  For that code point, `glyph_cache_fetch_tile()` returns NULL. Scratch is
  valid until the next `glyph_cache_fetch()` or `glyph_cache_fetch_tile()` on
  the same thread.
  `glyph_cache_fetch_tile()` calls `glyph_cache_fetch()`, so a tile fetch also
  ends a scratch glyph.
- A cache slot in serial mode. A refill of the slot with a different key
  writes the new glyph in place. The old pointer then shows the new glyph.
- A cache slot in parallel mode. A refill makes a new buffer. A published
  buffer does not change. Two threads can refill one slot at the same time.
  Each thread gets its own buffer. The thread that replaced a buffer in the
  slot keeps that buffer on its deferred list. The old buffer
  stays valid until that thread calls `glyph_cache_flush_deferred()` or
  `glyph_cache_flush_frame()`. The renderer makes these calls only between
  frames.
- A primed ASCII tile. A rebuild frees it at once. It never waits on a
  deferred list.

Every rebuild retires every glyph, tile, and run of the instance. In serial
mode the rebuild frees them at once. A rebuild can come from
`glyph_cache_rebuild()`, `glyph_cache_begin_frame()`, `glyph_cache_refresh()`,
`font_instance_acquire()`, or `font_instance_default()`. A fetch or shape call
never rebuilds. Thus a safe rule is: use a fetch result before the next call
to one of these five functions.

`struct glyph_span` cells come from a run cache entry or from thread scratch
(`priv_run_cells`). In parallel mode another worker can evict the run entry.
The evicted entry waits for `glyph_cache_flush_frame()`, so the span stays
valid to the end of `emit`.

## Stale checks

A font change bumps `font_generation()`. A rebuild happens only at frame
start or between frames. The calls below check for staleness:

| Call | Checks | Action when stale |
|---|---|---|
| `glyph_cache_begin_frame()` | Sets the tile colors. Then compares `built_gen`, `built_fg`, and `built_bg` with the current values. | Rebuilds. Serial mode only. |
| `glyph_cache_refresh()` | `built_gen` against the generation | Rebuilds. Serial mode only. |
| `font_instance_acquire()` (hit) | The same check as `glyph_cache_begin_frame()`, against the current tile colors | Rebuilds. Serial mode only. |
| `font_instance_default()` | The same check as `font_instance_acquire()` | Rebuilds. Serial mode only. |
| `glyph_cache_rebuild()` | `built_gen` against the generation | Reopens the face when stale. It always drops and fills the caches. |
| `glyph_cache_fetch()`, `glyph_cache_fetch_tile()`, `glyph_cache_shape_segment()` | Nothing | Never rebuild. |
| `font_instance_render_glyph()` | Nothing | Renders with the face and metrics that the instance has now. |

A fetch after a generation bump and before the next frame-start rebuild can
return a glyph of the old face. Nothing is freed, so the memory stays valid.
The next frame shows the new face.

In parallel mode, refresh, begin_frame, acquire on a hit, and default do not
rebuild. Do not call `glyph_cache_rebuild()` in parallel mode. Paint workers
read the cache, so a rebuild there would free memory under them. The
renderer calls `glyph_cache_begin_frame()` before it turns on parallel mode.

The tile colors are process-global. One frame has one color pair. Only
`glyph_cache_begin_frame()` keeps the tiles current with the colors. A
`glyph_span.tiles` pointer has no color check.

Each frame starts with one of these calls, on the frame thread, before any
glyph fetch:

- The CPU terminal path calls `glyph_cache_begin_frame()` in
  `renderer_paint_frame()` and `render_paint_full_terminal_frame()`.
- `draw_terminal_frame()` calls `glyph_cache_refresh()` before the
  GPU path. The GPU path does not use the default-color tiles.
- `render_ui_scene()` calls `glyph_cache_refresh()` on the UI font
  before the GPU and the CPU UI paint.
- `ui_scene_paint()` calls `glyph_cache_refresh()`. `ui_scene_paint_clipped()`
  does not. Its caller refreshes once per frame.

`font_instance_version()` changes on a new instance and on a generation
change. Renderers compare it with the version of a painted buffer. A
different version forces a full repaint.

## Failed reopen

`glyph_cache_rebuild()` reopens the face of a stale instance. The steps are:

1. It frees the layout analysis of each variant.
2. It closes the old face and sets the face to NULL.
3. It opens a new face for the configured family at the instance size.
4. If the open succeeds, it measures the new face. A failed measure keeps
   the old metrics.
5. If the open fails, the face stays NULL. `font_measure_size()` then opens,
   measures, and closes a temporary face. If that also fails, the old
   metrics stay.
6. It stores the current generation in `built_gen`, also after a failure.

The old face does not survive a failed open. An instance without a face draws
cell-aligned symbols itself. It draws ASCII with the built-in 5x7 font and
other code points as an unknown box. It shapes nothing.

A failure is not retried on each frame, because `built_gen` is current. The
next generation bump retries the reopen at the next frame start. No error
goes to the caller.

`font_instance_acquire()` acts differently for a new instance. A failed open
and a failed `font_measure_size()` make it return NULL. A failed measure of
an open face also makes it return NULL.

## Re-entrancy

`glyph_cache_shape_segment()` keeps scratch in thread-local buffers:
`seg_canon`, `seg_roles`, and `priv_run_cells`. Do not call
`glyph_cache_shape_segment()` from its own `emit` callback. A nested call
overwrites that scratch. A longer nested segment frees it.

`emit` can call `glyph_cache_fetch()` and `glyph_cache_fetch_tile()`. The CPU
renderer does this in `paint_span()`. These calls do not touch the run cache,
`priv_run_cells`, or the segment scratch. The span cells stay valid. A
glyph that a nested fetch renders gets its own planes.

A generation bump during `emit` frees nothing. A nested fetch does not
rebuild, so the span cells and the layout analysis stay valid. The bump can
come from any thread. The CoreText font registration callback is an example.
The next frame start rebuilds the instance.

Do not call a rebuilding function from `emit`. These are
`glyph_cache_rebuild()`, `glyph_cache_begin_frame()`, `glyph_cache_refresh()`,
`font_instance_acquire()`, and `font_instance_default()`.

## Thread affinity

| Call | Thread |
|---|---|
| `font_system_init()`, `font_system_shutdown()` | Main thread |
| `font_instance_acquire()`, `font_instance_ref()`, `font_instance_unref()` | Main thread. The reference count and the instance list have no lock. |
| `font_instance_default()` | Main thread. It can rebuild. |
| `glyph_cache_rebuild()`, `glyph_cache_begin_frame()`, `glyph_cache_refresh()` | Main thread, outside a parallel frame |
| `glyph_cache_set_parallel()`, `glyph_cache_flush_frame()` | Main thread. Call `glyph_cache_flush_frame()` after every worker has joined. |
| `font_set_family()`, `font_set_hint_style()`, `font_set_aa_mode()`, `font_set_configured_size()`, `font_apply_settings()` | Main thread, between frames |
| `glyph_cache_fetch()`, `glyph_cache_fetch_tile()`, `glyph_cache_shape_segment()` | Any thread in parallel mode. One thread at a time in serial mode. |
| `font_instance_render_glyph()`, `font_bind_thread()` | Any thread. They read the instance and write only per-thread state. A rebuild changes the face and the metrics of the instance, so do not call them while a rebuild of that instance can run. |
| `font_thread_attach()`, `font_thread_detach()` | The calling thread only. Each thread attaches and detaches its own state. |
| `glyph_cache_flush_deferred()`, `glyph_cache_thread_detach()` | The calling thread only |
| `font_generation()`, `font_bump_generation()` | Any thread. The counter is atomic. |

A paint worker calls `font_thread_attach()` when it starts and at the start
of each job. It calls `glyph_cache_flush_deferred()` at the start of each job.
Before it exits, it calls `glyph_cache_thread_detach()` and then
`font_thread_detach()`. Each backend call also attaches on demand.

## Counters

| Counter | Scope | Changes when |
|---|---|---|
| `font_generation()` | Global, atomic | A family change, a hinting or antialiasing change, `font_backend_shutdown()`, and a CoreText font registration bump it. `font_bump_generation()` bumps it. |
| `font_instance_version()` | Per instance, computed | It is `id << 32 \| (uint32_t) generation`. |
| `built_gen` | Per instance | A rebuild or a new instance stores the generation. |
| `built_fg`, `built_bg` | Per instance | A rebuild stores the tile colors. |
| `run_stamp` | Per instance | Each run cache store increments it. A hit bumps the stamp of its entry. The oldest stamp in a set is evicted first. |
| `ft_sync_epoch` | FreeType, global, atomic | A family reload, a hinting change, and backend shutdown increment it. |
| `synced_epoch` | FreeType, per thread | The epoch at which this thread loaded its faces. When it differs from `ft_sync_epoch`, the next backend call reloads the faces of the thread. |
| `fit_pass` | FreeType, per thread | Each run fit increments it. Raster cache entries of the current pass are not evicted, so their pointers stay valid while one run is fitted. CoreText uses `ct_use_pass` for the same job. |
| `slot_reuse` | FreeType, per face slot | A release of the face slot increments it. A raster cache entry of an older face in the same slot then does not match. |
| `face_stamp` | FreeType, per thread | Each face load and each face use in a shaped run increment it. The face slot with the oldest stamp is evicted first. |

## Public API

### `src/font/font.h`

- `font_prefers_emoji()`: true when a code point draws as emoji under a style.
- `font_size_px()`: the em size in device pixels of a `struct font_size`.
- `font_size_resolve()`: replaces a zero size with the configured size and a zero scale with 120.
- `font_instance_acquire()`: returns an owned instance for a size and a scale.
- `font_instance_ref()`: adds one reference.
- `font_instance_unref()`: drops one reference. The last one frees the instance.
- `font_instance_default()`: returns the borrowed default instance.
- `font_instance_metrics()`: returns the cell metrics of an instance.
- `font_instance_size_pt()`: returns the size of an instance in points.
- `font_instance_scale_120()`: returns the scale of an instance.
- `font_instance_version()`: returns a value that changes when the glyphs can look different.
- `font_bind_thread()`: makes the calling thread rasterize at the size of an instance.
- `font_instance_render_glyph()`: renders one code point into a caller-owned glyph.
- `glyph_prepare()`: sizes the planes of a glyph for a width.
- `glyph_release()`: frees the planes of a glyph.
- `glyph_color_plane()`, `glyph_subpx_plane()`: allocate the color plane or the subpixel plane.
- `glyph_mask()`, `glyph_mark()`, `glyph_row_inked()`, `glyph_inked()`: read and write the ink bits.
- `glyph_color_at()`, `glyph_subpx_at()`: address one pixel of a plane.
- `font_set_hint_style()`: sets the hinting strength. Auto takes it from the system.
- `font_set_aa_mode()`: sets the antialiasing mode. Auto takes it from the system.
- `font_parse_hint_style()`, `font_hint_style_name()`: convert a hinting strength from and to a name.
- `font_parse_aa_mode()`, `font_aa_mode_name()`: convert an antialiasing mode from and to a name.
- `font_set_family()`, `font_get_family()`: set and get the configured family list.
- `font_list_families()`: lists the installed monospace families.
- `font_set_configured_size()`, `font_configured_size()`: set and get the size that a zero size resolves to.
- `font_apply_settings()`: applies a font settings struct. With a previous struct, it sets only the values that changed.
- `font_backend_init()`, `font_backend_shutdown()`: start and stop the backend. Tests and the system layer only.
- `font_thread_attach()`, `font_thread_detach()`: load and free the backend state of the calling thread.
- `font_generation()`: returns the global font generation.
- `font_bump_generation()`: increments the global font generation.

### `src/font/glyph_cache.h`

- `font_system_init()`: starts the backend and makes the default instance.
- `font_system_shutdown()`: frees every instance and stops the backend.
- `glyph_cache_rebuild()`: reopens a stale face, then drops and fills the caches of an instance.
- `glyph_cache_begin_frame()`: sets the tile colors and rebuilds a stale instance.
- `glyph_cache_refresh()`: rebuilds an instance when the font changed. Call it once per frame.
- `glyph_cache_set_parallel()`: turns parallel mode on or off.
- `glyph_cache_flush_deferred()`: frees the buffers that the calling thread retired.
- `glyph_cache_flush_frame()`: frees the run entries retired during the frame and the deferred buffers of the calling thread.
- `glyph_cache_thread_detach()`: frees the glyph cache buffers of the calling thread.
- `glyph_cache_fetch()`: returns the glyph of a code point and a style.
- `glyph_cache_fetch_tile()`: returns the glyph pre-blended over the tile colors, or NULL.
- `glyph_cache_shape_segment()`: shapes a segment and calls `emit` for each span of shaped cells.
