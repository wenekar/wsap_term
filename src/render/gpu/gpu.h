#ifndef RENDER_GPU_GPU_H
#define RENDER_GPU_GPU_H

/*
 * The GPU backend interface used by the renderer.
 *
 * One backend is in a build:
 * - gpu/metal/ on macOS.
 * - gpu/vulkan/ on Linux, when the build has Vulkan 1.3 and glslc.
 * - gpu/none.c in all other builds. It has no GPU.
 *
 * gpu/build.c is the CPU half of a GPU backend. It turns a render_frame
 * into instance lists, draw commands, and atlas uploads. A backend that
 * uses it holds only the code that talks to its GPU API. The Metal and
 * Vulkan backends use it.
 *
 * WSAP_RENDER_GPU selects the GPU at run time. "0" or "off" gives no GPU.
 * "software" lets the Vulkan backend use a device that runs on the CPU,
 * such as lavapipe. This value is for tests.
 *
 * Rules for a backend
 * - The GPU draws a whole frame or nothing. It decides before it changes
 *   any state: it checks the atlas capacity first, and it delays refcount
 *   drops until commit.
 * - On a failure it marks its own grid fully dirty and returns
 *   RENDER_GPU_UNUSED. The CPU then paints the frame and
 *   render_gpu_present() shows the pixels.
 * - With no free frame slot it returns RENDER_GPU_DROPPED: nothing drawn,
 *   nothing presented, no state changed. It does not wait for the GPU or
 *   for the display without a limit, so the main thread keeps its time for
 *   input and PTY reads. A backend can wait a few milliseconds when a
 *   dropped frame costs more, as in a resize. It states each such wait.
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

/* Creates and initializes a GPU backend context. Returns NULL when unavailable. */
struct render_gpu *render_gpu_new(void);
/* Waits for idle GPU execution and frees all backend resources. */
void render_gpu_free(struct render_gpu *g);
/* Returns a pointer to the platform native device handle, or NULL when disabled. */
void *render_gpu_native_device(struct render_gpu *g);

/*
 * Builds, uploads, and draws `frame` to the target surface of `fb`.
 * Returns RENDER_GPU_PRESENTED on successful submission,
 * RENDER_GPU_DROPPED when slots are busy, or RENDER_GPU_UNUSED on fallback.
 */
enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame);

/*
 * Uploads damaged CPU framebuffer pixels and submits the surface quad for presentation.
 * Returns true on submission success, or false on error or missing drawable.
 */
bool render_gpu_present(struct render_gpu *g, struct render_surface *s, struct framebuffer *fb,
                        const struct damage_list *damage);

/* Frees surface GPU state allocated in `gpu_surface`. Ignores NULL pointers. */
void render_gpu_surface_free(void *gpu_surface);

#endif
