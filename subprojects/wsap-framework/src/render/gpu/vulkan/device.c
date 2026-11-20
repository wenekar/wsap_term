#define _GNU_SOURCE
#include "render/gpu/vulkan/internal.h"
#include "util/app_identity.h"

/* The app identity gives the tag that this backend puts on its log lines. */
static const char *log_tag(void)
{
    const struct app_identity *identity = app_identity_current();
    return identity ? identity->id : "vk";
}

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "render/gpu/gpu.h"

#include "vulkan_shaders_spv.h"

/* The instance, the device, the pipelines, and the resource helpers. */

void vulkan_wait_idle(struct render_gpu *g)
{
    g->vk.QueueWaitIdle(g->dev.queue);
}

/* ---- Textures and buffers ----------------------------------------------- */

/* The number of sets of the first descriptor pool. */
#define VULKAN_DESC_SETS_START 64

/*
 * Adds a descriptor pool to `g`. The pool takes twice the sets of the last
 * one, so a device with many textures adds a few pools only. Returns false
 * and reports the reason when the device cannot make the pool.
 */
static bool desc_pool_add(struct render_gpu *g)
{
    uint32_t sets = g->n_desc_pools
        ? g->desc_pools[g->n_desc_pools - 1].sets * 2 : VULKAN_DESC_SETS_START;
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = sets,
    };
    VkDescriptorPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = sets,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    if (g->n_desc_pools == g->cap_desc_pools) {
        uint32_t cap = g->cap_desc_pools ? g->cap_desc_pools * 2 : 4;
        struct vulkan_desc_pool *pools = realloc(g->desc_pools, (size_t)cap * sizeof(*pools));
        if (!pools) {
            fprintf(stderr, "%s: no memory for a Vulkan descriptor pool of %u sets\n",
                    log_tag(), sets);
            return false;
        }
        g->desc_pools = pools;
        g->cap_desc_pools = cap;
    }
    struct vulkan_desc_pool *entry = &g->desc_pools[g->n_desc_pools];
    if (g->vk.CreateDescriptorPool(g->dev.device, &info, NULL, &entry->pool) != VK_SUCCESS) {
        fprintf(stderr, "%s: the Vulkan device makes no descriptor pool of %u sets\n",
                    log_tag(), sets);
        return false;
    }
    entry->sets = sets;
    g->n_desc_pools++;
    return true;
}

/*
 * Allocates one descriptor set with the layout of the device. The set comes
 * from the newest pool that has room. When every pool is full, a new pool
 * of twice the sets takes it. Returns false when no pool gives a set; a
 * failure of the new pool is reported.
 */
static bool desc_set_alloc(struct render_gpu *g, VkDescriptorSet *set, VkDescriptorPool *pool)
{
    VkDescriptorSetAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pSetLayouts = &g->desc_layout,
    };
    for (uint32_t i = g->n_desc_pools; i-- > 0;) {
        info.descriptorPool = g->desc_pools[i].pool;
        VkResult result = g->vk.AllocateDescriptorSets(g->dev.device, &info, set);
        if (result == VK_SUCCESS) {
            *pool = info.descriptorPool;
            return true;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY)
            return false;
    }
    if (!desc_pool_add(g))
        return false;
    info.descriptorPool = g->desc_pools[g->n_desc_pools - 1].pool;
    if (g->vk.AllocateDescriptorSets(g->dev.device, &info, set) != VK_SUCCESS) {
        fprintf(stderr, "%s: the new Vulkan descriptor pool gives no set\n",
                    log_tag());
        return false;
    }
    *pool = info.descriptorPool;
    return true;
}

static uint32_t memory_type(struct render_gpu *g, uint32_t type_bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props;
    g->vk.GetPhysicalDeviceMemoryProperties(g->dev.physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Allocates memory of a type with `want`. With `any`, a type without those
 * properties is acceptable when no other type fits. */
static bool memory_allocate(struct render_gpu *g, const VkMemoryRequirements *reqs,
                            VkMemoryPropertyFlags want, bool any, VkDeviceMemory *out)
{
    uint32_t type = memory_type(g, reqs->memoryTypeBits, want);
    if (type == UINT32_MAX && any)
        type = memory_type(g, reqs->memoryTypeBits, 0);
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs->size,
        .memoryTypeIndex = type,
    };
    return g->vk.AllocateMemory(g->dev.device, &info, NULL, out) == VK_SUCCESS;
}

/* Destroys the image, the view, and the memory of texture `t` and keeps its
 * descriptor set. */
static void texture_drop_image(struct render_gpu *g, struct vulkan_texture *t)
{
    VkDevice device = g->dev.device;
    if (t->view)
        g->vk.DestroyImageView(device, t->view, NULL);
    if (t->image)
        g->vk.DestroyImage(device, t->image, NULL);
    if (t->memory)
        g->vk.FreeMemory(device, t->memory, NULL);
    VkDescriptorSet set = t->set;
    VkDescriptorPool pool = t->pool;
    *t = (struct vulkan_texture){ .set = set, .pool = pool };
}

static bool texture_make_image(struct render_gpu *g, struct vulkan_texture *t,
                               struct vector2i size)
{
    VkDevice device = g->dev.device;
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = TEXEL_FORMAT,
        .extent = { (uint32_t)size.x, (uint32_t)size.y, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (g->vk.CreateImage(device, &image_info, NULL, &t->image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements reqs;
    g->vk.GetImageMemoryRequirements(device, t->image, &reqs);
    if (!memory_allocate(g, &reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, &t->memory) ||
        g->vk.BindImageMemory(device, t->image, t->memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = TEXEL_FORMAT,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (g->vk.CreateImageView(device, &view_info, NULL, &t->view) != VK_SUCCESS)
        return false;
    if (!t->set) {
        if (!desc_set_alloc(g, &t->set, &t->pool))
            return false;
    }
    VkDescriptorImageInfo sampled = {
        .sampler = g->sampler,
        .imageView = t->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = t->set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &sampled,
    };
    g->vk.UpdateDescriptorSets(device, 1, &write, 0, NULL);
    t->size = size;
    return true;
}

bool vulkan_texture_ensure(struct render_gpu *g, struct vulkan_texture *t, struct vector2i size)
{
    if (t->image && vector2i_eq(t->size, size))
        return true;
    if (t->image) {
        /* A submitted frame can still sample the old image. */
        vulkan_wait_idle(g);
        texture_drop_image(g, t);
    }
    if (texture_make_image(g, t, size))
        return true;
    texture_drop_image(g, t);
    return false;
}

void vulkan_texture_destroy(struct render_gpu *g, struct vulkan_texture *t)
{
    texture_drop_image(g, t);
    if (t->set)
        g->vk.FreeDescriptorSets(g->dev.device, t->pool, 1, &t->set);
    *t = (struct vulkan_texture){0};
}

void vulkan_buffer_destroy(struct render_gpu *g, struct vulkan_buffer *b)
{
    VkDevice device = g->dev.device;
    if (b->mapped)
        g->vk.UnmapMemory(device, b->memory);
    if (b->buffer)
        g->vk.DestroyBuffer(device, b->buffer, NULL);
    if (b->memory)
        g->vk.FreeMemory(device, b->memory, NULL);
    *b = (struct vulkan_buffer){0};
}

bool vulkan_buffer_ensure(struct render_gpu *g, struct vulkan_buffer *b, size_t bytes,
                          VkBufferUsageFlags usage)
{
    if (b->buffer && b->cap >= bytes)
        return true;
    size_t cap = b->cap ? b->cap : 65536;
    while (cap < bytes)
        cap *= 2;
    vulkan_buffer_destroy(g, b);

    VkDevice device = g->dev.device;
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = cap,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkMemoryRequirements reqs;
    bool ok = g->vk.CreateBuffer(device, &info, NULL, &b->buffer) == VK_SUCCESS;
    if (ok) {
        g->vk.GetBufferMemoryRequirements(device, b->buffer, &reqs);
        ok = memory_allocate(g, &reqs,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             false, &b->memory) &&
             g->vk.BindBufferMemory(device, b->buffer, b->memory, 0) == VK_SUCCESS &&
             g->vk.MapMemory(device, b->memory, 0, cap, 0, &b->mapped) == VK_SUCCESS;
    }
    if (!ok) {
        b->mapped = NULL;
        vulkan_buffer_destroy(g, b);
        return false;
    }
    b->cap = cap;
    return true;
}

/* ---- Pipelines ---------------------------------------------------------- */

enum shader_id {
    SHADER_GLYPH_VERT,
    SHADER_GLYPH_FRAG,
    SHADER_SHAPE_VERT,
    SHADER_SHAPE_FRAG,
    SHADER_COPY_FRAG,
    SHADER_COUNT,
};

#define ALL_CHANNELS                                                                    \
    (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |  \
     VK_COLOR_COMPONENT_A_BIT)

static const struct pipeline_spec {
    enum shader_id vert, frag;
    /* The instances are gpu_shape_inst, not gpu_glyph_inst. */
    bool shape;
    /* -1: no blending. Else one of enum gpu_blend. */
    int blend;
    VkColorComponentFlags mask;
} PIPELINES[PIPE_COUNT] = {
    [PIPE_GLYPH + GPU_BLEND_PREMUL] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false,
                                        GPU_BLEND_PREMUL, ALL_CHANNELS },
    [PIPE_GLYPH + GPU_BLEND_ALPHA] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false,
                                       GPU_BLEND_ALPHA, ALL_CHANNELS },
    [PIPE_GLYPH + GPU_BLEND_ADDITIVE] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false,
                                          GPU_BLEND_ADDITIVE, ALL_CHANNELS },
    [PIPE_GLYPH + GPU_BLEND_MULTIPLY] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false,
                                          GPU_BLEND_MULTIPLY, ALL_CHANNELS },
    [PIPE_GLYPH + GPU_BLEND_NONE] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false,
                                      GPU_BLEND_NONE, ALL_CHANNELS },
    [PIPE_SUBPX + 0] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false, GPU_BLEND_PREMUL,
                         VK_COLOR_COMPONENT_R_BIT },
    [PIPE_SUBPX + 1] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false, GPU_BLEND_PREMUL,
                         VK_COLOR_COMPONENT_G_BIT },
    [PIPE_SUBPX + 2] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false, GPU_BLEND_PREMUL,
                         VK_COLOR_COMPONENT_B_BIT },
    [PIPE_SHAPE] = { SHADER_SHAPE_VERT, SHADER_SHAPE_FRAG, true, GPU_BLEND_PREMUL, ALL_CHANNELS },
    [PIPE_COPY] = { SHADER_GLYPH_VERT, SHADER_COPY_FRAG, false, -1, ALL_CHANNELS },
};

/*
 * The color blend state of `blend`. It gives the same 8-bit sRGB expression
 * as blend_mode_px() in render/cpu/scene.c. A fixed-point attachment clamps
 * the result, so ADDITIVE needs no clamp in the shader.
 */
static VkPipelineColorBlendAttachmentState blend_attachment(int blend)
{
    VkPipelineColorBlendAttachmentState a = {
        .blendEnable = blend >= 0 && blend != GPU_BLEND_NONE ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    if (blend == GPU_BLEND_ADDITIVE) {
        /* src + dst, for the alpha channel too. */
        a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (blend == GPU_BLEND_MULTIPLY) {
        /* src * dst. The alpha blends as PREMUL. */
        a.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        a.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    return a;
}

static const VkVertexInputAttributeDescription GLYPH_ATTRIBS[] = {
    { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_glyph_inst, rect) },
    { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_glyph_inst, uv) },
    { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_glyph_inst, color) },
    { 3, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_glyph_inst, mode) },
    { 4, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_glyph_inst, blend) },
};

static const VkVertexInputAttributeDescription SHAPE_ATTRIBS[] = {
    { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_shape_inst, rect) },
    { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_shape_inst, color) },
    { 2, 0, VK_FORMAT_R32_SFLOAT, offsetof(struct gpu_shape_inst, radius) },
    { 3, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_shape_inst, mode) },
    { 4, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_shape_inst, blur) },
    { 5, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_shape_inst, blend) },
};

static VkPipeline pipeline_make(struct render_gpu *g, const struct pipeline_spec *spec,
                                const VkShaderModule *shaders, VkFormat format)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = shaders[spec->vert],
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = shaders[spec->frag],
            .pName = "main",
        },
    };
    /* One instance for each quad. The vertex shader makes the corners. */
    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = spec->shape ? sizeof(struct gpu_shape_inst) : sizeof(struct gpu_glyph_inst),
        .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
    };
    VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = spec->shape
            ? sizeof(SHAPE_ATTRIBS) / sizeof(SHAPE_ATTRIBS[0])
            : sizeof(GLYPH_ATTRIBS) / sizeof(GLYPH_ATTRIBS[0]),
        .pVertexAttributeDescriptions = spec->shape ? SHAPE_ATTRIBS : GLYPH_ATTRIBS,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState attachment = blend_attachment(spec->blend);
    attachment.colorWriteMask = spec->mask;
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    /* Dynamic rendering: the pipeline names the format, not a render pass. */
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = g->pipeline_layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (g->vk.CreateGraphicsPipelines(g->dev.device, g->pipeline_cache, 1, &info, NULL,
                                      &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

static void pipelines_destroy(struct render_gpu *g)
{
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (g->pipelines[i])
            g->vk.DestroyPipeline(g->dev.device, g->pipelines[i], NULL);
        g->pipelines[i] = VK_NULL_HANDLE;
    }
    g->pipeline_format = VK_FORMAT_UNDEFINED;
}

bool vulkan_pipelines_ensure(struct render_gpu *g, VkFormat format)
{
    if (g->pipeline_format == format)
        return true;
    if (g->pipeline_format != VK_FORMAT_UNDEFINED)
        vulkan_wait_idle(g);
    pipelines_destroy(g);

    const struct {
        const uint32_t *code;
        size_t size;
    } sources[SHADER_COUNT] = {
        [SHADER_GLYPH_VERT] = { glyph_vert_spv, glyph_vert_spv_size },
        [SHADER_GLYPH_FRAG] = { glyph_frag_spv, glyph_frag_spv_size },
        [SHADER_SHAPE_VERT] = { shape_vert_spv, shape_vert_spv_size },
        [SHADER_SHAPE_FRAG] = { shape_frag_spv, shape_frag_spv_size },
        [SHADER_COPY_FRAG] = { copy_frag_spv, copy_frag_spv_size },
    };
    VkShaderModule shaders[SHADER_COUNT] = {0};
    bool ok = true;
    for (int i = 0; i < SHADER_COUNT && ok; i++) {
        VkShaderModuleCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sources[i].size,
            .pCode = sources[i].code,
        };
        ok = g->vk.CreateShaderModule(g->dev.device, &info, NULL, &shaders[i]) == VK_SUCCESS;
    }
    for (int i = 0; i < PIPE_COUNT && ok; i++) {
        g->pipelines[i] = pipeline_make(g, &PIPELINES[i], shaders, format);
        ok = g->pipelines[i] != VK_NULL_HANDLE;
    }
    for (int i = 0; i < SHADER_COUNT; i++)
        if (shaders[i])
            g->vk.DestroyShaderModule(g->dev.device, shaders[i], NULL);
    if (!ok) {
        pipelines_destroy(g);
        return false;
    }
    g->pipeline_format = format;
    return true;
}

/* ---- The instance and the device ---------------------------------------- */

enum gpu_choice {
    GPU_OFF,
    GPU_HARDWARE,
    /* A software Vulkan device is permitted too. For tests. */
    GPU_SOFTWARE,
};

/* Evaluates the WSAP_RENDER_GPU environment variable to select GPU backend mode. */
static enum gpu_choice gpu_choice(void)
{
    const char *env = getenv("WSAP_RENDER_GPU");
    if (env && (!strcmp(env, "0") || !strcmp(env, "off")))
        return GPU_OFF;
    return env && !strcmp(env, "software") ? GPU_SOFTWARE : GPU_HARDWARE;
}

static bool has_extension(const VkExtensionProperties *list, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(list[i].extensionName, name))
            return true;
    return false;
}

struct vk_sym {
    size_t offset;
    const char *name;
};

#define VK_SYM(field) { offsetof(struct vulkan_fns, field), "vk" #field }

static bool resolve_instance_syms(struct render_gpu *g, VkInstance instance,
                                  const struct vk_sym *syms, size_t count)
{
    char *base = (char *)&g->vk;
    for (size_t i = 0; i < count; i++) {
        PFN_vkVoidFunction fn = g->dev.get_instance_proc_addr(instance, syms[i].name);
        if (!fn)
            return false;
        memcpy(base + syms[i].offset, &fn, sizeof(fn));
    }
    return true;
}

static bool resolve_device_syms(struct render_gpu *g, const struct vk_sym *syms, size_t count)
{
    char *base = (char *)&g->vk;
    for (size_t i = 0; i < count; i++) {
        PFN_vkVoidFunction fn = g->vk.GetDeviceProcAddr(g->dev.device, syms[i].name);
        if (!fn)
            return false;
        memcpy(base + syms[i].offset, &fn, sizeof(fn));
    }
    return true;
}

static bool load_vulkan_library(struct render_gpu *g)
{
    static const struct vk_sym global_syms[] = {
        VK_SYM(EnumerateInstanceExtensionProperties),
        VK_SYM(CreateInstance),
    };
    g->vk_lib = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!g->vk_lib)
        return false;
    void *sym = dlsym(g->vk_lib, "vkGetInstanceProcAddr");
    if (!sym)
        return false;
    memcpy(&g->dev.get_instance_proc_addr, &sym, sizeof(sym));
    return resolve_instance_syms(g, VK_NULL_HANDLE, global_syms,
                                 sizeof(global_syms) / sizeof(global_syms[0]));
}

static bool load_instance_fns(struct render_gpu *g)
{
    static const struct vk_sym syms[] = {
        VK_SYM(DestroyInstance),
        VK_SYM(EnumeratePhysicalDevices),
        VK_SYM(GetPhysicalDeviceProperties),
        VK_SYM(GetPhysicalDeviceFeatures2),
        VK_SYM(EnumerateDeviceExtensionProperties),
        VK_SYM(GetPhysicalDeviceQueueFamilyProperties),
        VK_SYM(GetPhysicalDeviceMemoryProperties),
        VK_SYM(CreateDevice),
        VK_SYM(GetDeviceProcAddr),
        VK_SYM(GetPhysicalDeviceSurfaceSupportKHR),
        VK_SYM(GetPhysicalDeviceSurfacePresentModesKHR),
        VK_SYM(GetPhysicalDeviceSurfaceFormatsKHR),
        VK_SYM(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    };
    return resolve_instance_syms(g, g->dev.instance, syms, sizeof(syms) / sizeof(syms[0]));
}

static bool load_device_fns(struct render_gpu *g)
{
    static const struct vk_sym syms[] = {
        VK_SYM(DestroyDevice),
        VK_SYM(GetDeviceQueue),
        VK_SYM(DeviceWaitIdle),
        VK_SYM(QueueWaitIdle),
        VK_SYM(QueueSubmit2),
        VK_SYM(AllocateMemory),
        VK_SYM(FreeMemory),
        VK_SYM(MapMemory),
        VK_SYM(UnmapMemory),
        VK_SYM(CreateBuffer),
        VK_SYM(DestroyBuffer),
        VK_SYM(GetBufferMemoryRequirements),
        VK_SYM(BindBufferMemory),
        VK_SYM(CreateImage),
        VK_SYM(DestroyImage),
        VK_SYM(GetImageMemoryRequirements),
        VK_SYM(BindImageMemory),
        VK_SYM(CreateImageView),
        VK_SYM(DestroyImageView),
        VK_SYM(CreateDescriptorPool),
        VK_SYM(DestroyDescriptorPool),
        VK_SYM(CreateDescriptorSetLayout),
        VK_SYM(DestroyDescriptorSetLayout),
        VK_SYM(AllocateDescriptorSets),
        VK_SYM(FreeDescriptorSets),
        VK_SYM(UpdateDescriptorSets),
        VK_SYM(CreatePipelineLayout),
        VK_SYM(DestroyPipelineLayout),
        VK_SYM(CreateSampler),
        VK_SYM(DestroySampler),
        VK_SYM(CreatePipelineCache),
        VK_SYM(DestroyPipelineCache),
        VK_SYM(CreateShaderModule),
        VK_SYM(DestroyShaderModule),
        VK_SYM(CreateGraphicsPipelines),
        VK_SYM(DestroyPipeline),
        VK_SYM(CreateCommandPool),
        VK_SYM(DestroyCommandPool),
        VK_SYM(AllocateCommandBuffers),
        VK_SYM(BeginCommandBuffer),
        VK_SYM(EndCommandBuffer),
        VK_SYM(CmdPipelineBarrier2),
        VK_SYM(CmdCopyBufferToImage),
        VK_SYM(CmdBeginRendering),
        VK_SYM(CmdEndRendering),
        VK_SYM(CmdSetViewport),
        VK_SYM(CmdSetScissor),
        VK_SYM(CmdPushConstants),
        VK_SYM(CmdBindDescriptorSets),
        VK_SYM(CmdBindVertexBuffers),
        VK_SYM(CmdBindPipeline),
        VK_SYM(CmdDraw),
        VK_SYM(CreateFence),
        VK_SYM(DestroyFence),
        VK_SYM(GetFenceStatus),
        VK_SYM(WaitForFences),
        VK_SYM(ResetFences),
        VK_SYM(CreateSemaphore),
        VK_SYM(DestroySemaphore),
        VK_SYM(CreateSwapchainKHR),
        VK_SYM(DestroySwapchainKHR),
        VK_SYM(GetSwapchainImagesKHR),
        VK_SYM(AcquireNextImageKHR),
        VK_SYM(QueuePresentKHR),
    };
    return resolve_device_syms(g, syms, sizeof(syms) / sizeof(syms[0]));
}

/* Creates the Vulkan instance with surface and window system extension support. */
static bool instance_create(struct render_gpu *g)
{
    static const char *const window_systems[] = {
        "VK_KHR_wayland_surface",
        "VK_KHR_xlib_surface",
        "VK_KHR_xcb_surface",
    };
    uint32_t count = 0;
    if (g->vk.EnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || !count)
        return false;
    VkExtensionProperties *available = calloc(count, sizeof(*available));
    if (!available)
        return false;
    if (g->vk.EnumerateInstanceExtensionProperties(NULL, &count, available) != VK_SUCCESS)
        count = 0;
    const char *enabled[1 + sizeof(window_systems) / sizeof(window_systems[0])];
    uint32_t n = 0;
    if (has_extension(available, count, VK_KHR_SURFACE_EXTENSION_NAME))
        enabled[n++] = VK_KHR_SURFACE_EXTENSION_NAME;
    for (size_t i = 0; n && i < sizeof(window_systems) / sizeof(window_systems[0]); i++)
        if (has_extension(available, count, window_systems[i]))
            enabled[n++] = window_systems[i];
    free(available);
    /* VK_KHR_surface alone cannot show a window. */
    if (n < 2)
        return false;

    const struct app_identity *identity = app_identity_current();
    (void)identity;
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = identity ? identity->id : "",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = n,
        .ppEnabledExtensionNames = enabled,
    };
    if (g->vk.CreateInstance(&info, NULL, &g->dev.instance) != VK_SUCCESS)
        return false;
    if (load_instance_fns(g))
        return true;
    PFN_vkDestroyInstance destroy_instance =
        (PFN_vkDestroyInstance)g->dev.get_instance_proc_addr(g->dev.instance, "vkDestroyInstance");
    if (destroy_instance)
        destroy_instance(g->dev.instance, NULL);
    g->dev.instance = VK_NULL_HANDLE;
    return false;
}

/* Checks physical device capabilities and retrieves its graphics queue family index. */
static bool device_usable(const struct render_gpu *g, VkPhysicalDevice pd, bool software_ok,
                          uint32_t *queue_family)
{
    VkPhysicalDeviceProperties props;
    g->vk.GetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3)
        return false;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !software_ok)
        return false;

    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &v13,
    };
    g->vk.GetPhysicalDeviceFeatures2(pd, &features);
    if (!v13.synchronization2 || !v13.dynamicRendering)
        return false;

    uint32_t count = 0;
    g->vk.EnumerateDeviceExtensionProperties(pd, NULL, &count, NULL);
    VkExtensionProperties *extensions = count ? calloc(count, sizeof(*extensions)) : NULL;
    if (!extensions)
        return false;
    if (g->vk.EnumerateDeviceExtensionProperties(pd, NULL, &count, extensions) != VK_SUCCESS)
        count = 0;
    bool swapchain = has_extension(extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    free(extensions);
    if (!swapchain)
        return false;

    count = 0;
    g->vk.GetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
    VkQueueFamilyProperties *families = count ? calloc(count, sizeof(*families)) : NULL;
    if (!families)
        return false;
    g->vk.GetPhysicalDeviceQueueFamilyProperties(pd, &count, families);
    bool found = false;
    for (uint32_t i = 0; i < count && !found; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *queue_family = i;
            found = true;
        }
    }
    free(families);
    return found;
}

/* Selects a compatible physical device and creates the logical device and graphics queue. */
static bool device_create(struct render_gpu *g, bool software_ok)
{
    uint32_t count = 0;
    g->vk.EnumeratePhysicalDevices(g->dev.instance, &count, NULL);
    VkPhysicalDevice *devices = count ? calloc(count, sizeof(*devices)) : NULL;
    if (!devices)
        return false;
    VkResult vr = g->vk.EnumeratePhysicalDevices(g->dev.instance, &count, devices);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        count = 0;
    for (uint32_t i = 0; i < count && !g->dev.physical_device; i++)
        if (device_usable(g, devices[i], software_ok, &g->dev.queue_family))
            g->dev.physical_device = devices[i];
    free(devices);
    if (!g->dev.physical_device)
        return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g->dev.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    const char *extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &v13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &extension,
    };
    if (g->vk.CreateDevice(g->dev.physical_device, &info, NULL, &g->dev.device) != VK_SUCCESS)
        return false;
    if (!load_device_fns(g)) {
        PFN_vkDestroyDevice destroy_device =
            (PFN_vkDestroyDevice)g->vk.GetDeviceProcAddr(g->dev.device, "vkDestroyDevice");
        if (destroy_device)
            destroy_device(g->dev.device, NULL);
        g->dev.device = VK_NULL_HANDLE;
        return false;
    }
    g->vk.GetDeviceQueue(g->dev.device, g->dev.queue_family, 0, &g->dev.queue);
    return true;
}

/* Creates the command pool, the descriptor and pipeline layouts, the
 * sampler, the pipeline cache, and the resources of every frame slot. */
static bool objects_create(struct render_gpu *g)
{
    VkDevice device = g->dev.device;
    VkCommandPoolCreateInfo cmd_pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g->dev.queue_family,
    };
    if (g->vk.CreateCommandPool(device, &cmd_pool, NULL, &g->cmd_pool) != VK_SUCCESS)
        return false;

    /* One set for each texture: the atlas, the UI images, and the CPU
     * pixels of each surface that needs them. The first pool comes with
     * the first texture, and a full pool adds a bigger one. */
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo desc_layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    if (g->vk.CreateDescriptorSetLayout(device, &desc_layout, NULL, &g->desc_layout) != VK_SUCCESS)
        return false;

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .size = sizeof(struct push_constants),
    };
    VkPipelineLayoutCreateInfo pipeline_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g->desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    if (g->vk.CreatePipelineLayout(device, &pipeline_layout, NULL, &g->pipeline_layout) !=
        VK_SUCCESS)
        return false;

    /* The shaders read with texelFetch, so no filter runs; the combined
     * descriptor still needs a sampler. */
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (g->vk.CreateSampler(device, &sampler, NULL, &g->sampler) != VK_SUCCESS)
        return false;

    /* The cache only makes the pipelines faster to build, so a failure
     * leaves no cache and is no error. */
    VkPipelineCacheCreateInfo cache = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (g->vk.CreatePipelineCache(device, &cache, NULL, &g->pipeline_cache) != VK_SUCCESS)
        g->pipeline_cache = VK_NULL_HANDLE;

    for (int i = 0; i < FRAME_SLOTS; i++) {
        struct frame_slot *slot = &g->slots[i];
        VkFenceCreateInfo fence = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        VkCommandBufferAllocateInfo cmd = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g->cmd_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (g->vk.CreateFence(device, &fence, NULL, &slot->fence) != VK_SUCCESS ||
            !vulkan_slot_renew_semaphore(g, slot) ||
            g->vk.AllocateCommandBuffers(device, &cmd, &slot->cmd) != VK_SUCCESS)
            return false;
    }
    return true;
}

/* ---- Driver memory ------------------------------------------------------- */

/* The kernel GPU driver names, and the loader manifest patterns of their
 * Vulkan drivers. */
static const struct {
    const char *kernel;
    const char *manifests;
} driver_manifests[] = {
    { "amdgpu", "*radeon*,*amd*" },
    { "radeon", "*radeon*,*amd*" },
    { "i915", "*intel*" },
    { "xe", "*intel*" },
    { "nouveau", "*nouveau*" },
    { "nvidia", "*nvidia*" },
    { "virtio_gpu", "*virtio*" },
    { "msm", "*freedreno*" },
    { "panfrost", "*panfrost*" },
    { "panthor", "*panfrost*" },
    { "asahi", "*asahi*" },
    { "v3d", "*broadcom*" },
};

/* Appends the manifest patterns of `kernel` to `out`. Returns false for an
 * unknown kernel or when the patterns do not fit. */
static bool append_manifests(char *out, size_t cap, const char *kernel)
{
    for (size_t i = 0; i < sizeof(driver_manifests) / sizeof(driver_manifests[0]); i++) {
        if (strcmp(driver_manifests[i].kernel, kernel))
            continue;
        const char *add = driver_manifests[i].manifests;
        if (strstr(out, add))
            return true;
        size_t len = strlen(out);
        int n = snprintf(out + len, cap - len, "%s%s", len ? "," : "", add);
        return n > 0 && (size_t)n < cap - len;
    }
    return false;
}

/* Makes the loader filter for the Vulkan drivers of the GPUs in the machine.
 * The loader opens every driver it finds to ask for devices. Each driver
 * that has no device still maps its library and its dependencies, and that
 * raises the peak RSS. Returns false if a user setting, an unknown kernel
 * driver, or a failed scan means the full list must stay. */
static bool driver_filter(char *out, size_t cap)
{
    static const char *const user_settings[] = {
        "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE",
        "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
    };
    for (size_t i = 0; i < sizeof(user_settings) / sizeof(user_settings[0]); i++)
        if (getenv(user_settings[i]))
            return false;
    out[0] = '\0';
#ifdef __linux__
    DIR *dir = opendir("/sys/class/drm");
    if (!dir)
        return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(dir))) {
        if (strncmp(e->d_name, "renderD", 7))
            continue;
        char path[300], target[256];
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", e->d_name);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n <= 0) {
            ok = false;
            break;
        }
        target[n] = '\0';
        const char *slash = strrchr(target, '/');
        ok = append_manifests(out, cap, slash ? slash + 1 : target);
    }
    closedir(dir);
    return ok && out[0];
#else
    (void)cap;
    return false;
#endif
}

/* Gives back the resident pages of the LLVM library. RADV links LLVM, and
 * the static constructors of LLVM touch about 20 MiB of its code and
 * read-only data when the loader opens the driver. RADV compiles with ACO,
 * so the pages are not used again. The function releases only read-only
 * mappings that hold no private copy. Those pages are clean page cache, and
 * a later access maps them again from the file. Relocated (RELRO) and
 * writable mappings hold anonymous pages. It keeps those. */
static void driver_pages_release(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/self/smaps", "re");
    if (!f)
        return;
    char line[512];
    unsigned long start = 0, end = 0;
    bool candidate = false;
    while (fgets(line, sizeof(line), f)) {
        char perms[8], path[384];
        unsigned long s, e, anon;
        int fields = sscanf(line, "%lx-%lx %7s %*s %*s %*s %383s", &s, &e, perms, path);
        if (fields >= 3) {
            const char *base = fields == 4 ? strrchr(path, '/') : NULL;
            start = s;
            end = e;
            candidate = base && !strncmp(base + 1, "libLLVM", 7) && perms[1] != 'w' &&
                        perms[3] == 'p';
        } else if (candidate && sscanf(line, "Anonymous: %lu", &anon) == 1) {
            if (anon == 0)
                madvise((void *)start, end - start, MADV_DONTNEED);
            candidate = false;
        }
    }
    fclose(f);
#endif
}

static struct render_gpu *gpu_open(enum gpu_choice choice, const char *drivers)
{
    struct render_gpu *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    if (!load_vulkan_library(g)) {
        render_gpu_free(g);
        return NULL;
    }
    /* Skip Mesa software rasterizer probing unless a driver filter is in
     * use, software mode was requested, or the user set
     * VK_LOADER_DRIVERS_DISABLE. The loader reads these settings when it
     * creates the instance. */
    bool hide_lvp = !drivers && choice == GPU_HARDWARE && !getenv("VK_LOADER_DRIVERS_DISABLE");
    if (drivers)
        setenv("VK_LOADER_DRIVERS_SELECT", drivers, 1);
    else if (hide_lvp)
        setenv("VK_LOADER_DRIVERS_DISABLE", "*lvp*", 1);
    bool inst_ok = instance_create(g);
    if (drivers)
        unsetenv("VK_LOADER_DRIVERS_SELECT");
    else if (hide_lvp)
        unsetenv("VK_LOADER_DRIVERS_DISABLE");
    /* Release the pages before the device and the pipelines allocate, so
     * the two do not add up in the peak. */
    if (inst_ok)
        driver_pages_release();

    const struct vector2i atlas_size = { .x = GPU_ATLAS_START_SIZE, .y = GPU_ATLAS_START_SIZE };
    if (!inst_ok || !device_create(g, choice == GPU_SOFTWARE) || !objects_create(g) ||
        !gpu_build_init(&g->build) || !vulkan_texture_ensure(g, &g->atlas, atlas_size) ||
        !vulkan_pipelines_ensure(g, TEXEL_FORMAT)) {
        render_gpu_free(g);
        return NULL;
    }
    return g;
}

struct render_gpu *render_gpu_new(void)
{
    enum gpu_choice choice = gpu_choice();
    if (choice == GPU_OFF)
        return NULL;
    char drivers[256];
    if (choice == GPU_HARDWARE && driver_filter(drivers, sizeof(drivers))) {
        struct render_gpu *g = gpu_open(choice, drivers);
        if (g)
            return g;
    }
    /* The filter can miss a driver with an unusual manifest name. */
    return gpu_open(choice, NULL);
}

void render_gpu_free(struct render_gpu *g)
{
    if (!g)
        return;
    VkDevice device = g->dev.device;
    if (device) {
        g->vk.DeviceWaitIdle(device);
        /* A surface can outlive the renderer. Take its objects now. */
        while (g->surfaces)
            vulkan_surface_release(g->surfaces);
        for (int i = 0; i < FRAME_SLOTS; i++) {
            struct frame_slot *slot = &g->slots[i];
            if (slot->fence)
                g->vk.DestroyFence(device, slot->fence, NULL);
            if (slot->acquired)
                g->vk.DestroySemaphore(device, slot->acquired, NULL);
            vulkan_buffer_destroy(g, &slot->instances);
            vulkan_buffer_destroy(g, &slot->uploads);
        }
        vulkan_texture_destroy(g, &g->atlas);
        for (int i = 0; i < g->n_images; i++)
            vulkan_texture_destroy(g, &g->images[i]);
        pipelines_destroy(g);
        if (g->pipeline_cache)
            g->vk.DestroyPipelineCache(device, g->pipeline_cache, NULL);
        if (g->pipeline_layout)
            g->vk.DestroyPipelineLayout(device, g->pipeline_layout, NULL);
        if (g->sampler)
            g->vk.DestroySampler(device, g->sampler, NULL);
        if (g->desc_layout)
            g->vk.DestroyDescriptorSetLayout(device, g->desc_layout, NULL);
        for (uint32_t i = 0; i < g->n_desc_pools; i++)
            g->vk.DestroyDescriptorPool(device, g->desc_pools[i].pool, NULL);
        /* The pool frees the command buffers of the slots. */
        if (g->cmd_pool)
            g->vk.DestroyCommandPool(device, g->cmd_pool, NULL);
        g->vk.DestroyDevice(device, NULL);
    }
    if (g->dev.instance && g->vk.DestroyInstance)
        g->vk.DestroyInstance(g->dev.instance, NULL);
    if (g->vk_lib)
        dlclose(g->vk_lib);
    gpu_build_release(&g->build);
    free(g->copies);
    free(g->images);
    free(g->image_offsets);
    free(g->desc_pools);
    free(g);
}

void *render_gpu_native_device(struct render_gpu *g)
{
    return g ? (void *)&g->dev : NULL;
}

bool render_gpu_idle(struct render_gpu *g)
{
    (void)g;
    /* The backend keeps no transient state that it releases when idle. */
    return true;
}
