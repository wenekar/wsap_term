#include "render/gpu/gpu.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "font/font.h"
#include "font/glyph_cache.h"
#include "paint/framebuffer.h"
#include "paint/scene.h"
#include "paint/shadow.h"
#include "paint/text_run.h"
#include "paint/vulkan.h"
#include "render/cpu/decor.h"
#include "render/frame/walk.h"
#include "render/render.h"
#include "render/surface.h"
#include "util/damage.h"
#include "util/geom.h"
#include "util/hash.h"

#include "vulkan_shaders_spv.h"

/*
 * The Vulkan rendering backend.
 * One command buffer draws a whole frame.
 * The CPU builds instance lists, then one render pass draws them:
 *   1. the grid area in default_bg, and cell backgrounds that differ;
 *   2. grid glyphs, clipped to cell bounds;
 *   3. decorations and cursor marks;
 *   4. UI scene operations in scene order.
 */

#define FRAME_SLOTS 3
#define RESIZE_FENCE_WAIT_NS 20000000ull
#define ATLAS_START_SIZE 1024
#define ATLAS_MAX_SIZE 4096
#define ATLAS_MAP_SIZE (1u << 15)
#define ATLAS_MAP_LIMIT (ATLAS_MAP_SIZE / 2)
#define SHELVES_MAX 1024
#define IMAGE_CACHE 4
#define STREAM_ALIGN 256
#define BUFFER_MIN 65536
#define SWAPCHAIN_IMAGES_MAX 8

enum glyph_mode {
    MODE_SOLID = 0,
    MODE_ALPHA = 1,
    MODE_COLOR = 2,
    MODE_IMAGE = 3,
    MODE_SUBPX = 4,
};

enum shape_mode {
    SHAPE_ROUNDED = 1,
    SHAPE_CIRCLE = 2,
    SHAPE_SHADOW = 3,
};

struct glyph_inst {
    float rect[4];
    float uv[4];
    float color[4];
    uint32_t mode;
    uint32_t pad[3];
};

struct shape_inst {
    float rect[4];
    float color[4];
    float radius;
    uint32_t mode;
    uint32_t blur;
    uint32_t pad;
};

struct push_constants {
    float viewport_size[2];
    uint32_t channel;
};

_Static_assert(sizeof(struct glyph_inst) == 64, "glyph_inst is 64 bytes");
_Static_assert(sizeof(struct shape_inst) == 48, "shape_inst is 48 bytes");

enum stream_id {
    ST_BG,
    ST_INK,
    ST_SUBPX,
    ST_DECO,
    ST_UI,
    ST_SHAPE,
    ST_COUNT,
};

static const size_t STREAM_STRIDE[ST_COUNT] = {
    sizeof(struct glyph_inst), sizeof(struct glyph_inst), sizeof(struct glyph_inst),
    sizeof(struct glyph_inst), sizeof(struct glyph_inst), sizeof(struct shape_inst),
};

struct stream {
    uint8_t *data;
    uint32_t n;
    uint32_t cap;
};

enum cmd_kind {
    CMD_GLYPH,
    CMD_SUBPX,
    CMD_SHAPE,
};

struct cmd {
    enum cmd_kind kind;
    enum stream_id stream;
    struct rect2i scissor;
    uint32_t start;
    uint32_t count;
    VkDescriptorSet desc_set;
};

enum atlas_space {
    SPACE_GRID,
    SPACE_UI,
    SPACE_DECOR,
};

enum glyph_kind {
    KIND_NONE,
    KIND_ALPHA,
    KIND_SUBPX,
    KIND_COLOR,
};

struct atlas_entry {
    uint64_t id;
    uint64_t version;
    uint32_t space;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t rows;
    int16_t x_offset;
    uint8_t kind;
    bool used;
};

struct shelf {
    uint16_t y;
    uint16_t h;
    uint16_t x;
};

struct upload {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    size_t offset;
};

struct atlas {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet desc_set;
    struct atlas_entry *map;
    uint8_t *stage;
    size_t stage_used;
    size_t stage_cap;
    struct upload *uploads;
    VkBufferImageCopy *vk_copies;
    VkImageLayout layout;
    int size;
    uint32_t count;
    int n_shelves;
    int next_y;
    uint32_t n_uploads;
    uint32_t cap_uploads;
    struct shelf shelves[SHELVES_MAX];
    bool full;
};

struct frame_slot {
    VkFence fence;
    VkSemaphore image_acquired;
    VkCommandBuffer cmd;
    VkBuffer instance_buffer;
    VkDeviceMemory instance_memory;
    void *instance_mapped;
    size_t instance_cap;
    VkBuffer upload_buffer;
    VkDeviceMemory upload_memory;
    void *upload_mapped;
    size_t upload_cap;
};

struct image_entry {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet desc_set;
    const uint32_t *pixels;
    uint64_t gen;
    struct vector2i size;
    uint64_t used;
    size_t upload_offset;
    bool needs_upload;
};

struct render_gpu {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkFormat render_pass_format;
    struct vulkan_device device_info;

    PFN_vkQueueSubmit2 pfn_vkQueueSubmit2;
    PFN_vkCmdPipelineBarrier2 pfn_vkCmdPipelineBarrier2;

    VkCommandPool cmd_pool;
    VkDescriptorPool desc_pool;
    VkDescriptorSetLayout desc_layout;
    VkPipelineLayout pipeline_layout;
    VkSampler sampler;

    VkRenderPass render_pass;
    VkRenderPass render_pass_copy;
    VkPipelineCache pipeline_cache;

    VkPipeline glyph_pipeline;
    VkPipeline subpx_pipeline[3];
    VkPipeline shape_pipeline;
    VkPipeline copy_pipeline;

    VkImage dummy_image;
    VkDeviceMemory dummy_memory;
    VkImageView dummy_view;

    struct frame_slot slots[FRAME_SLOTS];
    unsigned next_slot;
    int cap_clips;

    struct atlas atlas;
    struct decor_cache *decor;
    struct image_entry images[IMAGE_CACHE];
    uint64_t image_clock;

    struct stream streams[ST_COUNT];
    size_t stream_offsets[ST_COUNT];
    struct cmd *cmds;
    struct rect2i *clips;
    uint32_t n_cmds;
    uint32_t cap_cmds;
    bool failed;
};

struct gpu_surface {
    VkDevice device;
    VkDescriptorPool desc_pool;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR present_mode;
    uint32_t image_count;
    VkImageLayout pixels_layout;
    /* The size and the pacing that the swapchain was last made or checked for. */
    struct vector2i request_size;
    bool request_tearing;
    /* The driver reported that the swapchain no longer matches the surface. */
    bool recheck;
    /* The swapchain cannot be used anymore and must be recreated. */
    bool out_of_date;
    /* The answers of the surface queries. They are valid for `surface` only. */
    struct {
        VkSurfaceKHR surface;
        VkSurfaceFormatKHR format;
        bool immediate, mailbox, fifo_relaxed;
    } traits;
    VkImage images[SWAPCHAIN_IMAGES_MAX];
    VkImageView views[SWAPCHAIN_IMAGES_MAX];
    VkFramebuffer framebuffers[SWAPCHAIN_IMAGES_MAX];
    VkSemaphore render_finished[SWAPCHAIN_IMAGES_MAX];

    struct {
        VkSwapchainKHR swapchain;
        uint32_t image_count;
        VkImageView views[SWAPCHAIN_IMAGES_MAX];
        VkFramebuffer framebuffers[SWAPCHAIN_IMAGES_MAX];
        VkSemaphore render_finished[SWAPCHAIN_IMAGES_MAX];
    } retired;

    VkImage pixels_image;
    VkDeviceMemory pixels_memory;
    VkImageView pixels_view;
    VkDescriptorSet pixels_desc_set;
    struct vector2i pixels_size;
};

static inline void color_of(uint32_t argb, float opacity, float out[4])
{
    out[0] = ((argb >> 16) & 0xff) / 255.0f;
    out[1] = ((argb >> 8) & 0xff) / 255.0f;
    out[2] = (argb & 0xff) / 255.0f;
    out[3] = ((argb >> 24) & 0xff) / 255.0f * opacity;
}

static inline void opaque_color_of(uint32_t argb, float opacity, float out[4])
{
    color_of(argb | 0xFF000000u, opacity, out);
}

static inline void rect_to_vec4(float x, float y, float w, float h, float out[4])
{
    out[0] = x;
    out[1] = y;
    out[2] = w;
    out[3] = h;
}

static inline void rect2i_to_vec4(struct rect2i r, float out[4])
{
    rect_to_vec4((float)r.x, (float)r.y, (float)r.width, (float)r.height, out);
}

static inline int min_i(int a, int b) { return a < b ? a : b; }
static inline int max_i(int a, int b) { return a > b ? a : b; }

static inline size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

static uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t type_filter,
                                 VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

static bool ensure_slot_buffer(struct render_gpu *g, VkBuffer *buf, VkDeviceMemory *mem,
                               void **mapped, size_t *cap, size_t bytes,
                               VkBufferUsageFlags usage)
{
    if (*buf && *cap >= bytes)
        return true;
    size_t target_cap = *cap ? *cap : BUFFER_MIN;
    while (target_cap < bytes)
        target_cap *= 2;
    if (*buf) {
        if (*mapped) {
            vkUnmapMemory(g->device, *mem);
            *mapped = NULL;
        }
        vkDestroyBuffer(g->device, *buf, NULL);
        vkFreeMemory(g->device, *mem, NULL);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        *cap = 0;
    }
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = target_cap,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(g->device, &bi, NULL, buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(g->device, *buf, &reqs);
    uint32_t type_idx = find_memory_type(
        g->physical_device, reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type_idx == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type_idx,
    };
    if (vkAllocateMemory(g->device, &ai, NULL, mem) != VK_SUCCESS)
        return false;
    if (vkBindBufferMemory(g->device, *buf, *mem, 0) != VK_SUCCESS)
        return false;
    if (vkMapMemory(g->device, *mem, 0, target_cap, 0, mapped) != VK_SUCCESS)
        return false;
    *cap = target_cap;
    return true;
}

/* ---- Streams and commands ---------------------------------------------- */

static void *stream_push(struct render_gpu *g, enum stream_id id)
{
    struct stream *s = &g->streams[id];
    size_t stride = STREAM_STRIDE[id];
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 256;
        void *p = realloc(s->data, (size_t)cap * stride);
        if (!p) {
            g->failed = true;
            return NULL;
        }
        s->data = p;
        s->cap = cap;
    }
    void *e = s->data + (size_t)s->n++ * stride;
    memset(e, 0, stride);
    return e;
}

static struct glyph_inst *push_glyph(struct render_gpu *g, enum stream_id id)
{
    return stream_push(g, id);
}

static struct cmd *cmd_open(struct render_gpu *g, enum cmd_kind kind, enum stream_id stream,
                            struct rect2i scissor, VkDescriptorSet desc_set)
{
    uint32_t next = g->streams[stream].n;
    if (g->n_cmds) {
        struct cmd *last = &g->cmds[g->n_cmds - 1];
        if (last->kind == kind && last->stream == stream && last->desc_set == desc_set &&
            rect2i_eq(last->scissor, scissor) && last->start + last->count == next)
            return last;
    }
    if (g->n_cmds == g->cap_cmds) {
        uint32_t cap = g->cap_cmds ? g->cap_cmds * 2 : 64;
        struct cmd *p = realloc(g->cmds, (size_t)cap * sizeof(*p));
        if (!p) {
            g->failed = true;
            return NULL;
        }
        g->cmds = p;
        g->cap_cmds = cap;
    }
    struct cmd *c = &g->cmds[g->n_cmds++];
    *c = (struct cmd){
        .kind = kind, .stream = stream, .scissor = scissor, .start = next,
        .desc_set = desc_set,
    };
    return c;
}

static void cmd_whole_stream(struct render_gpu *g, enum cmd_kind kind, enum stream_id stream,
                             struct rect2i scissor)
{
    uint32_t n = g->streams[stream].n;
    if (!n)
        return;
    struct cmd *c = cmd_open(g, kind, stream, scissor, g->atlas.desc_set);
    if (!c)
        return;
    c->start = 0;
    c->count = n;
}

static void begin_build(struct render_gpu *g)
{
    for (int i = 0; i < ST_COUNT; i++)
        g->streams[i].n = 0;
    g->n_cmds = 0;
    g->atlas.stage_used = 0;
    g->atlas.n_uploads = 0;
    g->atlas.full = false;
    g->failed = false;
}

/* ---- Atlas management --------------------------------------------------- */

static bool atlas_make_texture(struct render_gpu *g, int size)
{
    if (g->atlas.image) {
        vkDeviceWaitIdle(g->device);
        vkDestroyImageView(g->device, g->atlas.view, NULL);
        vkDestroyImage(g->device, g->atlas.image, NULL);
        vkFreeMemory(g->device, g->atlas.memory, NULL);
        g->atlas.image = VK_NULL_HANDLE;
        g->atlas.memory = VK_NULL_HANDLE;
        g->atlas.view = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { (uint32_t)size, (uint32_t)size, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(g->device, &ii, NULL, &g->atlas.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(g->device, g->atlas.image, &reqs);
    uint32_t type_idx = find_memory_type(g->physical_device, reqs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_idx == UINT32_MAX)
        type_idx = find_memory_type(g->physical_device, reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type_idx == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type_idx,
    };
    if (vkAllocateMemory(g->device, &ai, NULL, &g->atlas.memory) != VK_SUCCESS)
        return false;
    if (vkBindImageMemory(g->device, g->atlas.image, g->atlas.memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = g->atlas.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(g->device, &vi, NULL, &g->atlas.view) != VK_SUCCESS)
        return false;

    if (!g->atlas.desc_set) {
        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &g->desc_layout,
        };
        if (vkAllocateDescriptorSets(g->device, &dsai, &g->atlas.desc_set) != VK_SUCCESS)
            return false;
    }
    VkDescriptorImageInfo image_info = {
        .sampler = g->sampler,
        .imageView = g->atlas.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkDescriptorImageInfo dummy_info = {
        .sampler = g->sampler,
        .imageView = g->dummy_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = g->atlas.desc_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = g->atlas.desc_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dummy_info,
        },
    };
    vkUpdateDescriptorSets(g->device, 2, writes, 0, NULL);
    g->atlas.size = size;
    g->atlas.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

static void atlas_clear(struct atlas *a)
{
    memset(a->map, 0, (size_t)ATLAS_MAP_SIZE * sizeof(*a->map));
    a->count = 0;
    a->n_shelves = 0;
    a->next_y = 0;
    a->stage_used = 0;
    a->n_uploads = 0;
    a->full = false;
}

static void atlas_reset(struct render_gpu *g)
{
    atlas_clear(&g->atlas);
}

static bool atlas_recycle(struct render_gpu *g)
{
    struct atlas *a = &g->atlas;
    if (a->size < ATLAS_MAX_SIZE && !atlas_make_texture(g, a->size * 2))
        return false;
    atlas_clear(a);
    return true;
}

static inline uint64_t atlas_hash(const struct atlas_entry *k)
{
    uint64_t h = k->id ^ (k->version * UINT64_C(0x9E3779B97F4A7C15)) ^
                 ((uint64_t)k->space << 56) ^ UINT64_C(0xA5A5A5A5);
    return hash64_finish(h);
}

static bool atlas_key_eq(const struct atlas_entry *e, const struct atlas_entry *k)
{
    return e->id == k->id && e->version == k->version && e->space == k->space;
}

static const struct atlas_entry *atlas_find(const struct atlas *a, uint64_t id, uint64_t version,
                                            enum atlas_space space)
{
    struct atlas_entry key = { .id = id, .version = version, .space = space };
    for (uint32_t i = (uint32_t)atlas_hash(&key) & (ATLAS_MAP_SIZE - 1);;
         i = (i + 1) & (ATLAS_MAP_SIZE - 1)) {
        const struct atlas_entry *e = &a->map[i];
        if (!e->used)
            return NULL;
        if (atlas_key_eq(e, &key))
            return e;
    }
}

static bool atlas_insert(struct atlas *a, const struct atlas_entry *entry)
{
    if (a->count >= ATLAS_MAP_LIMIT) {
        a->full = true;
        return false;
    }
    uint32_t i = (uint32_t)atlas_hash(entry) & (ATLAS_MAP_SIZE - 1);
    while (a->map[i].used)
        i = (i + 1) & (ATLAS_MAP_SIZE - 1);
    a->map[i] = *entry;
    a->map[i].used = true;
    a->count++;
    return true;
}

static bool atlas_place(struct atlas *a, int w, int h, uint16_t *x, uint16_t *y)
{
    int quantized = (h + 3) & ~3;
    for (int i = 0; i < a->n_shelves; i++) {
        struct shelf *s = &a->shelves[i];
        if (s->h >= h && s->h <= quantized + quantized / 2 && s->x + w <= a->size) {
            *x = s->x;
            *y = s->y;
            s->x = (uint16_t)(s->x + w);
            return true;
        }
    }
    if (a->n_shelves == SHELVES_MAX || a->next_y + quantized > a->size)
        return false;
    struct shelf *s = &a->shelves[a->n_shelves++];
    *s = (struct shelf){ .y = (uint16_t)a->next_y, .h = (uint16_t)quantized, .x = (uint16_t)w };
    a->next_y += quantized;
    *x = 0;
    *y = s->y;
    return true;
}

static uint32_t *atlas_stage(struct render_gpu *g, int w, int h, uint16_t *x, uint16_t *y)
{
    struct atlas *a = &g->atlas;
    if (w > a->size || h > a->size) {
        a->full = true;
        return NULL;
    }
    if (!atlas_place(a, w, h, x, y)) {
        a->full = true;
        return NULL;
    }
    size_t bytes = (size_t)w * (size_t)h * 4;
    if (a->stage_used + bytes > a->stage_cap) {
        size_t cap = a->stage_cap ? a->stage_cap * 2 : (size_t)1 << 20;
        while (cap < a->stage_used + bytes)
            cap *= 2;
        uint8_t *p = realloc(a->stage, cap);
        if (!p) {
            g->failed = true;
            return NULL;
        }
        a->stage = p;
        a->stage_cap = cap;
    }
    if (a->n_uploads == a->cap_uploads) {
        uint32_t cap = a->cap_uploads ? a->cap_uploads * 2 : 128;
        struct upload *p = realloc(a->uploads, (size_t)cap * sizeof(*p));
        VkBufferImageCopy *cp = realloc(a->vk_copies, (size_t)cap * sizeof(*cp));
        if (!p || !cp) {
            if (p) a->uploads = p;
            if (cp) a->vk_copies = cp;
            g->failed = true;
            return NULL;
        }
        a->uploads = p;
        a->vk_copies = cp;
        a->cap_uploads = cap;
    }
    a->vk_copies[a->n_uploads] = (VkBufferImageCopy){
        .bufferOffset = a->stage_used,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)*x, (int32_t)*y, 0 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    a->uploads[a->n_uploads++] = (struct upload){
        .x = *x, .y = *y, .w = (uint16_t)w, .h = (uint16_t)h, .offset = a->stage_used,
    };
    uint32_t *texels = (uint32_t *)(void *)(a->stage + a->stage_used);
    a->stage_used += bytes;
    return texels;
}

static enum glyph_kind glyph_kind_of(const struct glyph *gl)
{
    if (!gl || !gl->width || !gl->height)
        return KIND_NONE;
    if (gl->has_color)
        return gl->color ? KIND_COLOR : KIND_NONE;
    if (gl->has_subpx_ink && gl->subpx)
        return KIND_SUBPX;
    return gl->alpha ? KIND_ALPHA : KIND_NONE;
}

static bool atlas_add_glyph(struct render_gpu *g, uint64_t id, uint64_t version,
                            enum atlas_space space, const struct glyph *gl, bool cache,
                            struct atlas_entry *out)
{
    enum glyph_kind kind = glyph_kind_of(gl);
    struct atlas_entry e = { .id = id, .version = version, .space = space, .kind = kind };
    if (kind != KIND_NONE) {
        int w = gl->width, rows = gl->height;
        uint32_t *dst = atlas_stage(g, w, rows, &e.x, &e.y);
        if (!dst)
            return false;
        e.w = (uint16_t)w;
        e.rows = (uint16_t)rows;
        e.x_offset = gl->x_offset;
        for (int y = 0; y < rows; y++, dst += w) {
            switch (kind) {
            case KIND_ALPHA: {
                const uint8_t *s = gl->alpha[y];
                for (int x = 0; x < w; x++)
                    dst[x] = (uint32_t)s[x] << 24;
                break;
            }
            case KIND_SUBPX: {
                const uint8_t *s = gl->subpx[y];
                for (int x = 0; x < w; x++)
                    dst[x] = 0xFF000000u | (uint32_t)s[3 * x] << 16 |
                             (uint32_t)s[3 * x + 1] << 8 | s[3 * x + 2];
                break;
            }
            case KIND_COLOR:
                memcpy(dst, gl->color + (size_t)y * (size_t)w, (size_t)w * sizeof(*dst));
                break;
            case KIND_NONE:
                break;
            }
        }
    }
    *out = e;
    return !cache || atlas_insert(&g->atlas, &e);
}

static bool atlas_add_mask(struct render_gpu *g, uint64_t id, uint64_t version,
                           enum atlas_space space, const uint8_t *cov, int w, int rows,
                           int stride, struct atlas_entry *out)
{
    struct atlas_entry e = {
        .id = id, .version = version, .space = space, .kind = KIND_ALPHA,
        .w = (uint16_t)w, .rows = (uint16_t)rows,
    };
    uint32_t *dst = atlas_stage(g, w, rows, &e.x, &e.y);
    if (!dst)
        return false;
    for (int y = 0; y < rows; y++, dst += w)
        for (int x = 0; x < w; x++)
            dst[x] = (uint32_t)cov[(size_t)y * (size_t)stride + (size_t)x] << 24;
    *out = e;
    return atlas_insert(&g->atlas, &e);
}

/* ---- Grid assembly ------------------------------------------------------ */

struct grid_geometry {
    const struct terminal_frame *tf;
    struct font_instance *font;
    struct rect2i rect;
    int cw;
    int ch;
    int cols;
    int rows;
};

static bool grid_geometry_of(const struct render_frame *frame, struct vector2i canvas,
                             struct grid_geometry *out)
{
    const struct terminal_frame *tf = frame->grid.frame;
    struct font_instance *font = frame->grid.font ? frame->grid.font : font_instance_default();
    struct rect2i rect = frame->grid.rect;
    if (!tf || !terminal_frame_valid(tf) || !font || rect.x < 0 || rect.y < 0 ||
        !rect2i_clip(&rect, canvas))
        return false;
    const struct font_metrics *m = font_instance_metrics(font);
    if (m->cell_w <= 0 || m->cell_h <= 0)
        return false;
    *out = (struct grid_geometry){
        .tf = tf, .font = font, .rect = rect, .cw = m->cell_w, .ch = m->cell_h,
        .cols = min_i(rect.width / m->cell_w, tf->cols),
        .rows = min_i(rect.height / m->cell_h, tf->rows),
    };
    return true;
}

struct grid_build {
    struct render_gpu *g;
    const struct grid_geometry *geo;
    const struct decor_set *decor;
    uint64_t font_version;
    uint64_t decor_version;
    uint32_t default_bg;
    uint32_t cursor_color;
    int row;
    enum wsap_vtcore_line_size line_size;
    bool run_open;
    int run_x0;
    int run_x1;
    uint32_t run_bg;
};

static void bg_flush(struct grid_build *b)
{
    if (!b->run_open)
        return;
    b->run_open = false;
    struct glyph_inst *in = push_glyph(b->g, ST_BG);
    if (!in)
        return;
    const struct grid_geometry *geo = b->geo;
    int cell_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE) ? 2 * geo->cw : geo->cw;
    rect_to_vec4((float)(geo->rect.x + b->run_x0 * cell_w),
                 (float)(geo->rect.y + b->row * geo->ch),
                 (float)((b->run_x1 - b->run_x0) * cell_w), (float)geo->ch, in->rect);
    opaque_color_of(b->run_bg, 1.0f, in->color);
    in->mode = MODE_SOLID;
}

static void bg_add(struct grid_build *b, int c0, int c1, uint32_t bg)
{
    bg |= 0xFF000000u;
    if (bg == b->default_bg) {
        bg_flush(b);
        return;
    }
    if (b->run_open && b->run_x1 == c0 && b->run_bg == bg) {
        b->run_x1 = c1;
        return;
    }
    bg_flush(b);
    b->run_open = true;
    b->run_x0 = c0;
    b->run_x1 = c1;
    b->run_bg = bg;
}

static void grid_run(void *ctx, int c0, int c1, uint32_t bg)
{
    bg_add(ctx, c0, c1, bg);
}

static void emit_glyph_part(struct grid_build *b, int col, const struct atlas_entry *e, int c0,
                            int c1, int dest_x, uint32_t fg, bool faint)
{
    const struct grid_geometry *geo = b->geo;
    int rows = min_i(e->rows, geo->ch);
    if (c1 <= c0 || rows <= 0)
        return;
    (void)col;
    bool subpx = e->kind == KIND_SUBPX;
    struct glyph_inst *in = push_glyph(b->g, subpx ? ST_SUBPX : ST_INK);
    if (!in)
        return;
    bool double_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE);
    bool double_h = (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP ||
                     b->line_size == WSAP_VTCORE_LINE_DOUBLE_BOTTOM);
    float width = (float)(c1 - c0);
    float height = (float)rows;
    float v0 = (float)e->y;
    float v1 = (float)(e->y + rows);
    if (double_w)
        width *= 2.0f;
    if (double_h) {
        int half_h = geo->ch / 2;
        if (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP) {
            int sub_rows = min_i(rows, half_h);
            v1 = (float)(e->y + sub_rows);
            height = (float)(sub_rows * 2);
        } else {
            int start_row = min_i(half_h, rows);
            int sub_rows = rows > half_h ? rows - half_h : 0;
            v0 = (float)(e->y + start_row);
            v1 = (float)(e->y + rows);
            height = (float)(sub_rows * 2);
        }
    }
    rect_to_vec4((float)dest_x, (float)(geo->rect.y + b->row * geo->ch), width, height, in->rect);
    rect_to_vec4((float)(e->x + c0), v0, (float)(e->x + c1), v1, in->uv);
    float opacity = faint ? 128.0f / 255.0f : 1.0f;
    if (e->kind == KIND_COLOR) {
        in->color[0] = 1.0f;
        in->color[1] = 1.0f;
        in->color[2] = 1.0f;
        in->color[3] = opacity;
        in->mode = MODE_COLOR;
    } else {
        opaque_color_of(fg, opacity, in->color);
        in->mode = subpx ? MODE_SUBPX : MODE_ALPHA;
    }
}

static void emit_mask(struct grid_build *b, int col, const struct decor_mask *m, int index,
                      uint32_t color, bool faint)
{
    if (!m || m->y0 >= m->y1)
        return;
    const struct grid_geometry *geo = b->geo;
    bool double_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE);
    bool double_h = (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP ||
                     b->line_size == WSAP_VTCORE_LINE_DOUBLE_BOTTOM);
    int cell_w = double_w ? 2 * geo->cw : geo->cw;
    int rows = m->y1 - m->y0;
    int y0 = m->y0;
    float dest_y = (float)(geo->rect.y + b->row * geo->ch);
    float height = (float)rows;
    if (double_h) {
        int half_h = geo->ch / 2;
        if (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP) {
            if (y0 >= half_h) return;
            rows = min_i(m->y1, half_h) - y0;
            height = (float)(rows * 2);
            dest_y += (float)(y0 * 2);
        } else {
            if (m->y1 <= half_h) return;
            y0 = max_i(y0, half_h);
            rows = m->y1 - y0;
            height = (float)(rows * 2);
            dest_y += (float)((y0 - half_h) * 2);
        }
    } else {
        dest_y += (float)y0;
    }
    struct glyph_inst *in = push_glyph(b->g, ST_DECO);
    if (!in)
        return;
    rect_to_vec4((float)(geo->rect.x + col * cell_w), dest_y, (float)cell_w, height, in->rect);
    opaque_color_of(color, faint ? 128.0f / 255.0f : 1.0f, in->color);
    if (m->solid) {
        in->mode = MODE_SOLID;
        return;
    }
    struct atlas_entry e;
    const struct atlas_entry *hit = atlas_find(&b->g->atlas, (uint64_t)index, b->decor_version,
                                               SPACE_DECOR);
    if (hit) {
        e = *hit;
    } else if (!atlas_add_mask(b->g, (uint64_t)index, b->decor_version, SPACE_DECOR,
                               m->cov + (size_t)m->y0 * (size_t)geo->cw, geo->cw, rows, geo->cw,
                               &e)) {
        return;
    }
    in->mode = MODE_ALPHA;
    rect_to_vec4((float)e.x, (float)e.y, (float)(e.x + geo->cw), (float)(e.y + rows), in->uv);
}

enum {
    MASK_STRIKE = 16,
    MASK_OVERLINE,
    MASK_CURSOR_BAR,
    MASK_CURSOR_UNDERLINE,
};

static void emit_decor(struct grid_build *b, int col, const struct render_cell *cell)
{
    const struct decor_set *dc = b->decor;
    uint16_t fl = cell->flags;
    if (!dc || !(fl & (RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)))
        return;
    bool faint = (fl & RENDER_CELL_FAINT) != 0;
    uint32_t ul = (fl & RENDER_CELL_UL_COLOR) ? cell->ul : cell->fg;
    unsigned underline = render_cell_underline(fl);
    if (underline && underline < DECOR_UNDERLINE_KINDS)
        emit_mask(b, col, &dc->underline[underline], (int)underline, ul, faint);
    if (fl & RENDER_CELL_STRIKE)
        emit_mask(b, col, &dc->strike, MASK_STRIKE, cell->fg, faint);
    if (fl & RENDER_CELL_OVERLINE)
        emit_mask(b, col, &dc->overline, MASK_OVERLINE, cell->fg, faint);
    if (fl & RENDER_CELL_CURSOR_BAR)
        emit_mask(b, col, &dc->cursor_bar, MASK_CURSOR_BAR, b->cursor_color, false);
    if (fl & RENDER_CELL_CURSOR_UNDERLINE)
        emit_mask(b, col, &dc->cursor_underline, MASK_CURSOR_UNDERLINE, b->cursor_color, false);
}

static void grid_cell(void *ctx, int col, const struct render_cell *cell,
                      const struct render_glyph_ref *ref)
{
    struct grid_build *b = ctx;
    const struct grid_geometry *geo = b->geo;
    if (b->g->failed || b->g->atlas.full)
        return;
    bg_add(b, col, col + 1, cell->bg);
    uint16_t fl = cell->flags;
    if (!(fl & RENDER_CELL_BLANK)) {
        struct atlas_entry e;
        const struct atlas_entry *hit =
            ref->id ? atlas_find(&b->g->atlas, ref->id, b->font_version, SPACE_GRID) : NULL;
        if (hit) {
            e = *hit;
        } else if (!atlas_add_glyph(b->g, ref->id, b->font_version, SPACE_GRID,
                                    render_glyph_get(ref), ref->id != 0, &e)) {
            return;
        }
        if (e.kind != KIND_NONE) {
            bool double_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE);
            int cell_w = double_w ? 2 * geo->cw : geo->cw;
            int cell_x = geo->rect.x + col * cell_w;
            int x_off = double_w ? 2 * e.x_offset : e.x_offset;
            int split = geo->cw - e.x_offset;
            split = split < 0 ? 0 : split > e.w ? e.w : split;
            bool faint = (fl & RENDER_CELL_FAINT) != 0;
            if (fl & RENDER_CELL_WIDE_TAIL)
                emit_glyph_part(b, col, &e, split, e.w,
                                double_w ? cell_x + 2 * (split - (geo->cw - e.x_offset))
                                         : cell_x + split - (geo->cw - e.x_offset),
                                cell->fg, faint);
            else if (fl & RENDER_CELL_WIDE)
                emit_glyph_part(b, col, &e, 0, split, cell_x + x_off, cell->fg, faint);
            else
                emit_glyph_part(b, col, &e, 0, e.w, cell_x + x_off, cell->fg, faint);
        }
    }
    emit_decor(b, col, cell);
}

static bool build_grid(struct render_gpu *g, const struct grid_geometry *geo)
{
    const struct terminal_frame *tf = geo->tf;
    struct grid_build b = {
        .g = g,
        .geo = geo,
        .decor = decor_get(g->decor, font_instance_metrics(geo->font)),
        .font_version = font_instance_version(geo->font),
        .default_bg = tf->default_bg | 0xFF000000u,
        .cursor_color = tf->cursor_color,
    };
    struct decor_key dk = decor_key_of(font_instance_metrics(geo->font));
    b.decor_version = hash64_finish(((uint64_t)(uint32_t)dk.cw << 32) | (uint32_t)dk.ch) ^
                      hash64_finish(((uint64_t)(uint32_t)dk.baseline << 32) |
                                    (uint32_t)dk.thickness);

    struct glyph_inst *fill = push_glyph(g, ST_BG);
    if (!fill)
        return false;
    rect2i_to_vec4(geo->rect, fill->rect);
    opaque_color_of(b.default_bg, 1.0f, fill->color);
    fill->mode = MODE_SOLID;

    const struct render_walk_sink sink = { .cell = grid_cell, .run = grid_run, .ctx = &b };
    (void)render_walk_thread_reserve(tf->cols);
    glyph_cache_set_parallel(true);
    for (int row = 0; row < geo->rows && !g->failed && !g->atlas.full; row++) {
        b.row = row;
        b.line_size = terminal_frame_row_size(tf, row);
        int cols = (b.line_size != WSAP_VTCORE_LINE_SINGLE)
            ? (geo->cols / 2 > 0 ? geo->cols / 2 : 1) : geo->cols;
        render_row_walk(geo->font, tf, row, 0, cols, RENDER_WALK_PEEK, &sink);
        bg_flush(&b);
    }
    glyph_cache_set_parallel(false);
    return !g->failed && !g->atlas.full;
}

/* ---- UI scene assembly -------------------------------------------------- */

static void emit_solid(struct render_gpu *g, struct rect2i r, const float color[4],
                       struct rect2i canvas_rect)
{
    struct cmd *c = cmd_open(g, CMD_GLYPH, ST_UI, canvas_rect, g->atlas.desc_set);
    struct glyph_inst *in = push_glyph(g, ST_UI);
    if (!c || !in)
        return;
    c->count++;
    rect2i_to_vec4(r, in->rect);
    memcpy(in->color, color, sizeof(in->color));
    in->mode = MODE_SOLID;
}

static void emit_shape(struct render_gpu *g, struct rect2i scissor, struct rect2i shape,
                       const float color[4], float radius, enum shape_mode mode, uint32_t blur)
{
    struct cmd *c = cmd_open(g, CMD_SHAPE, ST_SHAPE, scissor, g->atlas.desc_set);
    struct shape_inst *in = stream_push(g, ST_SHAPE);
    if (!c || !in)
        return;
    c->count++;
    rect2i_to_vec4(shape, in->rect);
    memcpy(in->color, color, sizeof(in->color));
    in->radius = radius;
    in->mode = (uint32_t)mode;
    in->blur = blur;
}

static VkDescriptorSet image_descriptor(struct render_gpu *g, const struct ui_image *image)
{
    struct image_entry *victim = &g->images[0];
    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct image_entry *e = &g->images[i];
        if (e->image && image->gen && e->pixels == image->pixels && e->gen == image->gen &&
            vector2i_eq(e->size, image->size)) {
            e->used = ++g->image_clock;
            return e->desc_set;
        }
        if (e->used < victim->used)
            victim = e;
    }
    if (victim->image) {
        vkDeviceWaitIdle(g->device);
        vkDestroyImageView(g->device, victim->view, NULL);
        vkDestroyImage(g->device, victim->image, NULL);
        vkFreeMemory(g->device, victim->memory, NULL);
        victim->image = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { (uint32_t)image->size.x, (uint32_t)image->size.y, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(g->device, &ii, NULL, &victim->image) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(g->device, victim->image, &reqs);
    uint32_t type_idx = find_memory_type(g->physical_device, reqs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_idx == UINT32_MAX)
        type_idx = find_memory_type(g->physical_device, reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type_idx == UINT32_MAX)
        return VK_NULL_HANDLE;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type_idx,
    };
    if (vkAllocateMemory(g->device, &ai, NULL, &victim->memory) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (vkBindImageMemory(g->device, victim->image, victim->memory, 0) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = victim->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(g->device, &vi, NULL, &victim->view) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    if (!victim->desc_set) {
        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &g->desc_layout,
        };
        if (vkAllocateDescriptorSets(g->device, &dsai, &victim->desc_set) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo atlas_info = {
        .sampler = g->sampler,
        .imageView = g->atlas.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkDescriptorImageInfo image_info = {
        .sampler = g->sampler,
        .imageView = victim->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = victim->desc_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &atlas_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = victim->desc_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        },
    };
    vkUpdateDescriptorSets(g->device, 2, writes, 0, NULL);

    victim->pixels = image->pixels;
    victim->gen = image->gen;
    victim->size = image->size;
    victim->used = ++g->image_clock;
    victim->needs_upload = true;
    return victim->desc_set;
}

static void scene_image(struct render_gpu *g, const struct ui_scene_op *op, struct rect2i clip,
                        struct rect2i canvas_rect)
{
    const struct ui_image *image = op->image;
    if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
        return;
    struct rect2i img = {
        .pos = op->rect.pos,
        .size = { .x = min_i(op->rect.width, image->size.x),
                  .y = min_i(op->rect.height, image->size.y) },
    };
    struct rect2i r = rect2i_intersect(img, clip);
    if (!rect2i_clip(&r, canvas_rect.size))
        return;
    VkDescriptorSet ds = image_descriptor(g, image);
    if (!ds) {
        g->failed = true;
        return;
    }
    struct cmd *c = cmd_open(g, CMD_GLYPH, ST_UI, canvas_rect, ds);
    struct glyph_inst *in = push_glyph(g, ST_UI);
    if (!c || !in)
        return;
    c->count++;
    float sx = (float)(r.x - op->rect.x), sy = (float)(r.y - op->rect.y);
    rect2i_to_vec4(r, in->rect);
    rect_to_vec4(sx, sy, sx + (float)r.width, sy + (float)r.height, in->uv);
    in->color[0] = 1.0f;
    in->color[1] = 1.0f;
    in->color[2] = 1.0f;
    in->color[3] = 1.0f;
    in->mode = MODE_IMAGE;
}

static void scene_text(struct render_gpu *g, struct font_instance *fi, const struct ui_scene *s,
                       const struct ui_scene_op *op, struct rect2i clip, struct rect2i canvas_rect)
{
    const char *text = ui_scene_op_text(s, op);
    if (!text || op->style.advance <= 0 || op->style.cell_h <= 0)
        return;
    clip = rect2i_intersect(clip, ui_scene_text_ink(s, op));
    if (!rect2i_clip(&clip, canvas_rect.size))
        return;
    const int max_rows = min_i(op->style.cell_h, font_instance_metrics(fi)->cell_h);
    const uint16_t flags = (uint16_t)(op->style.flags & (UI_TEXT_BOLD | UI_TEXT_ITALIC));
    const uint64_t version = font_instance_version(fi);
    float color[4];
    color_of(op->color, 1.0f, color);
    int64_t tx = op->rect.x;
    for (uint32_t i = 0; i < op->text_len;) {
        uint32_t cp;
        int cells;
        i += (uint32_t)ui_text_run_next(text + i, op->text_len - i, &cp, &cells);
        if (tx + 382 > clip.x) {
            uint64_t id = (uint64_t)cp | (uint64_t)flags << 32;
            struct atlas_entry e;
            const struct atlas_entry *hit = atlas_find(&g->atlas, id, version, SPACE_UI);
            if (hit)
                e = *hit;
            else if (!atlas_add_glyph(g, id, version, SPACE_UI,
                                      glyph_cache_fetch(fi, cp, flags), true, &e))
                return;
            int rows = min_i(e.rows, max_rows);
            if (e.kind != KIND_NONE && rows > 0) {
                bool subpx = e.kind == KIND_SUBPX;
                struct cmd *c = cmd_open(g, subpx ? CMD_SUBPX : CMD_GLYPH, ST_UI, clip,
                                         g->atlas.desc_set);
                struct glyph_inst *in = push_glyph(g, ST_UI);
                if (!c || !in)
                    return;
                c->count++;
                rect_to_vec4((float)(tx + e.x_offset), (float)op->rect.y, (float)e.w, (float)rows,
                             in->rect);
                rect_to_vec4((float)e.x, (float)e.y, (float)(e.x + e.w), (float)(e.y + rows),
                             in->uv);
                if (e.kind == KIND_COLOR) {
                    in->color[0] = 1.0f;
                    in->color[1] = 1.0f;
                    in->color[2] = 1.0f;
                    in->color[3] = color[3];
                    in->mode = MODE_COLOR;
                } else {
                    memcpy(in->color, color, sizeof(in->color));
                    in->mode = subpx ? MODE_SUBPX : MODE_ALPHA;
                }
            }
        }
        tx += (int64_t)cells * op->style.advance;
        if (tx >= (int64_t)clip.x + clip.width + 128)
            break;
    }
}

static bool op_outside_clip(const struct ui_scene *s, const struct ui_scene_op *op,
                            struct rect2i clip)
{
    struct rect2i ink = op->kind == UI_SCENE_OP_TEXT ? ui_scene_text_ink(s, op) : op->rect;
    return rect2i_is_empty(rect2i_intersect(ink, clip));
}

static void scene_rect(struct render_gpu *g, struct rect2i rect, struct rect2i clip,
                       uint32_t color, float opacity_mult, struct rect2i canvas_rect)
{
    struct rect2i r = rect2i_intersect(rect, clip);
    if (!rect2i_clip(&r, canvas_rect.size))
        return;
    float c[4];
    color_of(color, opacity_mult, c);
    if (c[3] > 0.0f)
        emit_solid(g, r, c, canvas_rect);
}

static bool build_scene(struct render_gpu *g, struct font_instance *ufont,
                        const struct ui_scene *scene, struct rect2i canvas_rect)
{
    struct rect2i inline_stack[16];
    struct rect2i *stack = inline_stack;
    int stack_cap = (int)(sizeof(inline_stack) / sizeof(inline_stack[0]));
    if (scene->clips_cap > stack_cap) {
        if (scene->clips_cap > g->cap_clips) {
            struct rect2i *p = realloc(g->clips, (size_t)scene->clips_cap * sizeof(*p));
            if (!p) {
                g->failed = true;
                return false;
            }
            g->clips = p;
            g->cap_clips = scene->clips_cap;
        }
        stack = g->clips;
        stack_cap = g->cap_clips;
    }
    int depth = 0;
    struct rect2i clip = canvas_rect;

    for (int i = 0; i < scene->n_ops && !g->failed && !g->atlas.full; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        switch (op->kind) {
        case UI_SCENE_OP_PUSH_CLIP:
            if (depth < stack_cap)
                stack[depth++] = clip;
            clip = rect2i_intersect(clip, op->rect);
            break;
        case UI_SCENE_OP_POP_CLIP:
            if (depth > 0)
                clip = stack[--depth];
            break;
        case UI_SCENE_OP_RECT:
            if (!op_outside_clip(scene, op, clip))
                scene_rect(g, op->rect, clip, op->color, 1.0f, canvas_rect);
            break;
        case UI_SCENE_OP_BLEND_RECT:
            if (!op_outside_clip(scene, op, clip))
                scene_rect(g, op->rect, clip, op->color | 0xFF000000u, op->alpha / 255.0f,
                           canvas_rect);
            break;
        case UI_SCENE_OP_ROUNDED_RECT: {
            if (op_outside_clip(scene, op, clip))
                break;
            if (op->radius <= 0) {
                scene_rect(g, op->rect, clip, op->color, 1.0f, canvas_rect);
                break;
            }
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size) || !(op->color >> 24))
                break;
            int radius = min_i(op->radius, min_i(op->rect.width / 2, op->rect.height / 2));
            float c[4];
            color_of(op->color, 1.0f, c);
            emit_shape(g, visible, op->rect, c, (float)radius, SHAPE_ROUNDED, 0);
            break;
        }
        case UI_SCENE_OP_CIRCLE: {
            if (op_outside_clip(scene, op, clip) || op->radius <= 0 || !op->alpha)
                break;
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size))
                break;
            float c[4];
            color_of(op->color | 0xFF000000u, op->alpha / 255.0f, c);
            emit_shape(g, visible, op->rect, c, (float)op->radius, SHAPE_CIRCLE, 0);
            break;
        }
        case UI_SCENE_OP_SHADOW: {
            if (op_outside_clip(scene, op, clip) || op->blur <= 0 || !(op->color >> 24))
                break;
            int blur = min_i(op->blur, UI_SHADOW_MAX_BLUR);
            int radius = geom_clamp_i(op->radius, 0, UI_SHADOW_MAX_RADIUS);
            if (rect2i_is_empty(rect2i_inset(op->rect, ui_shadow_extent(blur))))
                break;
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size))
                break;
            float c[4];
            color_of(op->color, 1.0f, c);
            emit_shape(g, visible, op->rect, c, (float)radius, SHAPE_SHADOW, (uint32_t)blur);
            break;
        }
        case UI_SCENE_OP_TEXT:
            if (!op_outside_clip(scene, op, clip))
                scene_text(g, ufont, scene, op, clip, canvas_rect);
            break;
        case UI_SCENE_OP_IMAGE:
            if (!op_outside_clip(scene, op, clip))
                scene_image(g, op, clip, canvas_rect);
            break;
        }
    }
    return !g->failed && !g->atlas.full;
}

static bool build_frame(struct render_gpu *g, const struct render_frame *frame,
                        struct vector2i canvas)
{
    begin_build(g);
    const struct rect2i canvas_rect = { .size = canvas };

    struct grid_geometry geo;
    if (grid_geometry_of(frame, canvas, &geo)) {
        if (!build_grid(g, &geo))
            return false;
        cmd_whole_stream(g, CMD_GLYPH, ST_BG, canvas_rect);
        cmd_whole_stream(g, CMD_GLYPH, ST_INK, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
        cmd_whole_stream(g, CMD_SUBPX, ST_SUBPX, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
        cmd_whole_stream(g, CMD_GLYPH, ST_DECO, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
    }
    const struct ui_scene *scene = frame->ui;
    if (scene && ui_scene_is_valid(scene)) {
        struct font_instance *ufont =
            frame->ui_font ? frame->ui_font : font_instance_default();
        if (ufont && !build_scene(g, ufont, scene, canvas_rect))
            return false;
    }
    return !g->failed && !g->atlas.full;
}

/* ---- Pipelines and render pass ------------------------------------------ */

static VkShaderModule create_shader_module(VkDevice device, const uint32_t *spv, size_t size)
{
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = spv,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, NULL, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

static bool ensure_render_pass(struct render_gpu *g, VkFormat format)
{
    if (g->render_pass && g->render_pass_copy && g->render_pass_format == format)
        return true;
    if (g->render_pass) {
        vkDestroyRenderPass(g->device, g->render_pass, NULL);
        g->render_pass = VK_NULL_HANDLE;
    }
    if (g->render_pass_copy) {
        vkDestroyRenderPass(g->device, g->render_pass_copy, NULL);
        g->render_pass_copy = VK_NULL_HANDLE;
    }
    VkAttachmentDescription color_att = {
        .format = format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };
    VkSubpassDependency deps[2] = {
        {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        },
        {
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = 0,
        },
    };
    VkRenderPassCreateInfo rpi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_att,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 2,
        .pDependencies = deps,
    };
    if (vkCreateRenderPass(g->device, &rpi, NULL, &g->render_pass) != VK_SUCCESS)
        return false;

    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    if (vkCreateRenderPass(g->device, &rpi, NULL, &g->render_pass_copy) != VK_SUCCESS) {
        vkDestroyRenderPass(g->device, g->render_pass, NULL);
        g->render_pass = VK_NULL_HANDLE;
        return false;
    }
    g->render_pass_format = format;
    return true;
}

static VkPipeline make_graphics_pipeline(struct render_gpu *g, VkShaderModule vert,
                                         VkShaderModule frag,
                                         const VkVertexInputBindingDescription *binding,
                                         const VkVertexInputAttributeDescription *attribs,
                                         uint32_t attrib_count, bool blend,
                                         VkColorComponentFlags write_mask)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = binding ? 1 : 0,
        .pVertexBindingDescriptions = binding,
        .vertexAttributeDescriptionCount = attrib_count,
        .pVertexAttributeDescriptions = attribs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cb = {
        .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = write_mask,
    };
    VkPipelineColorBlendStateCreateInfo bs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cb,
    };
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };
    VkGraphicsPipelineCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &bs,
        .pDynamicState = &ds,
        .layout = g->pipeline_layout,
        .renderPass = g->render_pass,
        .subpass = 0,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g->device, g->pipeline_cache, 1, &pi, NULL, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

static bool pipelines_create(struct render_gpu *g)
{
    if (!g->render_pass && !ensure_render_pass(g, VK_FORMAT_B8G8R8A8_UNORM))
        return false;

    if (g->glyph_pipeline) {
        vkDestroyPipeline(g->device, g->glyph_pipeline, NULL);
        g->glyph_pipeline = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 3; i++) {
        if (g->subpx_pipeline[i]) {
            vkDestroyPipeline(g->device, g->subpx_pipeline[i], NULL);
            g->subpx_pipeline[i] = VK_NULL_HANDLE;
        }
    }
    if (g->shape_pipeline) {
        vkDestroyPipeline(g->device, g->shape_pipeline, NULL);
        g->shape_pipeline = VK_NULL_HANDLE;
    }
    if (g->copy_pipeline) {
        vkDestroyPipeline(g->device, g->copy_pipeline, NULL);
        g->copy_pipeline = VK_NULL_HANDLE;
    }

    VkShaderModule glyph_vert = create_shader_module(g->device, glyph_vert_spv, glyph_vert_spv_size);
    VkShaderModule glyph_frag = create_shader_module(g->device, glyph_frag_spv, glyph_frag_spv_size);
    VkShaderModule shape_vert = create_shader_module(g->device, shape_vert_spv, shape_vert_spv_size);
    VkShaderModule shape_frag = create_shader_module(g->device, shape_frag_spv, shape_frag_spv_size);
    VkShaderModule copy_frag = create_shader_module(g->device, copy_frag_spv, copy_frag_spv_size);
    if (!glyph_vert || !glyph_frag || !shape_vert || !shape_frag || !copy_frag)
        return false;

    VkVertexInputBindingDescription glyph_binding = {
        .binding = 0,
        .stride = sizeof(struct glyph_inst),
        .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
    };
    VkVertexInputAttributeDescription glyph_attribs[4] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(struct glyph_inst, rect) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(struct glyph_inst, uv) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(struct glyph_inst, color) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = offsetof(struct glyph_inst, mode) },
    };

    VkVertexInputBindingDescription shape_binding = {
        .binding = 0,
        .stride = sizeof(struct shape_inst),
        .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
    };
    VkVertexInputAttributeDescription shape_attribs[5] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(struct shape_inst, rect) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(struct shape_inst, color) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(struct shape_inst, radius) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = offsetof(struct shape_inst, mode) },
        { .location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = offsetof(struct shape_inst, blur) },
    };

    VkColorComponentFlags all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    g->glyph_pipeline = make_graphics_pipeline(g, glyph_vert, glyph_frag, &glyph_binding,
                                               glyph_attribs, 4, true, all);
    g->shape_pipeline = make_graphics_pipeline(g, shape_vert, shape_frag, &shape_binding,
                                               shape_attribs, 5, true, all);
    g->copy_pipeline = make_graphics_pipeline(g, glyph_vert, copy_frag, &glyph_binding,
                                              glyph_attribs, 4, false, all);

    VkColorComponentFlags masks[3] = {
        VK_COLOR_COMPONENT_R_BIT, VK_COLOR_COMPONENT_G_BIT, VK_COLOR_COMPONENT_B_BIT,
    };
    bool ok = g->glyph_pipeline && g->shape_pipeline && g->copy_pipeline;
    for (int i = 0; i < 3; i++) {
        g->subpx_pipeline[i] = make_graphics_pipeline(g, glyph_vert, glyph_frag, &glyph_binding,
                                                      glyph_attribs, 4, true, masks[i]);
        ok = ok && g->subpx_pipeline[i];
    }
    vkDestroyShaderModule(g->device, glyph_vert, NULL);
    vkDestroyShaderModule(g->device, glyph_frag, NULL);
    vkDestroyShaderModule(g->device, shape_vert, NULL);
    vkDestroyShaderModule(g->device, shape_frag, NULL);
    vkDestroyShaderModule(g->device, copy_frag, NULL);
    return ok;
}

/* ---- Swapchain per surface ---------------------------------------------- */

static struct gpu_surface *gpu_surface_of(struct render_gpu *g, struct render_surface *s)
{
    if (!s->gpu) {
        struct gpu_surface *gs = calloc(1, sizeof(struct gpu_surface));
        if (!gs)
            return NULL;
        gs->device = g->device;
        gs->desc_pool = g->desc_pool;
        s->gpu = gs;
    }
    return s->gpu;
}

static void surface_swapchain_destroy_retired(struct gpu_surface *gs)
{
    if (!gs->retired.swapchain)
        return;
    for (uint32_t i = 0; i < gs->retired.image_count; i++) {
        if (gs->retired.render_finished[i])
            vkDestroySemaphore(gs->device, gs->retired.render_finished[i], NULL);
        if (gs->retired.framebuffers[i])
            vkDestroyFramebuffer(gs->device, gs->retired.framebuffers[i], NULL);
        if (gs->retired.views[i])
            vkDestroyImageView(gs->device, gs->retired.views[i], NULL);
    }
    vkDestroySwapchainKHR(gs->device, gs->retired.swapchain, NULL);
    memset(&gs->retired, 0, sizeof(gs->retired));
}

/*
 * Wait until the queue completes the submitted frames. Do not wait on the slot
 * fences: slot_acquire() resets the fence of the current slot before the
 * swapchain is made, and that fence signals only after its frame is submitted.
 */
static void wait_submitted_frames(struct render_gpu *g)
{
    vkQueueWaitIdle(g->queue);
}

static void surface_swapchain_destroy(struct gpu_surface *gs)
{
    if (gs->device)
        vkDeviceWaitIdle(gs->device);
    surface_swapchain_destroy_retired(gs);
    for (uint32_t i = 0; i < gs->image_count; i++) {
        if (gs->render_finished[i])
            vkDestroySemaphore(gs->device, gs->render_finished[i], NULL);
        if (gs->framebuffers[i])
            vkDestroyFramebuffer(gs->device, gs->framebuffers[i], NULL);
        if (gs->views[i])
            vkDestroyImageView(gs->device, gs->views[i], NULL);
        gs->render_finished[i] = VK_NULL_HANDLE;
        gs->framebuffers[i] = VK_NULL_HANDLE;
        gs->views[i] = VK_NULL_HANDLE;
    }
    if (gs->swapchain) {
        vkDestroySwapchainKHR(gs->device, gs->swapchain, NULL);
        gs->swapchain = VK_NULL_HANDLE;
    }
    gs->present_mode = VK_PRESENT_MODE_FIFO_KHR;
    gs->image_count = 0;
    gs->recheck = false;
    gs->out_of_date = false;
    memset(&gs->traits, 0, sizeof(gs->traits));
}

/*
 * Read the present modes and the format of a surface. On Wayland, Mesa makes
 * a new registry for each of these queries and waits for the compositor three
 * times. Thus query one time for each surface, not one time for each frame.
 */
static bool surface_traits_ensure(struct render_gpu *g, struct gpu_surface *gs,
                                  VkSurfaceKHR surface)
{
    if (gs->traits.surface == surface)
        return true;
    memset(&gs->traits, 0, sizeof(gs->traits));

    /* Vulkan defines fewer present modes than this. VK_INCOMPLETE is not an error. */
    VkPresentModeKHR modes[16];
    uint32_t mode_count = 16;
    VkResult vr = vkGetPhysicalDeviceSurfacePresentModesKHR(g->physical_device, surface,
                                                            &mode_count, modes);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        mode_count = 0;
    for (uint32_t i = 0; i < mode_count; i++) {
        gs->traits.immediate |= modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
        gs->traits.mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
        gs->traits.fifo_relaxed |= modes[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }

    VkSurfaceFormatKHR formats_inline[32];
    VkSurfaceFormatKHR *formats = formats_inline;
    uint32_t format_count = 32;
    vr = vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical_device, surface, &format_count,
                                              formats);
    if (vr == VK_INCOMPLETE) {
        format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical_device, surface, &format_count, NULL);
        formats = calloc(format_count ? format_count : 1, sizeof(*formats));
        if (!formats)
            return false;
        vr = vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical_device, surface, &format_count,
                                                  formats);
    }
    bool ok = (vr == VK_SUCCESS || vr == VK_INCOMPLETE) && format_count;
    if (ok) {
        gs->traits.format = formats[0];
        for (uint32_t i = 0; i < format_count; i++) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
                gs->traits.format = formats[i];
                break;
            }
        }
        if (gs->traits.format.format == VK_FORMAT_UNDEFINED)
            gs->traits.format.format = VK_FORMAT_B8G8R8A8_UNORM;
        gs->traits.surface = surface;
    }
    if (formats != formats_inline)
        free(formats);
    return ok;
}

static bool surface_swapchain_ensure(struct render_gpu *g, struct gpu_surface *gs,
                                     VkSurfaceKHR surface, struct vector2i size, bool tearing,
                                     bool force)
{
    bool recreate = force || gs->out_of_date;

    /* Most frames keep the surface, the size, and the pacing. Ask the driver
     * nothing for these frames. */
    if (!recreate && gs->swapchain && gs->surface == surface && !gs->recheck &&
        vector2i_eq(gs->request_size, size) && gs->request_tearing == tearing)
        return true;

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->physical_device, surface, &caps) != VK_SUCCESS)
        return false;

    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = (uint32_t)geom_clamp_i(size.x, (int)caps.minImageExtent.width,
                                              (int)caps.maxImageExtent.width);
        extent.height = (uint32_t)geom_clamp_i(size.y, (int)caps.minImageExtent.height,
                                               (int)caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height)
        return false;

    if (!surface_traits_ensure(g, gs, surface))
        return false;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (tearing && gs->traits.immediate)
        present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (gs->traits.mailbox)
        present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (tearing && gs->traits.fifo_relaxed)
        present_mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;

    if (!recreate && gs->swapchain && gs->surface == surface &&
        gs->extent.width == extent.width && gs->extent.height == extent.height &&
        gs->present_mode == present_mode) {
        /* Record the extent of the swapchain. On X11, extent comes from the window
         * and can lag behind size. Recheck on the next frame if they differ. */
        gs->request_size = (struct vector2i){ .x = (int)extent.width, .y = (int)extent.height };
        gs->request_tearing = tearing;
        gs->recheck = (int)extent.width != size.x || (int)extent.height != size.y;
        return true;
    }
    VkSurfaceFormatKHR selected_format = gs->traits.format;

    uint32_t image_count;
    if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        image_count = caps.minImageCount;
    } else if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR) {
        image_count = caps.minImageCount > 3 ? caps.minImageCount : 3;
    } else {
        image_count = caps.minImageCount + 1;
    }
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainKHR old_swapchain = gs->swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = selected_format.format,
        .imageColorSpace = selected_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    };
    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(g->device, &sci, NULL, &new_swapchain) != VK_SUCCESS) {
        /* The surface can have changed. Query it again for the next frame. */
        memset(&gs->traits, 0, sizeof(gs->traits));
        gs->recheck = true;
        return false;
    }

    if (gs->retired.swapchain) {
        wait_submitted_frames(g);
        surface_swapchain_destroy_retired(gs);
    }

    if (old_swapchain) {
        gs->retired.swapchain = old_swapchain;
        gs->retired.image_count = gs->image_count;
        memcpy(gs->retired.views, gs->views, sizeof(gs->views));
        memcpy(gs->retired.framebuffers, gs->framebuffers, sizeof(gs->framebuffers));
        memcpy(gs->retired.render_finished, gs->render_finished, sizeof(gs->render_finished));
        memset(gs->views, 0, sizeof(gs->views));
        memset(gs->framebuffers, 0, sizeof(gs->framebuffers));
        memset(gs->render_finished, 0, sizeof(gs->render_finished));
    }

    gs->swapchain = new_swapchain;
    gs->surface = surface;
    gs->format = selected_format.format;
    gs->extent = extent;
    gs->present_mode = present_mode;
    gs->request_size = (struct vector2i){ .x = (int)extent.width, .y = (int)extent.height };
    gs->request_tearing = tearing;
    gs->recheck = (int)extent.width != size.x || (int)extent.height != size.y;
    gs->out_of_date = false;

    if (g->render_pass_format != gs->format) {
        wait_submitted_frames(g);
        if (!ensure_render_pass(g, gs->format) || !pipelines_create(g))
            return false;
    }

    uint32_t actual_images = 0;
    vkGetSwapchainImagesKHR(g->device, gs->swapchain, &actual_images, NULL);
    if (actual_images > SWAPCHAIN_IMAGES_MAX)
        actual_images = SWAPCHAIN_IMAGES_MAX;
    gs->image_count = actual_images;
    vkGetSwapchainImagesKHR(g->device, gs->swapchain, &actual_images, gs->images);

    for (uint32_t i = 0; i < gs->image_count; i++) {
        VkSemaphoreCreateInfo sci_sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(g->device, &sci_sem, NULL, &gs->render_finished[i]) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = gs->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = gs->format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        if (vkCreateImageView(g->device, &vi, NULL, &gs->views[i]) != VK_SUCCESS)
            return false;
        VkFramebufferCreateInfo fbi = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = g->render_pass,
            .attachmentCount = 1,
            .pAttachments = &gs->views[i],
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };
        if (vkCreateFramebuffer(g->device, &fbi, NULL, &gs->framebuffers[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

/* ---- Staging and submission --------------------------------------------- */

/*
 * Takes a slot. When all slots are busy and wait_ns is positive, wait for the
 * oldest slot fence. Return NULL when no slot becomes available.
 */
static struct frame_slot *slot_acquire(struct render_gpu *g, uint64_t wait_ns)
{
    for (int i = 0; i < FRAME_SLOTS; i++) {
        unsigned index = (g->next_slot + (unsigned)i) % FRAME_SLOTS;
        struct frame_slot *slot = &g->slots[index];
        if (vkGetFenceStatus(g->device, slot->fence) == VK_SUCCESS) {
            vkResetFences(g->device, 1, &slot->fence);
            g->next_slot = (index + 1) % FRAME_SLOTS;
            return slot;
        }
    }
    if (wait_ns) {
        struct frame_slot *oldest = &g->slots[g->next_slot];
        if (vkWaitForFences(g->device, 1, &oldest->fence, VK_TRUE, wait_ns) == VK_SUCCESS) {
            vkResetFences(g->device, 1, &oldest->fence);
            g->next_slot = (g->next_slot + 1) % FRAME_SLOTS;
            return oldest;
        }
    }
    return NULL;
}

static bool stage_uploads(struct render_gpu *g, struct frame_slot *slot)
{
    struct atlas *a = &g->atlas;
    size_t total = a->stage_used;
    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct image_entry *e = &g->images[i];
        if (e->image && e->needs_upload) {
            e->upload_offset = total;
            total += (size_t)e->size.x * (size_t)e->size.y * 4;
        }
    }
    if (!total)
        return true;
    if (!ensure_slot_buffer(g, &slot->upload_buffer, &slot->upload_memory, &slot->upload_mapped,
                            &slot->upload_cap, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    if (a->stage_used)
        memcpy(slot->upload_mapped, a->stage, a->stage_used);
    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct image_entry *e = &g->images[i];
        if (e->image && e->needs_upload) {
            size_t bytes = (size_t)e->size.x * (size_t)e->size.y * 4;
            memcpy((uint8_t *)slot->upload_mapped + e->upload_offset, e->pixels, bytes);
        }
    }
    return true;
}

static void encode_uploads(struct render_gpu *g, struct frame_slot *slot, VkCommandBuffer cmd)
{
    struct atlas *a = &g->atlas;
    VkImageMemoryBarrier2 pre_barriers[1 + IMAGE_CACHE];
    uint32_t n_pre = 0;
    VkImageMemoryBarrier2 post_barriers[1 + IMAGE_CACHE];
    uint32_t n_post = 0;

    if (!a->n_uploads) {
        if (a->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            post_barriers[n_post++] = (VkImageMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = a->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            a->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    } else {
        bool undef = (a->layout == VK_IMAGE_LAYOUT_UNDEFINED);
        pre_barriers[n_pre++] = (VkImageMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = undef ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = undef ? 0 : VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = a->layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = a->image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        post_barriers[n_post++] = (VkImageMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = a->image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
    }

    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct image_entry *e = &g->images[i];
        if (e->image && e->needs_upload) {
            pre_barriers[n_pre++] = (VkImageMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image = e->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            post_barriers[n_post++] = (VkImageMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = e->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
        }
    }

    if (n_pre > 0) {
        VkDependencyInfo pre_dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = n_pre,
            .pImageMemoryBarriers = pre_barriers,
        };
        g->pfn_vkCmdPipelineBarrier2(cmd, &pre_dep);
    }

    if (a->n_uploads) {
        vkCmdCopyBufferToImage(cmd, slot->upload_buffer, a->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, a->n_uploads, a->vk_copies);
        a->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct image_entry *e = &g->images[i];
        if (e->image && e->needs_upload) {
            VkBufferImageCopy cpy = {
                .bufferOffset = e->upload_offset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { 0, 0, 0 },
                .imageExtent = { (uint32_t)e->size.x, (uint32_t)e->size.y, 1 },
            };
            vkCmdCopyBufferToImage(cmd, slot->upload_buffer, e->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
            e->needs_upload = false;
        }
    }

    if (n_post > 0) {
        VkDependencyInfo post_dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = n_post,
            .pImageMemoryBarriers = post_barriers,
        };
        g->pfn_vkCmdPipelineBarrier2(cmd, &post_dep);
    }
}

static bool prepare_frame(struct render_gpu *g, struct frame_slot *slot, struct framebuffer *fb,
                          const struct render_frame *frame)
{
    bool built = false;
    for (int attempt = 0; attempt < 2 && !built; attempt++) {
        if (attempt && !atlas_recycle(g))
            break;
        built = build_frame(g, frame, fb->size);
        if (!built && !g->atlas.full)
            break;
    }
    glyph_cache_flush_frame();
    if (!built) {
        atlas_reset(g);
        return false;
    }
    size_t total = 0;
    for (int i = 0; i < ST_COUNT; i++) {
        g->stream_offsets[i] = total;
        total += align_up((size_t)g->streams[i].n * STREAM_STRIDE[i], STREAM_ALIGN);
    }
    if (!ensure_slot_buffer(g, &slot->instance_buffer, &slot->instance_memory,
                            &slot->instance_mapped, &slot->instance_cap,
                            total ? total : STREAM_ALIGN, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !stage_uploads(g, slot)) {
        atlas_reset(g);
        return false;
    }
    for (int i = 0; i < ST_COUNT; i++)
        if (g->streams[i].n)
            memcpy((uint8_t *)slot->instance_mapped + g->stream_offsets[i], g->streams[i].data,
                   (size_t)g->streams[i].n * STREAM_STRIDE[i]);
    return true;
}

static struct vulkan_drawable *frame_drawable(struct framebuffer *fb)
{
    if (!fb->drawable && fb->acquire)
        fb->drawable = fb->acquire(fb->acquire_ud);
    return (struct vulkan_drawable *)fb->drawable;
}

static bool encode_frame(struct render_gpu *g, struct frame_slot *slot, struct gpu_surface *gs,
                         struct framebuffer *fb, uint32_t image_index, uint32_t base)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(slot->cmd, &bi) != VK_SUCCESS)
        return false;

    encode_uploads(g, slot, slot->cmd);

    float base_col[4];
    color_of(base, 1.0f, base_col);
    VkClearValue clear = {
        .color = { .float32 = { base_col[0], base_col[1], base_col[2], base_col[3] } },
    };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g->render_pass,
        .framebuffer = gs->framebuffers[image_index],
        .renderArea = { .offset = { 0, 0 }, .extent = gs->extent },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(slot->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)fb->size.x, .height = (float)fb->size.y,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(slot->cmd, 0, 1, &vp);

    struct push_constants push = {
        .viewport_size = { (float)fb->size.x, (float)fb->size.y },
        .channel = 0,
    };
    vkCmdPushConstants(slot->cmd, g->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    const struct rect2i canvas_rect = { .size = fb->size };
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_ds = VK_NULL_HANDLE;
    VkDeviceSize bound_v_offset = (VkDeviceSize)-1;
    VkRect2D bound_scissor = {0};
    uint32_t bound_channel = 0;

    if (g->n_cmds > 0) {
        vkCmdBindDescriptorSets(slot->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g->pipeline_layout, 0, 1, &g->atlas.desc_set, 0, NULL);
        bound_ds = g->atlas.desc_set;
    }

    for (uint32_t i = 0; i < g->n_cmds; i++) {
        const struct cmd *c = &g->cmds[i];
        struct rect2i scissor = c->scissor;
        if (!c->count || !rect2i_clip(&scissor, canvas_rect.size))
            continue;
        VkRect2D vk_scissor = {
            .offset = { scissor.x, scissor.y },
            .extent = { (uint32_t)scissor.width, (uint32_t)scissor.height },
        };
        if (vk_scissor.offset.x != bound_scissor.offset.x ||
            vk_scissor.offset.y != bound_scissor.offset.y ||
            vk_scissor.extent.width != bound_scissor.extent.width ||
            vk_scissor.extent.height != bound_scissor.extent.height) {
            vkCmdSetScissor(slot->cmd, 0, 1, &vk_scissor);
            bound_scissor = vk_scissor;
        }

        if (c->kind != CMD_SHAPE) {
            VkDescriptorSet ds = c->desc_set ? c->desc_set : g->atlas.desc_set;
            if (ds != bound_ds) {
                vkCmdBindDescriptorSets(slot->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        g->pipeline_layout, 0, 1, &ds, 0, NULL);
                bound_ds = ds;
            }
        }

        VkDeviceSize v_offset = (VkDeviceSize)g->stream_offsets[c->stream];
        if (v_offset != bound_v_offset) {
            vkCmdBindVertexBuffers(slot->cmd, 0, 1, &slot->instance_buffer, &v_offset);
            bound_v_offset = v_offset;
        }

        int passes = c->kind == CMD_SUBPX ? 3 : 1;
        for (int ch = 0; ch < passes; ch++) {
            VkPipeline pipeline = c->kind == CMD_SHAPE ? g->shape_pipeline
                : c->kind == CMD_SUBPX ? g->subpx_pipeline[ch] : g->glyph_pipeline;
            if (pipeline != bound) {
                vkCmdBindPipeline(slot->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound = pipeline;
            }
            if (c->kind == CMD_SUBPX) {
                uint32_t channel = (uint32_t)ch;
                if (channel != bound_channel) {
                    // The update names every stage of the push constant range.
                    vkCmdPushConstants(slot->cmd, g->pipeline_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       (uint32_t)offsetof(struct push_constants, channel),
                                       (uint32_t)sizeof(channel), &channel);
                    bound_channel = channel;
                }
            }
            vkCmdDraw(slot->cmd, 6, c->count, 0, c->start);
        }
    }
    vkCmdEndRenderPass(slot->cmd);
    if (vkEndCommandBuffer(slot->cmd) != VK_SUCCESS)
        return false;

    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = slot->cmd,
    };
    VkSemaphoreSubmitInfo wait_sem = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = slot->image_acquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal_sem = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = gs->render_finished[image_index],
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait_sem,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_sem,
    };
    if (g->pfn_vkQueueSubmit2(g->queue, 1, &si, slot->fence) != VK_SUCCESS)
        return false;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &gs->render_finished[image_index],
        .swapchainCount = 1,
        .pSwapchains = &gs->swapchain,
        .pImageIndices = &image_index,
    };
    VkResult presented = vkQueuePresentKHR(g->queue, &pi);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
        gs->out_of_date = true;
        gs->recheck = true;
    } else if (presented == VK_SUBOPTIMAL_KHR) {
        gs->recheck = true;
    }

    g->atlas.stage_used = 0;
    g->atlas.n_uploads = 0;
    return true;
}

static bool slot_recreate_acquire_semaphore(struct render_gpu *g, struct frame_slot *slot)
{
    VkSemaphoreCreateInfo sci_sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (slot->image_acquired)
        vkDestroySemaphore(g->device, slot->image_acquired, NULL);
    slot->image_acquired = VK_NULL_HANDLE;
    return vkCreateSemaphore(g->device, &sci_sem, NULL, &slot->image_acquired) == VK_SUCCESS;
}

/*
 * Acquire the next swapchain image for a frame slot.
 * On VK_SUBOPTIMAL_KHR, check if the swapchain changed. Keep the acquired image
 * if the swapchain is unchanged. If the swapchain changed, recreate the slot
 * semaphore and acquire from the new swapchain.
 * On VK_ERROR_OUT_OF_DATE_KHR, force swapchain recreation and acquire again.
 */
static bool surface_acquire_image(struct render_gpu *g, struct frame_slot *slot,
                                  struct gpu_surface *gs, VkSurfaceKHR surface,
                                  struct vector2i size, bool unpaced,
                                  uint32_t *out_image_index)
{
    if (!gs || !gs->swapchain)
        return false;

    uint32_t image_index = 0;
    VkResult vr = vkAcquireNextImageKHR(g->device, gs->swapchain, UINT64_MAX,
                                        slot->image_acquired, VK_NULL_HANDLE,
                                        &image_index);
    if (vr == VK_SUCCESS) {
        *out_image_index = image_index;
        return true;
    }

    if (vr == VK_SUBOPTIMAL_KHR) {
        VkSwapchainKHR old_swapchain = gs->swapchain;
        gs->recheck = true;
        if (!surface_swapchain_ensure(g, gs, surface, size, unpaced, false)) {
            slot_recreate_acquire_semaphore(g, slot);
            return false;
        }
        if (gs->swapchain == old_swapchain) {
            *out_image_index = image_index;
            return true;
        }
        if (!slot_recreate_acquire_semaphore(g, slot))
            return false;
        vr = vkAcquireNextImageKHR(g->device, gs->swapchain, UINT64_MAX,
                                   slot->image_acquired, VK_NULL_HANDLE,
                                   &image_index);
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR)
            return false;
        if (vr == VK_SUBOPTIMAL_KHR)
            gs->recheck = true;
        *out_image_index = image_index;
        return true;
    }

    if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
        gs->recheck = true;
        if (!surface_swapchain_ensure(g, gs, surface, size, unpaced, true))
            return false;
        vr = vkAcquireNextImageKHR(g->device, gs->swapchain, UINT64_MAX,
                                   slot->image_acquired, VK_NULL_HANDLE,
                                   &image_index);
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR)
            return false;
        if (vr == VK_SUBOPTIMAL_KHR)
            gs->recheck = true;
        *out_image_index = image_index;
        return true;
    }

    return false;
}

enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame)
{
    if (!g || !fb || !frame || (!fb->drawable && !fb->acquire) || fb->size.x <= 0 ||
        fb->size.y <= 0)
        return RENDER_GPU_UNUSED;
    struct gpu_surface *gs = gpu_surface_of(g, s);
    bool resizing = fb->transaction || (gs && !vector2i_eq(gs->request_size, fb->size));
    struct frame_slot *slot = slot_acquire(g, resizing ? RESIZE_FENCE_WAIT_NS : 0);
    if (!slot)
        return RENDER_GPU_DROPPED;

    enum render_gpu_status status = RENDER_GPU_UNUSED;
    if (prepare_frame(g, slot, fb, frame)) {
        struct vulkan_drawable *drawable = frame_drawable(fb);
        if (!drawable || !drawable->surface) {
            atlas_reset(g);
            status = RENDER_GPU_DROPPED;
        } else {
            bool unpaced = drawable->unpaced || fb->transaction;
            if (!gs || !surface_swapchain_ensure(g, gs, drawable->surface, fb->size, unpaced, false)) {
                atlas_reset(g);
                status = RENDER_GPU_DROPPED;
            } else {
                uint32_t image_index = 0;
                if (!surface_acquire_image(g, slot, gs, drawable->surface, fb->size, unpaced,
                                           &image_index)) {
                    atlas_reset(g);
                    status = RENDER_GPU_DROPPED;
                } else if (encode_frame(g, slot, gs, fb, image_index, frame->base)) {
                    status = RENDER_GPU_PRESENTED;
                } else {
                    slot_recreate_acquire_semaphore(g, slot);
                    atlas_reset(g);
                }
            }
        }
    }
    if (status != RENDER_GPU_PRESENTED)
        vkQueueSubmit(g->queue, 0, NULL, slot->fence);
    return status;
}

/* ---- Presenting CPU pixels ---------------------------------------------- */

static bool surface_texture(struct render_gpu *g, struct gpu_surface *gs, struct vector2i size)
{
    if (gs->pixels_image && vector2i_eq(gs->pixels_size, size))
        return true;
    if (gs->pixels_image) {
        vkDestroyImageView(g->device, gs->pixels_view, NULL);
        vkDestroyImage(g->device, gs->pixels_image, NULL);
        vkFreeMemory(g->device, gs->pixels_memory, NULL);
        gs->pixels_image = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { (uint32_t)size.x, (uint32_t)size.y, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(g->device, &ii, NULL, &gs->pixels_image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(g->device, gs->pixels_image, &reqs);
    uint32_t type_idx = find_memory_type(g->physical_device, reqs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_idx == UINT32_MAX)
        type_idx = find_memory_type(g->physical_device, reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type_idx == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type_idx,
    };
    if (vkAllocateMemory(g->device, &ai, NULL, &gs->pixels_memory) != VK_SUCCESS)
        return false;
    if (vkBindImageMemory(g->device, gs->pixels_image, gs->pixels_memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = gs->pixels_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(g->device, &vi, NULL, &gs->pixels_view) != VK_SUCCESS)
        return false;

    if (!gs->pixels_desc_set) {
        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &g->desc_layout,
        };
        if (vkAllocateDescriptorSets(g->device, &dsai, &gs->pixels_desc_set) != VK_SUCCESS)
            return false;
    }
    VkDescriptorImageInfo image_info = {
        .sampler = g->sampler,
        .imageView = gs->pixels_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkDescriptorImageInfo dummy_info = {
        .sampler = g->sampler,
        .imageView = g->dummy_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = gs->pixels_desc_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = gs->pixels_desc_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dummy_info,
        },
    };
    vkUpdateDescriptorSets(g->device, 2, writes, 0, NULL);
    gs->pixels_size = size;
    return true;
}

static bool present_pixels(struct render_gpu *g, struct frame_slot *slot, struct render_surface *s,
                           struct framebuffer *fb, const struct damage_list *damage)
{
    struct gpu_surface *gs = gpu_surface_of(g, s);
    if (!gs)
        return false;
    struct vulkan_drawable *drawable = frame_drawable(fb);
    if (!drawable || !drawable->surface)
        return false;
    bool unpaced = drawable->unpaced || fb->transaction;
    if (!surface_swapchain_ensure(g, gs, drawable->surface, fb->size, unpaced, false))
        return false;

    bool fresh = !gs->pixels_image || !vector2i_eq(gs->pixels_size, fb->size);
    if (!surface_texture(g, gs, fb->size))
        return false;

    struct rect2i whole = { .size = fb->size };
    struct rect2i rects[DAMAGE_RECTS_MAX];
    int n = 0;
    if (fresh || !damage || damage->all) {
        rects[n++] = whole;
    } else if (damage->n < 0) {
        rects[n] = damage->bounds;
        if (rect2i_clip(&rects[n], fb->size))
            n++;
    } else {
        for (int i = 0; i < damage->n && i < DAMAGE_RECTS_MAX; i++) {
            rects[n] = damage->rects[i];
            if (rect2i_clip(&rects[n], fb->size))
                n++;
        }
    }
    size_t bytes = 0;
    for (int i = 0; i < n; i++)
        bytes += (size_t)rects[i].width * (size_t)rects[i].height * 4;

    if (!ensure_slot_buffer(g, &slot->instance_buffer, &slot->instance_memory,
                            &slot->instance_mapped, &slot->instance_cap, STREAM_ALIGN,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        (bytes && !ensure_slot_buffer(g, &slot->upload_buffer, &slot->upload_memory,
                                      &slot->upload_mapped, &slot->upload_cap, bytes,
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT)))
        return false;

    struct glyph_inst quad = {0};
    rect2i_to_vec4(whole, quad.rect);
    rect_to_vec4(0, 0, (float)fb->size.x, (float)fb->size.y, quad.uv);
    quad.color[0] = 1.0f;
    quad.color[1] = 1.0f;
    quad.color[2] = 1.0f;
    quad.color[3] = 1.0f;
    quad.mode = MODE_IMAGE;
    memcpy(slot->instance_mapped, &quad, sizeof(quad));

    uint32_t image_index = 0;
    if (!surface_acquire_image(g, slot, gs, drawable->surface, fb->size, unpaced,
                               &image_index))
        return false;

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(slot->cmd, &bi) != VK_SUCCESS) {
        slot_recreate_acquire_semaphore(g, slot);
        return false;
    }

    if (bytes) {
        bool undef = fresh || gs->pixels_layout == VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageMemoryBarrier2 pre_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = undef ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = undef ? 0 : VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = undef ? VK_IMAGE_LAYOUT_UNDEFINED : gs->pixels_layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = gs->pixels_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkDependencyInfo pre_dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        };
        g->pfn_vkCmdPipelineBarrier2(slot->cmd, &pre_dep);

        size_t offset = 0;
        VkBufferImageCopy copies[DAMAGE_RECTS_MAX];
        for (int i = 0; i < n; i++) {
            const struct rect2i *r = &rects[i];
            size_t row_bytes = (size_t)r->width * 4;
            if (r->width == fb->size.x) {
                memcpy((uint8_t *)slot->upload_mapped + offset,
                       fb->pixels + (size_t)r->y * (size_t)fb->size.x + (size_t)r->x,
                       row_bytes * (size_t)r->height);
            } else {
                for (int y = 0; y < r->height; y++)
                    memcpy((uint8_t *)slot->upload_mapped + offset + (size_t)y * row_bytes,
                           fb->pixels + (size_t)(r->y + y) * (size_t)fb->size.x + (size_t)r->x,
                           row_bytes);
            }
            copies[i] = (VkBufferImageCopy){
                .bufferOffset = offset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { r->x, r->y, 0 },
                .imageExtent = { (uint32_t)r->width, (uint32_t)r->height, 1 },
            };
            offset += row_bytes * (size_t)r->height;
        }
        vkCmdCopyBufferToImage(slot->cmd, slot->upload_buffer, gs->pixels_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)n, copies);

        VkImageMemoryBarrier2 post_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = gs->pixels_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkDependencyInfo post_dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &post_barrier,
        };
        g->pfn_vkCmdPipelineBarrier2(slot->cmd, &post_dep);
        gs->pixels_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkClearValue clear = {0};
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g->render_pass_copy ? g->render_pass_copy : g->render_pass,
        .framebuffer = gs->framebuffers[image_index],
        .renderArea = { .offset = { 0, 0 }, .extent = gs->extent },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(slot->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)fb->size.x, .height = (float)fb->size.y,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(slot->cmd, 0, 1, &vp);

    VkRect2D vk_scissor = {
        .offset = { 0, 0 },
        .extent = { (uint32_t)fb->size.x, (uint32_t)fb->size.y },
    };
    vkCmdSetScissor(slot->cmd, 0, 1, &vk_scissor);

    struct push_constants push = {
        .viewport_size = { (float)fb->size.x, (float)fb->size.y },
        .channel = 0,
    };
    vkCmdPushConstants(slot->cmd, g->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdBindDescriptorSets(slot->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g->pipeline_layout, 0, 1, &gs->pixels_desc_set, 0, NULL);
    VkDeviceSize v_offset = 0;
    vkCmdBindVertexBuffers(slot->cmd, 0, 1, &slot->instance_buffer, &v_offset);
    vkCmdBindPipeline(slot->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->copy_pipeline);
    vkCmdDraw(slot->cmd, 6, 1, 0, 0);

    vkCmdEndRenderPass(slot->cmd);
    if (vkEndCommandBuffer(slot->cmd) != VK_SUCCESS) {
        slot_recreate_acquire_semaphore(g, slot);
        return false;
    }

    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = slot->cmd,
    };
    VkSemaphoreSubmitInfo wait_sem = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = slot->image_acquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal_sem = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = gs->render_finished[image_index],
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait_sem,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_sem,
    };
    if (g->pfn_vkQueueSubmit2(g->queue, 1, &si, slot->fence) != VK_SUCCESS) {
        slot_recreate_acquire_semaphore(g, slot);
        return false;
    }

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &gs->render_finished[image_index],
        .swapchainCount = 1,
        .pSwapchains = &gs->swapchain,
        .pImageIndices = &image_index,
    };
    VkResult presented = vkQueuePresentKHR(g->queue, &pi);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
        gs->out_of_date = true;
        gs->recheck = true;
    } else if (presented == VK_SUBOPTIMAL_KHR) {
        gs->recheck = true;
    }
    return true;
}

bool render_gpu_present(struct render_gpu *g, struct render_surface *s, struct framebuffer *fb,
                        const struct damage_list *damage)
{
    if (!g || !s || !fb || (!fb->drawable && !fb->acquire) || !fb->pixels ||
        fb->size.x <= 0 || fb->size.y <= 0)
        return false;
    struct gpu_surface *gs = gpu_surface_of(g, s);
    bool resizing = fb->transaction || (gs && !vector2i_eq(gs->request_size, fb->size));
    struct frame_slot *slot = slot_acquire(g, resizing ? RESIZE_FENCE_WAIT_NS : 0);
    if (!slot)
        return false;
    bool presented = present_pixels(g, slot, s, fb, damage);
    if (!presented)
        vkQueueSubmit(g->queue, 0, NULL, slot->fence);
    return presented;
}

void render_gpu_surface_free(void *gpu_surface)
{
    struct gpu_surface *gs = gpu_surface;
    if (!gs)
        return;
    if (gs->device) {
        surface_swapchain_destroy(gs);
        if (gs->pixels_view)
            vkDestroyImageView(gs->device, gs->pixels_view, NULL);
        if (gs->pixels_image)
            vkDestroyImage(gs->device, gs->pixels_image, NULL);
        if (gs->pixels_memory)
            vkFreeMemory(gs->device, gs->pixels_memory, NULL);
        if (gs->pixels_desc_set && gs->desc_pool)
            vkFreeDescriptorSets(gs->device, gs->desc_pool, 1, &gs->pixels_desc_set);
    }
    free(gs);
}

/* ---- Creation and teardown ---------------------------------------------- */

static bool gpu_disabled(void)
{
    const char *env = getenv("WSAP_RENDER_GPU");
    return env && (!strcmp(env, "0") || !strcmp(env, "off"));
}

struct render_gpu *render_gpu_new(void)
{
    if (gpu_disabled())
        return NULL;

    uint32_t ext_count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL) != VK_SUCCESS || !ext_count) {
        fprintf(stderr, "[vulkan] enumerate instance extensions failed\n");
        return NULL;
    }
    VkExtensionProperties *avail_exts = calloc(ext_count, sizeof(*avail_exts));
    if (!avail_exts)
        return NULL;
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, avail_exts);

    const char *req_exts[16];
    uint32_t req_count = 0;
    bool has_portability = false;
    for (uint32_t i = 0; i < ext_count && req_count < 16; i++) {
        const char *name = avail_exts[i].extensionName;
        if (!strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME)) {
            req_exts[req_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
        } else if (!strcmp(name, "VK_KHR_wayland_surface")) {
            req_exts[req_count++] = "VK_KHR_wayland_surface";
        } else if (!strcmp(name, "VK_KHR_xlib_surface")) {
            req_exts[req_count++] = "VK_KHR_xlib_surface";
        } else if (!strcmp(name, "VK_KHR_xcb_surface")) {
            req_exts[req_count++] = "VK_KHR_xcb_surface";
        } else if (!strcmp(name, "VK_EXT_metal_surface")) {
            req_exts[req_count++] = "VK_EXT_metal_surface";
        } else if (!strcmp(name, "VK_KHR_portability_enumeration")) {
            has_portability = true;
            req_exts[req_count++] = "VK_KHR_portability_enumeration";
        }
    }
    free(avail_exts);

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "wsap_term",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "wsap_vulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = has_portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0,
        .pApplicationInfo = &app,
        .enabledExtensionCount = req_count,
        .ppEnabledExtensionNames = req_exts,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vr = vkCreateInstance(&ici, NULL, &instance);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] vkCreateInstance failed: %d\n", vr);
        return NULL;
    }

    uint32_t phys_count = 0;
    vkEnumeratePhysicalDevices(instance, &phys_count, NULL);
    if (!phys_count) {
        vkDestroyInstance(instance, NULL);
        return NULL;
    }
    VkPhysicalDevice *devices = calloc(phys_count, sizeof(*devices));
    if (!devices) {
        vkDestroyInstance(instance, NULL);
        return NULL;
    }
    vkEnumeratePhysicalDevices(instance, &phys_count, devices);

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t selected_qf = UINT32_MAX;
    int best_score = -1;

    for (uint32_t i = 0; i < phys_count; i++) {
        VkPhysicalDevice pd = devices[i];

        uint32_t dev_ext_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &dev_ext_count, NULL);
        if (!dev_ext_count)
            continue;
        VkExtensionProperties *dev_exts = calloc(dev_ext_count, sizeof(*dev_exts));
        if (!dev_exts)
            continue;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &dev_ext_count, dev_exts);
        bool has_swapchain = false;
        for (uint32_t e = 0; e < dev_ext_count; e++)
            if (!strcmp(dev_exts[e].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                has_swapchain = true;
        free(dev_exts);
        if (!has_swapchain)
            continue;

        VkPhysicalDeviceSynchronization2Features sync2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        };
        VkPhysicalDeviceFeatures2 feats2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &sync2,
        };
        vkGetPhysicalDeviceFeatures2(pd, &feats2);
        if (sync2.synchronization2 != VK_TRUE)
            continue;

        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, NULL);
        VkQueueFamilyProperties *qfs = qf_count ? calloc(qf_count, sizeof(*qfs)) : NULL;
        if (qfs)
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs);
        uint32_t qf = UINT32_MAX;
        for (uint32_t q = 0; q < qf_count; q++) {
            if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                qf = q;
                break;
            }
        }
        free(qfs);
        if (qf == UINT32_MAX)
            continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        int score = 10;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score += 100;
        if (score > best_score) {
            best_score = score;
            selected = pd;
            selected_qf = qf;
        }
    }
    free(devices);
    if (!selected) {
        fprintf(stderr, "[vulkan] no suitable physical device found (phys_count=%u)\n", phys_count);
        vkDestroyInstance(instance, NULL);
        return NULL;
    }

    float q_priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = selected_qf,
        .queueCount = 1,
        .pQueuePriorities = &q_priority,
    };
    const char *dev_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkPhysicalDeviceSynchronization2Features sync2_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2 = VK_TRUE,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &sync2_enable,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &dev_ext,
    };
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(selected, &dci, NULL, &device) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        return NULL;
    }

    struct render_gpu *g = calloc(1, sizeof(*g));
    if (!g) {
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return NULL;
    }
    g->instance = instance;
    g->physical_device = selected;
    g->device = device;
    g->queue_family = selected_qf;
    vkGetDeviceQueue(device, selected_qf, 0, &g->queue);

    g->pfn_vkQueueSubmit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(device, "vkQueueSubmit2");
    if (!g->pfn_vkQueueSubmit2)
        g->pfn_vkQueueSubmit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(device, "vkQueueSubmit2KHR");
    g->pfn_vkCmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2");
    if (!g->pfn_vkCmdPipelineBarrier2)
        g->pfn_vkCmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2KHR");
    if (!g->pfn_vkQueueSubmit2 || !g->pfn_vkCmdPipelineBarrier2) {
        render_gpu_free(g);
        return NULL;
    }

    g->device_info = (struct vulkan_device){
        .instance = instance,
        .physical_device = selected,
        .device = device,
        .queue = g->queue,
        .queue_family = selected_qf,
    };

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = selected_qf,
    };
    if (vkCreateCommandPool(device, &cpci, NULL, &g->cmd_pool) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }

    VkDescriptorPoolSize pool_sizes[1] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 128 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 64,
        .poolSizeCount = 1,
        .pPoolSizes = pool_sizes,
    };
    if (vkCreateDescriptorPool(device, &dpci, NULL, &g->desc_pool) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }

    VkDescriptorSetLayoutBinding dslb[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = dslb,
    };
    if (vkCreateDescriptorSetLayout(device, &dslci, NULL, &g->desc_layout) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(struct push_constants),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g->desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(device, &plci, NULL, &g->pipeline_layout) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(device, &sci, NULL, &g->sampler) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }

    /* Dummy 1x1 image for descriptor binding 1 fallback. */
    VkImageCreateInfo dii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { 1, 1, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(device, &dii, NULL, &g->dummy_image) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device, g->dummy_image, &reqs);
    uint32_t type_idx = find_memory_type(selected, reqs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_idx == UINT32_MAX)
        type_idx = find_memory_type(selected, reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo dai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type_idx,
    };
    if (vkAllocateMemory(device, &dai, NULL, &g->dummy_memory) != VK_SUCCESS ||
        vkBindImageMemory(device, g->dummy_image, g->dummy_memory, 0) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }
    VkImageViewCreateInfo dvi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = g->dummy_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(device, &dvi, NULL, &g->dummy_view) != VK_SUCCESS) {
        render_gpu_free(g);
        return NULL;
    }

    for (int i = 0; i < FRAME_SLOTS; i++) {
        struct frame_slot *slot = &g->slots[i];
        VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        VkSemaphoreCreateInfo sci_sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g->cmd_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkCreateFence(device, &fci, NULL, &slot->fence) != VK_SUCCESS ||
            vkCreateSemaphore(device, &sci_sem, NULL, &slot->image_acquired) != VK_SUCCESS ||
            vkAllocateCommandBuffers(device, &cbai, &slot->cmd) != VK_SUCCESS) {
            render_gpu_free(g);
            return NULL;
        }
    }

    /* Transition dummy image to SHADER_READ_ONLY_OPTIMAL. */
    VkCommandBuffer cmd = g->slots[0].cmd;
    VkCommandBufferBeginInfo cb_begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cmd, &cb_begin) == VK_SUCCESS) {
        VkImageMemoryBarrier2 dib = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = g->dummy_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkDependencyInfo did = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &dib,
        };
        g->pfn_vkCmdPipelineBarrier2(cmd, &did);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cbsi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = cmd,
        };
        VkSubmitInfo2 si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cbsi,
        };
        g->pfn_vkQueueSubmit2(g->queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(g->queue);
    }

    VkPipelineCacheCreateInfo pcci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
    };
    if (vkCreatePipelineCache(device, &pcci, NULL, &g->pipeline_cache) != VK_SUCCESS)
        g->pipeline_cache = VK_NULL_HANDLE;

    g->streams[ST_BG].cap = 2048;
    g->streams[ST_BG].data = malloc(g->streams[ST_BG].cap * STREAM_STRIDE[ST_BG]);
    g->streams[ST_INK].cap = 2048;
    g->streams[ST_INK].data = malloc(g->streams[ST_INK].cap * STREAM_STRIDE[ST_INK]);
    g->streams[ST_SUBPX].cap = 2048;
    g->streams[ST_SUBPX].data = malloc(g->streams[ST_SUBPX].cap * STREAM_STRIDE[ST_SUBPX]);
    g->streams[ST_DECO].cap = 512;
    g->streams[ST_DECO].data = malloc(g->streams[ST_DECO].cap * STREAM_STRIDE[ST_DECO]);
    g->streams[ST_UI].cap = 512;
    g->streams[ST_UI].data = malloc(g->streams[ST_UI].cap * STREAM_STRIDE[ST_UI]);
    g->streams[ST_SHAPE].cap = 256;
    g->streams[ST_SHAPE].data = malloc(g->streams[ST_SHAPE].cap * STREAM_STRIDE[ST_SHAPE]);

    g->cap_cmds = 128;
    g->cmds = malloc(g->cap_cmds * sizeof(*g->cmds));

    g->atlas.cap_uploads = 256;
    g->atlas.uploads = malloc(g->atlas.cap_uploads * sizeof(*g->atlas.uploads));
    g->atlas.vk_copies = malloc(g->atlas.cap_uploads * sizeof(*g->atlas.vk_copies));

    g->atlas.stage_cap = 1024 * 1024;
    g->atlas.stage = malloc(g->atlas.stage_cap);

    g->cap_clips = 32;
    g->clips = malloc(g->cap_clips * sizeof(*g->clips));

    bool alloc_ok = g->cmds && g->atlas.uploads && g->atlas.vk_copies &&
                    g->atlas.stage && g->clips;
    for (int i = 0; i < ST_COUNT; i++)
        if (!g->streams[i].data)
            alloc_ok = false;

    g->decor = decor_cache_create();
    g->atlas.map = calloc(ATLAS_MAP_SIZE, sizeof(*g->atlas.map));
    if (!alloc_ok || !g->decor || !g->atlas.map || !atlas_make_texture(g, ATLAS_START_SIZE) ||
        !pipelines_create(g)) {
        render_gpu_free(g);
        return NULL;
    }
    return g;
}

void render_gpu_free(struct render_gpu *g)
{
    if (!g)
        return;
    if (g->device)
        vkDeviceWaitIdle(g->device);

    for (int i = 0; i < FRAME_SLOTS; i++) {
        struct frame_slot *slot = &g->slots[i];
        if (slot->fence)
            vkDestroyFence(g->device, slot->fence, NULL);
        if (slot->image_acquired)
            vkDestroySemaphore(g->device, slot->image_acquired, NULL);
        if (slot->instance_mapped)
            vkUnmapMemory(g->device, slot->instance_memory);
        if (slot->instance_buffer)
            vkDestroyBuffer(g->device, slot->instance_buffer, NULL);
        if (slot->instance_memory)
            vkFreeMemory(g->device, slot->instance_memory, NULL);
        if (slot->upload_mapped)
            vkUnmapMemory(g->device, slot->upload_memory);
        if (slot->upload_buffer)
            vkDestroyBuffer(g->device, slot->upload_buffer, NULL);
        if (slot->upload_memory)
            vkFreeMemory(g->device, slot->upload_memory, NULL);
    }
    if (g->glyph_pipeline)
        vkDestroyPipeline(g->device, g->glyph_pipeline, NULL);
    for (int i = 0; i < 3; i++)
        if (g->subpx_pipeline[i])
            vkDestroyPipeline(g->device, g->subpx_pipeline[i], NULL);
    if (g->shape_pipeline)
        vkDestroyPipeline(g->device, g->shape_pipeline, NULL);
    if (g->copy_pipeline)
        vkDestroyPipeline(g->device, g->copy_pipeline, NULL);
    if (g->pipeline_layout)
        vkDestroyPipelineLayout(g->device, g->pipeline_layout, NULL);
    if (g->pipeline_cache)
        vkDestroyPipelineCache(g->device, g->pipeline_cache, NULL);
    if (g->render_pass)
        vkDestroyRenderPass(g->device, g->render_pass, NULL);
    if (g->render_pass_copy)
        vkDestroyRenderPass(g->device, g->render_pass_copy, NULL);
    if (g->sampler)
        vkDestroySampler(g->device, g->sampler, NULL);
    if (g->desc_layout)
        vkDestroyDescriptorSetLayout(g->device, g->desc_layout, NULL);
    if (g->desc_pool)
        vkDestroyDescriptorPool(g->device, g->desc_pool, NULL);

    if (g->dummy_view)
        vkDestroyImageView(g->device, g->dummy_view, NULL);
    if (g->dummy_image)
        vkDestroyImage(g->device, g->dummy_image, NULL);
    if (g->dummy_memory)
        vkFreeMemory(g->device, g->dummy_memory, NULL);

    if (g->atlas.view)
        vkDestroyImageView(g->device, g->atlas.view, NULL);
    if (g->atlas.image)
        vkDestroyImage(g->device, g->atlas.image, NULL);
    if (g->atlas.memory)
        vkFreeMemory(g->device, g->atlas.memory, NULL);

    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct image_entry *e = &g->images[i];
        if (e->view)
            vkDestroyImageView(g->device, e->view, NULL);
        if (e->image)
            vkDestroyImage(g->device, e->image, NULL);
        if (e->memory)
            vkFreeMemory(g->device, e->memory, NULL);
    }
    if (g->cmd_pool)
        vkDestroyCommandPool(g->device, g->cmd_pool, NULL);
    if (g->device)
        vkDestroyDevice(g->device, NULL);
    if (g->instance)
        vkDestroyInstance(g->instance, NULL);

    for (int i = 0; i < ST_COUNT; i++)
        free(g->streams[i].data);
    free(g->cmds);
    free(g->clips);
    free(g->atlas.map);
    free(g->atlas.stage);
    free(g->atlas.uploads);
    free(g->atlas.vk_copies);
    decor_cache_destroy(g->decor);
    free(g);
}

void *render_gpu_native_device(struct render_gpu *g)
{
    return g ? (void *)&g->device_info : NULL;
}
