#ifndef RENDER_SURFACE_H
#define RENDER_SURFACE_H

/*
 * The state of one surface: what its persistent canvas holds now. It lives
 * in framebuffer_history.data. The zero value is valid: valid == false
 * means the canvas pixels are unknown.
 *
 * Per row it keeps the row key, the UI ink span, and the spill reach. One
 * extra entry describes the bottom margin, the band below the last full
 * row. That pseudo-row has a UI ink span and no key.
 *
 * Threads: main thread only, outside the parallel phase.
 */

#include <stdbool.h>
#include <stdint.h>

#include "paint/framebuffer.h"
#include "render/plan.h"
#include "util/geom.h"

/*
 * Everything that changes every pixel of the grid or the canvas. A change
 * makes the frame invalid: everything paints.
 * - `canvas`: the canvas size.
 * - `grid`: the grid rect clipped to the canvas. Empty: no grid.
 * - `cells`: the terminal size, frame cols and rows. With `grid` and `cell`
 *   it also fixes the visible columns and rows. A terminal resize rewraps
 *   rows and keeps their row keys, so it must repaint everything.
 * - `cell`: the cell size.
 * - `font_version`: font_instance_version() of the grid font.
 * - `palette_gen`, `default_fg`, `default_bg`, `cursor_color`,
 *   `reverse_video`, `blink_on`, `selection_fg`, `selection_bg`: from the
 *   frame, with the selection colors resolved. The cursor color is here
 *   because a palette edit is not the only way to change it. A blink phase
 *   change repaints the frame.
 * - `base`: render_frame.base.
 * Build it from a zero struct, so that unused fields compare equal.
 */
struct render_surface_key {
    struct vector2i canvas;
    struct rect2i grid;
    struct vector2i cells;
    struct vector2i cell;
    uint64_t font_version;
    uint64_t palette_gen;
    uint32_t default_fg, default_bg;
    uint32_t cursor_color;
    uint32_t selection_fg, selection_bg;
    uint32_t base;
    bool reverse_video;
    bool blink_on;
};

/* Field by field. It does not compare padding. */
bool render_surface_key_eq(const struct render_surface_key *a,
                           const struct render_surface_key *b);

/*
 * - `rows`: the rows that the arrays describe: the visible rows of the grid.
 * - `cap`: the allocated rows. The arrays have cap entries, and `ui` has
 *   cap + 1.
 * - `keys[r]`: the row key that canvas row r shows. 0 means unknown.
 * - `ui[r]`: the UI ink span over row r. ui[rows] is the bottom margin.
 * - `reach[r]`: the spill reach of row r from its last full paint.
 * - `gpu`: per-surface GPU state, owned by render/gpu/. NULL without a GPU.
 *   render_surface_release() frees it with render_gpu_surface_free().
 * - `ui_font_version`: font_instance_version() of the UI font of the last
 *   frame, or 0. render_draw_view() sets it. A different UI font changes
 *   the UI pixels but not the scene ops, so the call then repaints all of
 *   the UI. The other functions in this header do not touch it.
 */
struct render_surface {
    struct render_surface_key key;
    uint64_t *keys;
    struct plan_span *ui;
    uint16_t *reach;
    void *gpu;
    uint64_t ui_font_version;
    int rows;
    int cap;
    bool valid;
};

/*
 * The surface state of a history. The first call allocates it and sets
 * history->data and history->destroy. Returns NULL when the memory is
 * missing, or when another owner put data in the history. render_draw_view()
 * then paints like a draw without history.
 */
struct render_surface *render_surface_of(struct framebuffer_history *history);

/* For a surface that the renderer embeds, such as its scratch surface. */
void render_surface_init(struct render_surface *s);
void render_surface_release(struct render_surface *s);
/* The history destroy function. It releases and frees `data`. */
void render_surface_destroy(void *data);

/*
 * Makes room for `rows` rows. It allocates only on the first call and when
 * rows > cap. It keeps the contents. Returns false when the memory is
 * missing. The surface is then invalid.
 */
bool render_surface_reserve(struct render_surface *s, int rows);

/* Forgets the canvas contents: valid = false. It keeps the memory. */
void render_surface_invalidate(struct render_surface *s);

/*
 * Marks `s` valid with the given state for `rows` rows. A NULL `key`, a row
 * count above the reserved rows, or a missing `ui` array invalidates the
 * surface instead.
 */
void render_surface_commit(struct render_surface *s, const struct render_surface_key *key,
                           int rows, const uint64_t *keys, const struct plan_span *ui,
                           const uint16_t *reach);

#endif
