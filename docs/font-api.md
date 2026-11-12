# Font API

The font API is in `src/font/font.h` and `src/font/glyph_cache.h`.

## Vocabulary

| Word | Meaning |
|---|---|
| lookup | A cache read. It returns a found entry or NULL. It never builds data. Only internal code uses `cache_slot_lookup()`. The public API has no lookup function. OpenType layout in `src/font/glyph/layout.h` uses the term for a different concept. |
| fetch | A cache read that builds missing data. The function returns a borrowed pointer. `glyph_cache_fetch()` and `glyph_cache_fetch_tile()` perform fetches. |
| acquire | An operation that returns an owned reference. `font_instance_unref()` releases one reference. `font_instance_ref()` adds one reference. |
| render | An operation that rasterizes glyphs. It does not read or write the cache. `font_instance_render_glyph()` writes pixels into a caller-provided `struct glyph`. |
| rebuild | An operation in `glyph_cache_rebuild()`. It reopens the font face when the font generation changes. It drops all cached glyphs, tiles, and runs of the instance. It then caches ASCII and Latin-1 glyphs again. |
| thread_attach, thread_detach | Management of per-thread backend state. `font_thread_attach()` initializes thread state before use. `font_thread_detach()` frees thread state. `glyph_cache_thread_detach()` frees per-thread glyph cache buffers. |
| `font_handle_t` | An opaque face handle for one font size. The backend in `src/font/backend.h` creates and owns this handle. Each instance stores one handle in `face`. The handle can be NULL. |
| slot | A storage location. The term has three meanings. A cache slot (`struct cache_slot`) stores one glyph or tile. It publishes the buffer through an atomic pointer. The buffer header stores the key. A face slot (`struct ft_face_slot`, FreeType) holds one font file and its four variants. Variant arrays use `enum font_variant` as an index, not a slot index. |
| variant | One of four typeface styles in a family: plain, bold, italic, and bold italic. `enum font_variant` in `src/font/font.h` defines these values: plain (0), bold (1), italic (2), and bold italic (3). The expression `style & FONT_STYLE_VARIANT_MASK` or `font_variant_of()` calculates this index. Variant arrays use this index: `ft_face_slot.styles[]`, `hb[]`, `layout_gids[]`, `font_instance.layouts[]`, and CoreText `hb_face[]` and `hb_font[]`. |
| style | A bitmask of `enum font_style` flags: bold, italic, emoji, text, and wide. It does not represent raw cell attributes. `render_cell_style()` creates a style mask from a cell. |
| `_px` | A length in device pixels. |
| `_pt` | A size in points. `FONT_PT_TO_PX()` converts points to device pixels at scale 1. |
| `_120` | A display scale in 1/120 units. The value 120 represents scale 1. A zero value represents 120. |
| `built_*` | Snapshot values stored in the instance during the last rebuild. `built_gen` stores the font generation. `built_fg` and `built_bg` store the tile colors. Staleness checks compare these snapshots against current values. |

## Ownership and lifetime

| Call | Result | Lifetime |
|---|---|---|
| `font_instance_acquire()` | Owned reference, or NULL | Valid until `font_instance_unref()` releases it. |
| `font_instance_ref()` | The same pointer, one more reference | Valid until a matching `font_instance_unref()` call. |
| `font_instance_unref()` | Nothing | Releases one reference. The final unref frees the instance, caches, and face. |
| `font_instance_default()` | Borrowed, can be NULL | Valid between `font_system_init()` and `font_system_shutdown()`. The pointer is NULL before initialization or after initialization fails. |
| `font_instance_metrics()` | Borrowed | Valid for the lifetime of the instance. A rebuild updates values in place. |
| `glyph_cache_fetch()` | Borrowed glyph | See [Glyph and tile pointers](#glyph-and-tile-pointers). |
| `glyph_cache_fetch_tile()` | Borrowed pixels, or NULL | See [Glyph and tile pointers](#glyph-and-tile-pointers). |
| `glyph_cache_shape_segment()` | A `struct glyph_span` passed to `emit` | Valid only during the execution of that `emit` callback. |
| `font_instance_render_glyph()` | Fills a caller-owned `struct glyph` | Allocated planes remain valid until `glyph_release()` frees them. Subsequent renders into the same glyph reuse allocated planes. |
| `font_list_families()` | Owned array of owned strings | The array and each string require deallocation. |

`font_instance_acquire()` interns font instances. Identical size and scale parameters reuse the existing instance while a reference exists. A cache hit rebuilds a stale instance in place and invalidates previous glyph pointers of that instance.

`font_instance_default()` also rebuilds a stale instance in place. A rebuild invalidates existing glyph pointers of the default instance.

### Glyph and tile pointers

A fetch result comes from one of four sources:

- Thread scratch: Allocation failures return scratch storage. For `glyph_cache_fetch()`, code points above U+10FFFF also return scratch storage. For those code points, `glyph_cache_fetch_tile()` returns NULL. Scratch memory stays valid until the next `glyph_cache_fetch()` or `glyph_cache_fetch_tile()` call on the same thread. Because `glyph_cache_fetch_tile()` calls `glyph_cache_fetch()`, a tile fetch also overwrites scratch memory.
- A cache slot in serial mode: Refilling a slot with a new key overwrites the existing glyph in place. Existing pointers then reference the new glyph.
- A cache slot in parallel mode: Refilling a slot allocates a new buffer. Published buffers never change. Concurrent refills by multiple threads produce separate buffers. The replacing thread places the replaced buffer on its deferred free list. Replaced buffers remain valid until `glyph_cache_flush_deferred()` or `glyph_cache_flush_frame()` runs on that thread.
- A primed ASCII tile: An instance rebuild frees primed tiles immediately without placing them on a deferred list.

Every rebuild retires all glyphs, tiles, and runs of an instance. In serial mode, a rebuild frees this memory immediately. Five functions trigger a rebuild: `glyph_cache_rebuild()`, `glyph_cache_begin_frame()`, `glyph_cache_refresh()`, `font_instance_acquire()`, and `font_instance_default()`. Fetch and shape operations never trigger a rebuild. A rebuild invalidates all previously returned fetch pointers.

`struct glyph_span` cells originate from run cache entries or thread scratch (`priv_run_cells`). In parallel mode, cache eviction can retire a run entry. Evicted entries remain valid until `glyph_cache_flush_frame()` runs. Span cells therefore stay valid through the end of the `emit` callback.

## Stale checks

A font configuration change increments `font_generation()`. Rebuild operations occur only at frame start or between frames. The following calls check for staleness:

| Call | Checks | Action when stale |
|---|---|---|
| `glyph_cache_begin_frame()` | Sets tile colors, then compares `built_gen`, `built_fg`, and `built_bg` with current values. | Rebuilds the instance. Serial mode only. |
| `glyph_cache_refresh()` | Compares `built_gen` against the current font generation. | Rebuilds the instance. Serial mode only. |
| `font_instance_acquire()` (hit) | Compares `built_gen`, `built_fg`, and `built_bg` against current values. | Rebuilds the instance. Serial mode only. |
| `font_instance_default()` | Compares `built_gen`, `built_fg`, and `built_bg` against current values. | Rebuilds the instance. Serial mode only. |
| `glyph_cache_rebuild()` | Compares `built_gen` against the current font generation. | Reopens the face when stale. Always clears and repopulates caches. |
| `glyph_cache_fetch()`, `glyph_cache_fetch_tile()`, `glyph_cache_shape_segment()` | None | Never rebuild. |
| `font_instance_render_glyph()` | None | Renders with current instance face and metrics. |

A fetch operation after a generation increment and before the next frame-start rebuild returns a glyph from the previous face. Memory remains allocated and valid. The subsequent frame displays the new face.

In parallel mode, `glyph_cache_refresh()`, `glyph_cache_begin_frame()`, `font_instance_acquire()` on a hit, and `font_instance_default()` do not rebuild. `glyph_cache_rebuild()` requires serial execution because active paint workers read cache memory. The renderer invokes `glyph_cache_refresh()` before enabling parallel mode.

Tile colors are process-global. Each frame defines one color pair. Only `glyph_cache_begin_frame()` updates tiles to match current colors. A `glyph_span.tiles` pointer performs no color check.

Each frame begins on the frame thread with one of these calls before any glyph fetch:

- `render_draw()` refreshes the grid font and UI font with `glyph_cache_refresh()` in serial mode before any fetch. It does not invoke `glyph_cache_begin_frame()`, leaving default-color tiles unchanged.
- `ui_scene_paint()` refreshes font state with `glyph_cache_refresh()`. `ui_scene_paint_clipped()` performs no refresh.

`font_instance_version()` returns a distinct version number whenever instance identity or font generation changes. A version mismatch against a painted buffer indicates that the buffer requires a full repaint.

## Failed reopen

`glyph_cache_rebuild()` reopens the face of a stale instance through these steps:

1. It frees layout analysis data for each variant.
2. It closes the previous face and sets the face pointer to NULL.
3. It opens a new face for the configured family at the instance size.
4. When the open succeeds, it measures the new face. A measurement failure preserves previous metrics.
5. When the open fails, the face pointer remains NULL. `font_measure_size()` opens, measures, and closes a temporary face. When that measurement also fails, previous metrics remain.
6. It records the current font generation in `built_gen`, even after a failure.

An open failure permanently drops the previous face. An instance without a face renders cell-aligned symbols directly. It renders ASCII characters with an internal 5x7 font and replaces other code points with replacement glyph boxes. It performs no text shaping.

A reopen failure does not trigger retries on subsequent frames because `built_gen` matches the current generation. The next generation increment triggers a new reopen attempt at the start of the next frame. The function returns no error status.

`font_instance_acquire()` creates new instances with stricter error handling. It returns NULL when face opening fails and temporary measurement fails. It also returns NULL when measurement of an opened face fails.

## Re-entrancy

`glyph_cache_shape_segment()` stores scratch data in thread-local buffers: `seg_canon`, `seg_roles`, and `priv_run_cells`. A recursive invocation of `glyph_cache_shape_segment()` from inside its `emit` callback overwrites or frees active thread scratch buffers.

Segment scratch memory expands to accommodate the longest shaped segment on that thread. `glyph_cache_reserve_segment(n)` preallocates scratch capacity for `n` code points. Preallocated buffers avoid reallocations during subsequent shaping operations of up to `n` code points.

The `emit` callback safely invokes `glyph_cache_fetch()` and `glyph_cache_fetch_tile()`. These fetch functions do not modify the run cache, `priv_run_cells`, or segment scratch memory. Previously returned span cells remain valid. Glyph renders within nested fetches allocate separate planes.

A font generation increment during `emit` frees no memory. Nested fetch operations do not rebuild instances, preserving span cells and layout analysis data. Thread callbacks such as CoreText font registration can increment the generation at any time. The next frame start completes the instance rebuild.

Rebuilding functions (`glyph_cache_rebuild()`, `glyph_cache_begin_frame()`, `glyph_cache_refresh()`, `font_instance_acquire()`, and `font_instance_default()`) invalidate cache data and must not run inside an `emit` callback.

## Thread affinity

| Call | Thread |
|---|---|
| `font_system_init()`, `font_system_shutdown()` | Main thread only. |
| `font_instance_acquire()`, `font_instance_ref()`, `font_instance_unref()` | Main thread only. Reference counts and instance lists have no locking. |
| `font_instance_default()` | Main thread only. It can trigger an instance rebuild. |
| `glyph_cache_rebuild()`, `glyph_cache_begin_frame()`, `glyph_cache_refresh()` | Main thread only, outside parallel execution frames. |
| `glyph_cache_set_parallel()`, `glyph_cache_flush_frame()` | Main thread only. `glyph_cache_flush_frame()` requires that all worker threads have joined. |
| `font_set_family()`, `font_set_hint_style()`, `font_set_aa_mode()`, `font_set_configured_size()`, `font_apply_settings()` | Main thread only, between frames. |
| `glyph_cache_fetch()`, `glyph_cache_fetch_tile()`, `glyph_cache_shape_segment()` | Any thread in parallel mode. One thread at a time in serial mode. |
| `font_instance_render_glyph()`, `font_bind_thread()` | Any thread. These functions read instance state and write per-thread state only. They require that no rebuild runs concurrently on that instance. |
| `font_thread_attach()`, `font_thread_detach()` | Calling thread only. Each thread manages its own state. |
| `glyph_cache_flush_deferred()`, `glyph_cache_thread_detach()`, `glyph_cache_reserve_segment()` | Calling thread only. |
| `font_generation()`, `font_bump_generation()` | Any thread. The counter is atomic. |

Worker threads manage state during their lifecycle. A worker attaches backend state with `font_thread_attach()` at worker start and job start. A worker flushes deferred buffers with `glyph_cache_flush_deferred()` at job start. A worker frees resources with `glyph_cache_thread_detach()` and `font_thread_detach()` before exit. Backend functions also attach thread state on demand.

## Counters

| Counter | Scope | Changes when |
|---|---|---|
| `font_generation()` | Global, atomic | Font family updates, hinting or antialiasing changes, `font_backend_shutdown()`, and CoreText font registration increment this counter. `font_bump_generation()` also increments it. |
| `font_instance_version()` | Per instance, computed | Value equals `(id << 32) \| (uint32_t) generation`. |
| `built_gen` | Per instance | A rebuild or new instance creation writes the current font generation here. |
| `built_fg`, `built_bg` | Per instance | A rebuild writes current tile colors here. |
| `run_stamp` | Per instance | Each run cache insertion increments this stamp. A cache hit updates the entry stamp. Cache eviction drops the entry with the oldest stamp. |
| `ft_sync_epoch` | FreeType, global, atomic | Font family reloads, hinting changes, and backend shutdown increment this epoch. |
| `synced_epoch` | FreeType, per thread | Records the epoch when this thread loaded its font faces. When this value differs from `ft_sync_epoch`, the next backend call reloads thread faces. |
| `fit_pass` | FreeType, per thread | Each run fit pass increments this counter. Raster cache entries for the current pass remain protected from eviction during that pass. CoreText uses `ct_use_pass` for identical protection. |
| `slot_reuse` | FreeType, per face slot | Releasing a face slot increments this counter to invalidate older raster cache entries in that slot. |
| `face_stamp` | FreeType, per thread | Face loading and shaped run operations increment this stamp. Face eviction drops the slot with the oldest stamp. |

## Public API

### `src/font/font.h`

- `font_prefers_emoji()`: Returns true when a code point renders as emoji under the specified style.
- `font_size_px()`: Calculates the em size in device pixels for a `struct font_size`.
- `font_size_resolve()`: Replaces a zero size with the configured size and a zero scale with 120.
- `font_instance_acquire()`: Returns an owned font instance for a specified size and scale.
- `font_instance_ref()`: Increments the instance reference count.
- `font_instance_unref()`: Decrements the instance reference count and frees the instance on the last drop.
- `font_instance_default()`: Returns a borrowed pointer to the default font instance.
- `font_instance_metrics()`: Returns cell metrics for an instance.
- `font_instance_size_pt()`: Returns instance size in points.
- `font_instance_scale_120()`: Returns instance display scale.
- `font_instance_version()`: Returns a version integer that changes whenever glyph appearance changes.
- `font_bind_thread()`: Configures the calling thread to rasterize at the instance size.
- `font_instance_render_glyph()`: Rasterizes one code point into a caller-supplied glyph structure.
- `glyph_prepare()`: Allocates glyph planes for a specified width.
- `glyph_release()`: Deallocates glyph planes.
- `glyph_color_plane()`, `glyph_subpx_plane()`: Allocate color or subpixel planes.
- `glyph_mask()`, `glyph_mark()`, `glyph_row_inked()`, `glyph_inked()`: Read and write glyph ink bits.
- `glyph_color_at()`, `glyph_subpx_at()`: Calculate pixel addresses within a glyph plane.
- `font_set_hint_style()`: Sets font hinting strength. An auto setting queries the system value.
- `font_set_aa_mode()`: Sets antialiasing mode. An auto setting queries the system value.
- `font_parse_hint_style()`, `font_hint_style_name()`: Convert hinting strength between string names and enum values.
- `font_parse_aa_mode()`, `font_aa_mode_name()`: Convert antialiasing mode between string names and enum values.
- `font_set_family()`, `font_get_family()`: Set and read configured font family names.
- `font_list_families()`: Returns a list of installed monospace font families.
- `font_set_configured_size()`, `font_configured_size()`: Set and read the default fallback font size.
- `font_apply_settings()`: Applies font settings, updating only values that changed from a previous configuration.
- `font_backend_init()`, `font_backend_shutdown()`: Initialize and terminate the font backend.
- `font_thread_attach()`, `font_thread_detach()`: Initialize and release per-thread backend resources.
- `font_generation()`: Returns the global font generation number.
- `font_bump_generation()`: Increments the global font generation number.

### `src/font/glyph_cache.h`

- `font_system_init()`: Initializes the font backend and creates the default font instance.
- `font_system_shutdown()`: Frees all font instances and stops the font backend.
- `glyph_cache_rebuild()`: Reopens a stale font face and rebuilds instance caches.
- `glyph_cache_begin_frame()`: Updates tile colors and rebuilds a stale instance in serial mode.
- `glyph_cache_refresh()`: Rebuilds an instance when the font generation changes.
- `glyph_cache_set_parallel()`: Enables or disables parallel cache mode.
- `glyph_cache_flush_deferred()`: Frees deferred buffers on the calling thread.
- `glyph_cache_flush_frame()`: Frees retired run cache entries and thread deferred buffers.
- `glyph_cache_thread_detach()`: Deallocates glyph cache buffers for the calling thread.
- `glyph_cache_fetch()`: Retrieves a glyph for a code point and style.
- `glyph_cache_fetch_tile()`: Retrieves a pre-blended tile glyph for the current colors, or NULL.
- `glyph_cache_shape_segment()`: Shapes text code points into glyphs and delivers spans to an `emit` callback.
- `glyph_cache_reserve_segment()`: Preallocates segment scratch memory on the calling thread.
