#ifndef RENDER_GPU_GPU_H
#define RENDER_GPU_GPU_H

/*
 * The GPU backend interface. render.c is the only caller.
 *
 * Milestone 4 builds only gpu/none.c on every system. Milestone 6 adds
 * gpu/metal/ behind the same calls.
 *
 * Rules for a backend
 * - The GPU draws a whole frame or nothing. It decides before it changes
 *   any state: it checks the atlas capacity first, and it delays refcount
 *   drops until commit.
 * - On a failure it marks its own grid fully dirty and returns
 *   RENDER_GPU_UNUSED. The CPU then paints the frame and
 *   render_gpu_present() shows the pixels.
 * - With no free frame slot it returns RENDER_GPU_DROPPED: nothing drawn,
 *   nothing presented, no state changed. It does not wait for a slot, so the
 *   main thread keeps its time for input and PTY reads.
 * - It asks for the drawable of the frame after it builds the frame data, so
 *   the drawable is held for the shortest time. `framebuffer.drawable` holds
 *   the drawable that it acquired.
 * - Its per-surface state lives in render_surface.gpu. It can free that
 *   state without the renderer, with render_gpu_surface_free().
 * - "Presented" in this interface means that the GPU took the frame. It does
 *   not mean that a display showed the frame. A display reports presentation
 *   later, through the presented handler of the drawable. The window counts
 *   that report. See docs/high-refresh.md.
 *
 * What render.c does with the result
 * - RENDER_GPU_PRESENTED: it invalidates the CPU surface state, because the
 *   CPU canvas did not get this frame. It reports full damage and
 *   presented = true.
 * - RENDER_GPU_DROPPED: it reports no damage and presented = false. The
 *   window marks the next frame fresh.
 * - RENDER_GPU_UNUSED: the CPU paints. With a drawable, render_gpu_present()
 *   then presents the CPU pixels, and its result is `presented`.
 *
 * none.c semantics
 * - render_gpu_new() returns NULL. render_gpu_native_device() returns NULL.
 *   So the window makes CPU surfaces, and no frame has a drawable.
 * - Every other call accepts NULL. render_gpu_draw() returns
 *   RENDER_GPU_UNUSED. render_gpu_present() returns false.
 *   render_gpu_surface_free() does nothing.
 *
 * Threads: main thread only.
 */

#include <stdbool.h>

struct damage_list;
struct framebuffer;
struct render_frame;
struct render_gpu;
struct render_surface;

enum render_gpu_status {
    RENDER_GPU_UNUSED = 0,
    RENDER_GPU_PRESENTED,
    RENDER_GPU_DROPPED,
};

/* The GPU of the renderer, or NULL when there is none. */
struct render_gpu *render_gpu_new(void);
void render_gpu_free(struct render_gpu *g);
/* The native device for the window (an id<MTLDevice>), or NULL. */
void *render_gpu_native_device(struct render_gpu *g);

/*
 * Draws and presents the whole frame into fb->drawable, or does nothing.
 * render.c calls it only with a drawable and without cpu_only.
 */
enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame);

/*
 * Presents the CPU pixels of fb into fb->drawable. It uploads only the
 * damage rects into a persistent texture. Full damage uploads everything.
 * Returns true when the GPU took the frame. It does not report that a display
 * showed the frame.
 */
bool render_gpu_present(struct render_gpu *g, struct render_surface *s, struct framebuffer *fb,
                        const struct damage_list *damage);

/* Frees render_surface.gpu. NULL does nothing. */
void render_gpu_surface_free(void *gpu_surface);

#endif
