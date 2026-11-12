#ifndef RENDER_GPU_VULKAN_INTERNAL_H
#define RENDER_GPU_VULKAN_INTERNAL_H

/*
 * The Vulkan backend. See render/gpu/gpu.h for the rules of a backend, and
 * render/gpu/build.h for the frame data that it draws.
 *
 * - device.c makes the instance, the device, and the pipelines. It also
 *   holds the texture and buffer helpers.
 * - swapchain.c holds the state of one surface: its swapchain and the
 *   texture of its CPU pixels.
 * - frame.c moves one frame to the GPU, draws it, and presents it.
 *
 * Requirements. The device has Vulkan 1.3. The backend uses two features
 * of that version: synchronization2 and dynamic rendering. So it has no
 * render pass objects and no framebuffer objects.
 *
 * Frames. One frame is one command buffer. The frame owns a slot of three:
 * its instance buffer, its upload buffer, and a fence that the queue
 * signals when the frame is done. All frames go to one queue, in order.
 *
 * Textures. Each texture has its own descriptor set with one binding. A
 * draw binds the set of the texture that it samples: the atlas, one UI
 * image, or the CPU pixels of a surface.
 *
 * Destroying. An object that a submitted frame can still use is destroyed
 * only after vulkan_wait_idle(). This happens on rare events only: a
 * texture changes size, or a swapchain is replaced twice in a row.
 *
 * Blending. All pipelines write premultiplied color and blend in the space
 * of the swapchain format. Subpixel glyphs draw in three passes, one for
 * each color write mask.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "paint/vulkan.h"
#include "render/gpu/build.h"
#include "util/geom.h"

#define FRAME_SLOTS 3
#define SWAPCHAIN_IMAGES_MAX 16
/* The texel format of every texture, and the swapchain format that the
 * backend prefers. It matches the 0xAARRGGBB words of the CPU. */
#define TEXEL_FORMAT VK_FORMAT_B8G8R8A8_UNORM

/* A sampled image with its view and its descriptor set. All zero: no texture. */
struct vulkan_texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    VkImageLayout layout;
    struct vector2i size;
};

/* A buffer that the CPU writes through `mapped`. All zero: no buffer. */
struct vulkan_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    size_t cap;
};

struct frame_slot {
    /* Signaled when the GPU does not use the slot. */
    VkFence fence;
    /* The swapchain signals it when the image of the frame is ready. */
    VkSemaphore acquired;
    VkCommandBuffer cmd;
    struct vulkan_buffer instances;
    struct vulkan_buffer uploads;
};

enum pipeline_id {
    PIPE_GLYPH,
    /* The three subpixel passes: red, green, blue. */
    PIPE_SUBPX,
    PIPE_SHAPE = PIPE_SUBPX + 3,
    /* Copies the CPU pixels of a surface. No blending. */
    PIPE_COPY,
    PIPE_COUNT,
};

struct push_constants {
    float viewport_size[2];
    uint32_t channel;
};

struct gpu_surface;
struct render_surface;

struct render_gpu {
    /* The handles that the window layer reads. See paint/vulkan.h. */
    struct vulkan_device dev;

    VkCommandPool cmd_pool;
    VkDescriptorPool desc_pool;
    VkDescriptorSetLayout desc_layout;
    VkPipelineLayout pipeline_layout;
    VkPipelineCache pipeline_cache;
    VkSampler sampler;
    /* The color format that the pipelines draw to. */
    VkFormat pipeline_format;
    VkPipeline pipelines[PIPE_COUNT];

    struct frame_slot slots[FRAME_SLOTS];
    unsigned next_slot;

    struct gpu_build build;
    struct vulkan_texture atlas;
    struct vulkan_texture images[GPU_IMAGES];
    /* Scratch of one frame: the copy regions of the atlas, and where the
     * pixels of each image wait in the upload buffer. */
    VkBufferImageCopy *copies;
    uint32_t cap_copies;
    size_t image_offsets[GPU_IMAGES];

    /* Every surface that holds Vulkan objects of this device. */
    struct gpu_surface *surfaces;
};

/* The swapchain of a surface with the objects of its images. */
struct vulkan_swapchain {
    VkSwapchainKHR handle;
    uint32_t image_count;
    VkImage images[SWAPCHAIN_IMAGES_MAX];
    VkImageView views[SWAPCHAIN_IMAGES_MAX];
    /* The queue signals rendered[i] when image i can be presented. */
    VkSemaphore rendered[SWAPCHAIN_IMAGES_MAX];
};

/* The state of one render surface. It lives in render_surface.gpu. */
struct gpu_surface {
    /* NULL after the renderer is gone. Then the surface holds no objects. */
    struct render_gpu *gpu;
    struct gpu_surface *next;

    VkSurfaceKHR surface;
    struct vulkan_swapchain chain;
    /* The swapchain before `chain`. Submitted frames can still use it. */
    struct vulkan_swapchain retired;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR present_mode;
    /* The size and the pacing that the swapchain was last made or checked for. */
    struct vector2i request_size;
    bool request_tearing;
    /* The driver reported that the swapchain no longer matches the surface. */
    bool recheck;
    /* The swapchain cannot be used anymore and must be replaced. */
    bool out_of_date;
    /* The answers of the surface queries. They are valid for `surface` only. */
    struct {
        VkSurfaceKHR surface;
        VkSurfaceFormatKHR format;
        bool immediate, mailbox, fifo_relaxed;
    } traits;

    /* The CPU pixels of the surface, for render_gpu_present(). */
    struct vulkan_texture pixels;
};

/* ---- device.c ----------------------------------------------------------- */

/* Waits until the queue completes every submitted frame. */
void vulkan_wait_idle(struct render_gpu *g);

/*
 * Makes `t` a texture of `size`. A texture of that size stays as it is. A
 * texture of another size is replaced after vulkan_wait_idle(), and its
 * pixels are lost. On failure `t` holds no image.
 */
bool vulkan_texture_ensure(struct render_gpu *g, struct vulkan_texture *t, struct vector2i size);
/* Destroys the texture now. The caller makes sure that no frame uses it. */
void vulkan_texture_destroy(struct render_gpu *g, struct vulkan_texture *t);

/* Makes room for `bytes`. A buffer that grows loses its content. The GPU
 * must not use the buffer: it belongs to a free frame slot. */
bool vulkan_buffer_ensure(struct render_gpu *g, struct vulkan_buffer *b, size_t bytes,
                          VkBufferUsageFlags usage);
void vulkan_buffer_destroy(struct render_gpu *g, struct vulkan_buffer *b);

/* Makes the pipelines draw to `format`. It waits for the queue when it
 * must replace pipelines. */
bool vulkan_pipelines_ensure(struct render_gpu *g, VkFormat format);

/* ---- swapchain.c -------------------------------------------------------- */

/* The surface state of `s`. It makes the state on first use. NULL: no memory. */
struct gpu_surface *vulkan_surface_of(struct render_gpu *g, struct render_surface *s);
/* Destroys the Vulkan objects of the surface and takes it from its renderer.
 * It waits for the queue first. */
void vulkan_surface_release(struct gpu_surface *gs);

/*
 * Makes the swapchain of the surface fit `size` and the pacing. `tearing`
 * asks for a present mode that does not wait for the display. `force`
 * replaces the swapchain in any case.
 */
bool vulkan_swapchain_ensure(struct render_gpu *g, struct gpu_surface *gs, VkSurfaceKHR surface,
                             struct vector2i size, bool tearing, bool force);
/*
 * Takes the next image of the swapchain for the frame of `slot`. It waits
 * for `wait_ns` at most. It replaces a swapchain that is out of date.
 * False: no image, and the frame is dropped.
 */
bool vulkan_swapchain_acquire(struct render_gpu *g, struct gpu_surface *gs,
                              struct frame_slot *slot, VkSurfaceKHR surface,
                              struct vector2i size, bool tearing, uint64_t wait_ns,
                              uint32_t *image_index);

/* ---- frame.c ------------------------------------------------------------ */

/* Gives the slot a new semaphore. Use it when the old one can hold a signal
 * that no submit waits for. */
bool vulkan_slot_renew_semaphore(struct render_gpu *g, struct frame_slot *slot);

#endif
