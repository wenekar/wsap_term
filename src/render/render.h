#ifndef RENDER_H
#define RENDER_H

/*
 * The public renderer API.
 *
 * The app owns one renderer. Only the app calls render_draw(). The test
 * oracle for a full paint is a second renderer that draws a frame with no
 * history.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paint/framebuffer.h"
#include "render/term_frame.h"
#include "util/damage.h"
#include "util/geom.h"

struct font_instance;
struct renderer;
struct ui_damage;
struct ui_scene;

/* An explicit "off" value for the counts in struct render_options. */
#define RENDER_NONE (-1)

/* The cell cache budget that a zero cache_bytes gives. */
#define RENDER_CACHE_DEFAULT_BYTES ((ptrdiff_t)16 << 20)

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
 *
 * The settings never change pixels. They change only speed and memory.
 */
struct render_options {
    int isa;
    int threads;
    ptrdiff_t cache_bytes;
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
 * - The rest of the rect is margin. The margin is frame->default_bg, also
 *   in reverse video, as today.
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
    uint32_t base;
    struct render_grid grid;
    const struct ui_scene *ui;
    struct font_instance *ui_font;
    const struct ui_damage *ui_damage;
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
 * memory is missing. A worker thread that fails to start is left out.
 * Call render_init() first.
 */
struct renderer *renderer_create(const struct render_options *opt);
/* Joins the workers and frees the renderer. NULL does nothing. */
void renderer_destroy(struct renderer *r);
/*
 * The GPU device for native surfaces, or NULL. NULL means: give the window
 * a CPU surface. The CPU-only build (render/gpu/none.c) always gives NULL.
 */
void *renderer_device(struct renderer *r);
/* The enum render_isa level that the renderer uses. Never RENDER_ISA_AUTO. */
int renderer_isa(const struct renderer *r);

/*
 * Draws one frame into `fb`.
 *
 * - Main thread only. One call at a time for each renderer.
 * - With fb->history, the renderer repaints only what changed since the
 *   last frame of that history. The pixels are always the same as a fresh
 *   paint of the same frame.
 * - Without a history, it paints every pixel and keeps no state. It does
 *   not allocate for that case.
 * - The environment value WSAP_RENDER_FULL=1 makes every frame a fresh
 *   paint. It is a debug knob.
 * - The environment value WSAP_RENDER_SCROLL=move|paint|auto picks how a
 *   scroll is drawn: move the old rows, or paint them again. The default
 *   is auto: the renderer picks the faster one from measured cost. The
 *   renderer reads it once, at creation. Pixels and damage are the same
 *   for all three. It is a test and debug knob.
 * - NULL r, fb, or frame returns a zero result and draws nothing.
 */
struct render_result render_draw(struct renderer *r, struct framebuffer *fb,
                                 const struct render_frame *frame);

/*
 * Starts and stops the font system and builds the sRGB tables. The calls
 * nest. Main thread only. The thread pool is not here: each renderer owns
 * its own pool.
 */
bool render_init(void);
void render_shutdown(void);

#endif
