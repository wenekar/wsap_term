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
 * Requirements. The instance uses Vulkan 1.3, and the physical device must
 * support that version. The device enables two Vulkan 1.3 features,
 * synchronization2 and dynamic rendering, so the backend has no render pass
 * objects and no framebuffer objects. It also needs the VK_KHR_swapchain
 * device extension.
 *
 * Frames. One frame is one command buffer. Each frame takes one of three
 * slots. A slot holds its command buffer, its instance and upload buffers,
 * a fence that the queue signals when the frame is done, and the semaphore
 * that the image acquire signals. All frames go to one queue, in order.
 *
 * Textures. Each texture has its own descriptor set with one binding. A
 * draw binds the set of the texture that it samples: the atlas, one UI
 * image, or the CPU pixels of a surface. The sets come from a descriptor
 * pool; when a pool is full, another pool with twice its sets is added, so
 * the number of textures is not bounded by the first pool.
 *
 * Destroying. An object that a submitted frame can still use is destroyed
 * only after the queue is idle: vulkan_wait_idle() waits for the queue, and
 * render_gpu_free() uses vkDeviceWaitIdle(). A texture that changes size, a
 * surface that is released, and a swapchain that is replaced twice in a row
 * take this path.
 *
 * Blending. A glyph draw writes premultiplied color and blends with the
 * rule of its blend mode. A BLEND_NONE draw writes the straight color of a
 * solid or color glyph, and its pipeline disables blending. A shape draw
 * writes premultiplied color and uses the PREMUL blend state. A COPY draw
 * has no blend state. Subpixel glyphs draw in three passes, one for each
 * color write mask. One glyph pipeline exists for each blend mode, and its
 * color blend state gives the same expression as blend_mode_px() in
 * render/cpu/scene.c; a GPU_CMD_GLYPH command uses the pipeline of the
 * blend mode of its first instance.
 */

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "paint/vulkan.h"
#include "render/gpu/build.h"
#include "util/geom.h"

#define FRAME_SLOTS 3
#define SWAPCHAIN_IMAGES_MAX 16
/* The texel format of every texture, and the only swapchain format that the
 * backend accepts. It matches the 0xAARRGGBB words of the CPU. */
#define TEXEL_FORMAT VK_FORMAT_B8G8R8A8_UNORM

/* A sampled image with its view and its descriptor set. All zero: no texture.
 * `pool` owns `set`. */
struct vulkan_texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    VkDescriptorPool pool;
    /* The layout that the backend has recorded for `image`; see
     * barriers_add() in frame.c. */
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
    /* Signaled when the GPU has completed the submissions of the slot. */
    VkFence fence;
    /* Signaled by the image acquire; the frame submit waits for it. */
    VkSemaphore acquired;
    VkCommandBuffer cmd;
    struct vulkan_buffer instances;
    struct vulkan_buffer uploads;
};

enum pipeline_id {
    /* The first of the glyph pipelines, one for each blend mode. */
    PIPE_GLYPH,
    /* The three subpixel passes: red, green, blue. They use the PREMUL
     * blend state only, because a text op ignores its blend mode. */
    PIPE_SUBPX = PIPE_GLYPH + GPU_BLEND_COUNT,
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

struct vulkan_fns {
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkQueueSubmit2 QueueSubmit2;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets FreeDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCreatePipelineCache CreatePipelineCache;
    PFN_vkDestroyPipelineCache DestroyPipelineCache;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdBeginRendering CmdBeginRendering;
    PFN_vkCmdEndRendering CmdEndRendering;
    PFN_vkCmdSetViewport CmdSetViewport;
    PFN_vkCmdSetScissor CmdSetScissor;
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkGetFenceStatus GetFenceStatus;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
};

/* A descriptor pool and the number of sets that it can give. */
struct vulkan_desc_pool {
    VkDescriptorPool pool;
    uint32_t sets;
};

struct render_gpu {
    /* The handles that the window layer reads. See paint/vulkan.h. */
    struct vulkan_device dev;
    void *vk_lib;
    struct vulkan_fns vk;

    VkCommandPool cmd_pool;
    /* One set for each texture. A full pool adds a pool with twice its
     * sets. */
    struct vulkan_desc_pool *desc_pools;
    VkDescriptorSetLayout desc_layout;
    VkPipelineLayout pipeline_layout;
    VkPipelineCache pipeline_cache;
    VkSampler sampler;
    /* The color format that the pipelines draw to. */
    VkFormat pipeline_format;
    unsigned next_slot;
    uint32_t n_desc_pools;
    uint32_t cap_desc_pools;
    VkPipeline pipelines[PIPE_COUNT];

    struct frame_slot slots[FRAME_SLOTS];

    struct gpu_build build;
    struct vulkan_texture atlas;
    /* One texture for each image slot of the builder. */
    struct vulkan_texture *images;
    /* Scratch of one frame: the copy regions of the atlas, and the offset
     * of each image in the upload buffer. */
    VkBufferImageCopy *copies;
    size_t *image_offsets;
    int n_images;

    /* Every surface that holds Vulkan objects of this device. */
    struct gpu_surface *surfaces;
    uint32_t cap_copies;
};

/* A swapchain with the view and the present semaphore of each image. */
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
    /* The renderer that owns the objects of this surface, or NULL when the
     * surface holds none. */
    struct render_gpu *gpu;
    struct gpu_surface *next;

    VkSurfaceKHR surface;
    struct vulkan_swapchain chain;
    /* The swapchain before `chain`. Submitted frames can still use it. */
    struct vulkan_swapchain retired;
    VkFormat format;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;
    /* The swapchain extent that the last check recorded. */
    struct vector2i request_size;
    /* The cached surface queries. The cache is valid for `traits.surface`
     * only. */
    struct {
        VkSurfaceKHR surface;
        VkSurfaceFormatKHR format;
    } traits;
    bool trait_immediate;
    bool trait_mailbox;
    bool trait_fifo_relaxed;
    /* The pacing that the last check recorded. */
    bool request_tearing;
    /* The next check must query the surface again. */
    bool recheck;
    /* The swapchain cannot be used anymore and must be replaced. */
    bool out_of_date;

    /* The CPU pixels of the surface, for render_gpu_present(). */
    struct vulkan_texture pixels;
};

/* ---- device.c ----------------------------------------------------------- */

/* Waits until the queue completes every submitted frame. */
void vulkan_wait_idle(struct render_gpu *g);

/*
 * Ensures that texture `t` has an image of `size`.
 * A resize waits for the queue to go idle before it destroys the old image.
 * Returns false on allocation failure; `t` then holds no image, but it keeps
 * its descriptor set.
 */
bool vulkan_texture_ensure(struct render_gpu *g, struct vulkan_texture *t, struct vector2i size);
/* Destroys the image, the view, the memory, and the descriptor set of `t`,
 * and resets `t`. The caller must ensure that the queue is idle. */
void vulkan_texture_destroy(struct render_gpu *g, struct vulkan_texture *t);

/*
 * Ensures that buffer `b` has at least `bytes` of capacity, mapped for CPU
 * writes and coherent with device reads. A new size discards the contents.
 * Returns false on allocation failure; `b` then holds no buffer.
 */
bool vulkan_buffer_ensure(struct render_gpu *g, struct vulkan_buffer *b, size_t bytes,
                          VkBufferUsageFlags usage);
/* Unmaps and destroys the buffer and its memory, and resets `b`. The caller
 * must ensure that no submitted frame uses `b`. */
void vulkan_buffer_destroy(struct render_gpu *g, struct vulkan_buffer *b);

/*
 * Ensures that the graphics pipelines exist for `format`. A replacement of
 * live pipelines waits for the queue to go idle first.
 * Returns false when the device rejects a shader module or a pipeline; the
 * backend then has no pipelines, and a later call builds them again.
 */
bool vulkan_pipelines_ensure(struct render_gpu *g, VkFormat format);

/* ---- swapchain.c -------------------------------------------------------- */

/* Returns the Vulkan state of surface `s` for renderer `g`, and creates it on
 * the first use. State that an earlier renderer made is released first.
 * Returns NULL on allocation failure. */
struct gpu_surface *vulkan_surface_of(struct render_gpu *g, struct render_surface *s);
/*
 * Waits for the queue to go idle and destroys the Vulkan objects of `gs`.
 * Removes `gs` from the surface list of the renderer and clears `gs`, so a
 * second call does nothing.
 */
void vulkan_surface_release(struct gpu_surface *gs);

/*
 * Ensures that the swapchain of `gs` can present `size` at the pacing of
 * `tearing` on `surface`. It creates a swapchain when the surface, the
 * extent, or the present mode changed, when the old one is out of date, or
 * when `force` is set.
 * Returns false when the backend cannot make a swapchain for the surface;
 * the frame is then dropped. On success it records the extent and the
 * pacing, so a later frame can skip the surface queries.
 */
bool vulkan_swapchain_ensure(struct render_gpu *g, struct gpu_surface *gs, VkSurfaceKHR surface,
                             struct vector2i size, bool tearing, bool force);
/*
 * Acquires an image of the swapchain of `gs` for `slot` and writes its index
 * to `image_index`. An acquire waits at most `wait_ns` nanoseconds for an
 * image.
 * On VK_SUBOPTIMAL_KHR it checks the surface and keeps the acquired image
 * when the swapchain stays. An out-of-date swapchain is replaced once, and
 * the acquire then repeats.
 * Returns false when no image becomes available, or when there is no usable
 * swapchain.
 */
bool vulkan_swapchain_acquire(struct render_gpu *g, struct gpu_surface *gs,
                              struct frame_slot *slot, VkSurfaceKHR surface,
                              struct vector2i size, bool tearing, uint64_t wait_ns,
                              uint32_t *image_index);

/* ---- frame.c ------------------------------------------------------------ */

/* Replaces the image-available semaphore of `slot` with a new unsignaled one.
 * The caller must ensure that no submitted frame still waits on the old
 * semaphore.
 * Returns false when the device makes no semaphore; `slot->acquired` is then
 * VK_NULL_HANDLE. */
bool vulkan_slot_renew_semaphore(struct render_gpu *g, struct frame_slot *slot);

#endif
