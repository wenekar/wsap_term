#ifndef RENDER_CPU_CELL_CACHE_H
#define RENDER_CPU_CELL_CACHE_H

/*
 * The composited cell cache. A tile is one cell, fully composed: its bg,
 * its part of the glyph, its decorations, and its cursor marks. It has
 * cw * ch opaque pixels with a row stride of cw. The cache generalizes the
 * old default-color tiles of the font to any color pair.
 *
 * Pixels never depend on the cache. A hit blits the tile that a miss would
 * compose. The parity tests check this with a small budget, a cold cache,
 * and a warm cache.
 *
 * Storage
 * - One class for each cell size (cw, ch). A class has slabs of tiles, a
 *   key array, a stamp array, and an index.
 * - One byte budget covers all classes. When a class needs room and the
 *   budget is full, the least recently used other class is freed first.
 *   Only a class that no frame used in the last 8 frames is freed so. Else
 *   the class takes slabs from the largest other class, while that class
 *   stays at least as large. After that, the class evicts its own tiles.
 * - A class grows by whole slabs. A slab never moves, so a tile pointer
 *   stays valid until its slot is evicted.
 * - A class needs room for at least CELL_CACHE_MIN_TILES tiles. Else
 *   cell_cache_begin() returns NULL and the frame composes every cell.
 * - The index is open-addressed with linear probes. Each slot has a clock
 *   stamp: the frame number of its last use.
 * - Each class has its L0 tables (see below). The budget does not count
 *   them.
 *
 * Frame protocol
 * 1. Main thread, before the phase: cell_cache_begin().
 *    - It picks the class of the cell size. It clears the L0 tables of the
 *      class when the L0 domain changed (see below).
 *    - It fills the free list up to max(the claims that the last frame
 *      asked for, CELL_CACHE_REFILL_MIN). The first refill of a class
 *      fills it up to the cell count of the frame, when that is more. It
 *      takes never-used slots first, and it grows the class within the
 *      budget. Then it evicts by clock, but only up to the claims that the
 *      last frame asked for. A slot used in the last frame is never
 *      evicted. So an idle frame evicts nothing. A claim that found the
 *      free list empty still counts as asked. "The last frame" is the last
 *      frame of this class.
 *    - It resets each worker: pending table, hit log, claim count.
 * 2. Workers, during the phase: cell_cache_find(), cell_cache_claim().
 *    - The index, the keys, the stamps, the L0 tables, and the published
 *      tiles do not change during the phase. Readers need no lock.
 *    - A worker claims a free slot with one atomic_fetch_add on the free
 *      list cursor. It writes the tile, and it records the key and the slot
 *      in its own pending table. The pending table also serves the repeats
 *      of that key in the same frame, for that worker.
 *    - A worker writes its hits into its own hit log. It never writes a
 *      shared stamp.
 *    - When the free list is empty, claim returns NULL. The worker then
 *      composes the cell straight into the canvas.
 * 3. Main thread, after the join: cell_cache_publish().
 *    - It inserts the pending entries into the index. When two workers
 *      claimed the same key, it keeps one slot and frees the other.
 *    - It stamps the hit logs.
 *    - It puts each new slot and each logged hit of the L0 domain into L0.
 *    - It records the claim count for the next refill, and it advances the
 *      clock.
 *
 * Admission: each worker has a seen filter of CELL_CACHE_SEEN entries. It
 * admits a key on its second sighting. So a flood of one-off color pairs
 * (lolcat) does not evict useful tiles, and a flood of one-off shaped
 * cells does not pay a claim for each cell. The filter only steers
 * admission. It never changes pixels.
 *
 * L0: each class has two tables for the default color pair of the frame.
 * All workers read them. A cell in the default colors is then one or a few
 * loads.
 * - The ASCII table is direct-mapped [variant][cp < 128]. It holds the
 *   unshaped ids with text < 128.
 * - The id table holds the other ids: shaped cells and other text. It has
 *   sets of CELL_CACHE_L0_WAYS entries, picked by a hash of the id. Each
 *   entry keeps its id. A new id goes into the first entry of its set. The
 *   other entries move back by one, and the last one drops.
 * - The domain is (font version, default fg, default bg). A key is in the
 *   domain when it has that font version and that pair, no pixel flags,
 *   and half 0. cell_cache_begin() clears the tables when the domain
 *   changed.
 * - Only publish fills L0. So a key that a frame claims hits L0 from the
 *   next frame on.
 * - A slot that leaves the index (eviction, a slab that a class gives up)
 *   leaves L0 too. So L0 never points to a free slot, and it survives
 *   evictions.
 * - Before it evicts, cell_cache_begin() stamps each slot that L0 holds as
 *   used in the last frame. So L0 hits count as uses.
 *
 * Budget 0 or a disabled cache: cell_cache_create() returns NULL. Every
 * call accepts a NULL cache or a NULL class and then does nothing, finds
 * nothing, and claims nothing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "font/font.h"
#include "util/geom.h"

#define CELL_CACHE_SEEN 4096
#define CELL_CACHE_HIT_LOG 4096
#define CELL_CACHE_PENDING 2048
#define CELL_CACHE_MIN_TILES 16
#define CELL_CACHE_REFILL_MIN 256
#define CELL_CACHE_L0_CPS 128
#define CELL_CACHE_L0_ID_BITS 11
#define CELL_CACHE_L0_IDS (1 << CELL_CACHE_L0_ID_BITS)
#define CELL_CACHE_L0_WAYS 4

/*
 * The key of a tile. 32 bytes, with no padding. Compare it in full.
 * Build it from a zero struct.
 *
 * - `glyph`: render_glyph_ref.id. Never 0: an id of 0 is not cached.
 * - `font_version`: font_instance_version() of the grid font.
 * - `fg`, `bg`: render_cell.fg and render_cell.bg.
 * - `cursor`: terminal_frame.cursor_color when `flags` has a CURSOR bit,
 *   else 0. The design key had no cursor color. Without it, two terminals
 *   with different cursor colors would share a cursor tile.
 * - `flags`: render_cell.flags & RENDER_CELL_PIXEL_MASK.
 * - `half`: 0 for a one-column cell and for the left half of a wide glyph,
 *   1 for the right half.
 * - `zero`: always 0.
 */
struct cell_key {
    uint64_t glyph;
    uint64_t font_version;
    uint32_t fg, bg;
    uint32_t cursor;
    uint16_t flags;
    uint8_t half;
    uint8_t zero;
};

_Static_assert(sizeof(struct cell_key) == 32, "a cell key is 32 bytes");

static inline bool cell_key_eq(const struct cell_key *a, const struct cell_key *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/*
 * A tile. `px` NULL means a miss. `spill` means: the glyph of this tile
 * has ink outside the cell. On a hit with `spill`, the raster fetches the
 * glyph anyway and adds its spill coverage, as compose would.
 */
struct cell_tile {
    const uint32_t *px;
    bool spill;
};

/* One entry of the L0 id table. id 0 is an empty entry. */
struct cell_l0_id {
    uint64_t id;
    const uint32_t *px;
};

/*
 * The L0 tables of one class. Read them with cell_l0_find() and
 * cell_l0_find_id(). Only the cache writes them.
 * - px[v][cp] NULL is a miss.
 * - Bit cp of spill[v][cp / 64] is the spill flag of that tile.
 * - The set of id starts at ids[cell_l0_index(id)]. An id that no entry
 *   of the set holds is a miss.
 * - Bit i of id_spill[i / 64] is the spill flag of ids[i].
 */
struct cell_l0 {
    const uint32_t *px[FONT_VARIANT_COUNT][CELL_CACHE_L0_CPS];
    uint64_t spill[FONT_VARIANT_COUNT][CELL_CACHE_L0_CPS / 64];
    struct cell_l0_id ids[CELL_CACHE_L0_IDS];
    uint64_t id_spill[CELL_CACHE_L0_IDS / 64];
};

/*
 * Looks up an ASCII cell of the default pair. The caller checks the domain
 * first: unshaped id, text < 128, no pixel flags, half 0, the default fg
 * and bg, and a style with only variant bits.
 */
static inline struct cell_tile cell_l0_find(const struct cell_l0 *l0, uint32_t cp, uint16_t style)
{
    unsigned v = style & FONT_STYLE_VARIANT_MASK;
    return (struct cell_tile){
        .px = l0->px[v][cp],
        .spill = (l0->spill[v][cp >> 6] >> (cp & 63)) & 1,
    };
}

/*
 * The first entry of the id table set of a glyph id. A shaped id is a hash
 * already, but ids of one span are related. One multiply leaves them in
 * few sets, so the index mixes the id in full (the MurmurHash3 finalizer).
 */
static inline unsigned cell_l0_index(uint64_t id)
{
    id ^= id >> 33;
    id *= UINT64_C(0xFF51AFD7ED558CCD);
    id ^= id >> 33;
    id *= UINT64_C(0xC4CEB9FE1A85EC53);
    return (unsigned)(id >> (64 - CELL_CACHE_L0_ID_BITS)) & ~(unsigned)(CELL_CACHE_L0_WAYS - 1);
}

/*
 * Looks up a cell of the default pair by its glyph id. The caller checks
 * the domain first: id not 0, no pixel flags, half 0, and the default fg
 * and bg.
 */
static inline struct cell_tile cell_l0_find_id(const struct cell_l0 *l0, uint64_t id)
{
    unsigned i = cell_l0_index(id);
    for (unsigned j = i; j < i + CELL_CACHE_L0_WAYS; j++)
        if (l0->ids[j].id == id)
            return (struct cell_tile){
                .px = l0->ids[j].px, .spill = (l0->id_spill[j >> 6] >> (j & 63)) & 1,
            };
    return (struct cell_tile){0};
}

struct cell_cache;
struct cell_class;
struct cell_cache_worker;

/* `budget` in bytes. 0 returns NULL: no cache. Main thread only. */
struct cell_cache *cell_cache_create(size_t budget);
void cell_cache_destroy(struct cell_cache *c);

/* Per-worker state: seen filter, pending table, and hit log. */
struct cell_cache_worker *cell_cache_worker_create(void);
void cell_cache_worker_destroy(struct cell_cache_worker *w);

/* The L0 tables of a class. NULL gives empty tables. */
const struct cell_l0 *cell_class_l0(const struct cell_class *k);

/*
 * Starts the parallel phase for one cell size. Main thread only. Returns
 * the class, or NULL: no cache this frame. `cells` is the number of cells
 * that the frame paints. `workers` lists the worker states that take part,
 * the main thread first. Allocation happens only here and only while a
 * class grows.
 */
struct cell_class *cell_cache_begin(struct cell_cache *c, struct vector2i cell,
                                    uint64_t font_version,
                                    uint32_t default_fg, uint32_t default_bg, int cells,
                                    struct cell_cache_worker *const *workers, int n_workers);

/*
 * Finds a tile. It looks in the published index, then in the pending table
 * of `w`. An index hit goes into the hit log of `w`. Any thread during the
 * phase.
 */
struct cell_tile cell_cache_find(const struct cell_class *k, struct cell_cache_worker *w,
                                 const struct cell_key *key);

/*
 * Asks for a slot for a key that missed. The seen filter admits the key on
 * its second sighting. Returns a tile buffer of cw * ch pixels to compose
 * into, or NULL: compose straight into the canvas. The caller must fill
 * the whole tile before its next call into the cache. `spill` is the spill
 * flag that cell_cache_find() returns for this key later.
 * Any thread during the phase.
 */
uint32_t *cell_cache_claim(struct cell_class *k, struct cell_cache_worker *w,
                           const struct cell_key *key, bool spill);

/*
 * Ends the phase. Main thread, after the join. Pass the same workers as to
 * cell_cache_begin().
 */
void cell_cache_publish(struct cell_cache *c, struct cell_class *k,
                        struct cell_cache_worker *const *workers, int n_workers);

#endif
