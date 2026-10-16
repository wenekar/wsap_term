#ifndef WSAP_DARWIN_METAL_RENDER_H
#define WSAP_DARWIN_METAL_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include "render/backend.h"
#include "render/term_frame.h"
#include "paint/scene.h"
#include "paint/framebuffer.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
typedef id<MTLDevice> mtl_device_t;
typedef id<CAMetalDrawable> mtl_drawable_t;
typedef MTLRenderPassDescriptor *mtl_pass_desc_t;
#else
typedef void *mtl_device_t;
typedef void *mtl_drawable_t;
typedef void *mtl_pass_desc_t;
#endif

struct metal_render;

/* Shared backend state. Render resets the frame flags before each draw. */
struct framebuffer_gpu {
    struct metal_render *metal;
    struct framebuffer_history *history;
    /* What an empty frame shows under the UI. */
    uint32_t clear_color;
    /* The GPU drew into this frame; the pixels need no upload. */
    bool presented;
    /* The GPU gave up this frame; the rest of it goes to the CPU. */
    bool disabled;
    /* It gave up after it drew, so the CPU must draw the whole frame. */
    bool redraw;
};

struct metal_render *metal_render_create(mtl_device_t device);
void metal_render_destroy(struct metal_render *r);

enum render_backend_result metal_render_terminal(
    struct metal_render *r, struct framebuffer_history *history,
    const struct terminal_frame *frame,
    const struct render_backend_view *view);

void metal_render_begin(struct metal_render *r);

void metal_render_clear(struct metal_render *r, uint32_t color);

bool metal_render_scene(struct metal_render *r, const struct ui_scene *scene,
                        struct font_instance *font);

/* Draws a CPU frame of XRGB pixels over the viewport. The pixels must
 * stay valid until metal_render_submit() returns. */
bool metal_render_pixels(struct metal_render *r, const uint32_t *pixels,
                         struct vector2i size);

/* A NULL drawable submits an offscreen frame and waits for completion.
 * With transaction, the drawable presents in the current Core Animation
 * transaction; the layer must have presentsWithTransaction set. */
bool metal_render_submit(struct metal_render *r, mtl_drawable_t drawable,
                         mtl_pass_desc_t pass_desc, struct vector2i viewport,
                         bool transaction);

#endif
