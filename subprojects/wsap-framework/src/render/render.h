#ifndef RENDER_H
#define RENDER_H

/*
 * The public renderer API of this library.
 *
 * render_draw_view() (render/core.h) draws a frame that the caller has
 * already resolved into a render_grid_view. The terminal layer above this
 * library wraps it in render_draw(), which adds the terminal frame. The
 * rules below hold for both calls.
 *
 * The app owns one renderer. Only the app calls render_draw(). The test
 * oracle for a full paint is a second renderer that draws a frame with no
 * history.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paint/framebuffer.h"
#include "util/damage.h"
#include "util/geom.h"

struct font_instance;
struct renderer;
struct terminal_frame;
struct ui_damage;
struct ui_scene;

/* An explicit "off" value for the counts in struct render_options. */
#define RENDER_NONE (-1)

/* The cell cache budget that a zero cache_bytes gives. */
#define RENDER_CACHE_DEFAULT_BYTES ((ptrdiff_t)4 << 20)

/*
 * Renderer settings. An all-zero struct, or a NULL pointer, gives the
 * defaults.
 *
 * - `isa`: an enum render_isa value (render/cpu/isa.h). 0 is
 *   RENDER_ISA_AUTO: the WSAP_RENDER_ISA environment value, else the best
 *   level that the CPU has. A level that the CPU does not have falls back
 *   as render_isa_resolve() decides. renderer_isa() tells the result.
 * - `threads`: worker threads in addition to the main thread.
 *   0: the WSAP_RENDER_THREADS environment value, else min(nproc - 1, 3).
 *   RENDER_NONE: no workers. The main thread does all the work.
 *   N > 0: N workers, up to POOL_WORKERS_MAX (render/cpu/pool.h).
 * - `cache_bytes`: the byte budget of the composited cell cache.
 *   0: RENDER_CACHE_DEFAULT_BYTES. RENDER_NONE: no cell cache.
 *   A budget too small for the cell size also gives no cache.
 * - `disable_gpu`: true skips GPU device creation. The renderer uses only
 *   CPU paths.
 *
 * The settings never change pixels. They change only speed and memory.
 */
struct render_options {
    int isa;
    int threads;
    ptrdiff_t cache_bytes;
    bool disable_gpu;
};

/*
 * The terminal grid of a frame.
 *
 * - `frame` NULL, or an invalid frame, means: no grid. The base color and
 *   the UI then cover `rect`.
 * - `font` NULL means font_instance_default().
 * - `rect` is the grid area in canvas pixels. render_draw() clips it to the
 *   canvas. An empty rect, or a rect with a negative origin, draws no grid.
 * - The cells start at rect.pos. Visible columns are
 *   min(frame->cols, width / cell_w), and visible rows are
 *   min(frame->rows, height / cell_h), after the clip.
 * - The rest of the rect is margin. The margin is frame->default_bg in
 *   both video modes: reverse video swaps the colors of the cells only.
 */
struct render_grid {
    const struct terminal_frame *frame;
    struct font_instance *font;
    struct rect2i rect;
};

/*
 * One frame.
 *
 * - `base` fills every canvas pixel outside the grid, under the UI.
 *   It can be translucent. Popups use base 0.
 * - `ui` NULL means an empty scene.
 * - `ui_font` NULL means font_instance_default().
 * - `ui_damage` lists the canvas rects where this scene can differ from the
 *   scene of the last render_draw() with this history. NULL means: all of
 *   the UI changed.
 * - `repaint` means: forget the surface state and paint every pixel.
 * - `cpu_only` means: do not draw on the GPU. Screenshots set it, because
 *   they need the pixels in the canvas. A frame with a drawable still
 *   presents through the GPU, from the CPU pixels.
 *
 * Every pointer must stay valid during render_draw(). render_draw() keeps
 * no pointer after it returns. It holds no font reference.
 */
struct render_frame {
    struct render_grid grid;
    const struct ui_scene *ui;
    struct font_instance *ui_font;
    const struct ui_damage *ui_damage;
    uint32_t base;
    bool repaint;
    bool cpu_only;
};

/*
 * The result of one render_draw().
 *
 * - `damage` lists the canvas pixels that changed. Its coordinates are
 *   canvas pixels. damage.any false means: no pixel changed.
 *   damage.all means: every pixel can have changed.
 * - The list never overflows: on overflow it holds one rect, its bounds.
 * - `presented` is true when the GPU presented the frame into
 *   framebuffer.drawable. It is always false for a CPU canvas. A CPU canvas
 *   is presented by the window, with `damage`.
 */
struct render_result {
    struct damage_list damage;
    bool presented;
};

/*
 * Creates a renderer. `opt` NULL means the defaults. The renderer owns its
 * worker threads, its cell cache, and its GPU device. Returns NULL when the
 * memory is missing. When the worker pool cannot start, the main thread
 * does all of the work. Call render_init() first.
 */
struct renderer *renderer_create(const struct render_options *opt);
/* Joins the workers and frees the renderer. NULL does nothing. */
void renderer_destroy(struct renderer *r);
/*
 * The GPU device for native surfaces, or NULL. The first call creates the
 * device. NULL means: give the window a CPU surface. The CPU-only build
 * (render/gpu/none.c) always gives NULL.
 */
void *renderer_device(struct renderer *r);
/*
 * Creates the GPU device now, not at the first renderer_device() call. It
 * does nothing when the options disable the GPU or when the renderer already
 * tried. Returns true if a device exists.
 */
bool renderer_start_gpu(struct renderer *r);
/* Releases the GPU device. Call it after every surface stops presenting
 * through the GPU. */
void renderer_drop_gpu(struct renderer *r);
/* Releases idle GPU resources when no GPU work is in flight. Returns true
 * when nothing is in flight, also without a renderer or a GPU; a false
 * result says that the caller tries again later. */
bool renderer_idle_gpu(struct renderer *r);
/* The enum render_isa level that the renderer uses. Never RENDER_ISA_AUTO. */
int renderer_isa(const struct renderer *r);

/*
 * Starts and stops the font system and builds the sRGB tables. The calls
 * nest. Main thread only. The thread pool is not here: each renderer owns
 * its own pool.
 */
bool render_init(void);
void render_shutdown(void);

#endif
