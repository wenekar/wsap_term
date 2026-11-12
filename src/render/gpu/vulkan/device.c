#include "render/gpu/vulkan/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/gpu/gpu.h"

#include "vulkan_shaders_spv.h"

/* The instance, the device, the pipelines, and the resource helpers. */

void vulkan_wait_idle(struct render_gpu *g)
{
    vkQueueWaitIdle(g->dev.queue);
}

/* ---- Textures and buffers ----------------------------------------------- */

static uint32_t memory_type(struct render_gpu *g, uint32_t type_bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g->dev.physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Allocates memory with the `want` properties. With `any`, memory without
 * them is permitted when the device has none with them. */
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
    return vkAllocateMemory(g->dev.device, &info, NULL, out) == VK_SUCCESS;
}

/* Destroys the image of the texture. The descriptor set stays. */
static void texture_drop_image(struct render_gpu *g, struct vulkan_texture *t)
{
    VkDevice device = g->dev.device;
    if (t->view)
        vkDestroyImageView(device, t->view, NULL);
    if (t->image)
        vkDestroyImage(device, t->image, NULL);
    if (t->memory)
        vkFreeMemory(device, t->memory, NULL);
    VkDescriptorSet set = t->set;
    *t = (struct vulkan_texture){ .set = set };
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
    if (vkCreateImage(device, &image_info, NULL, &t->image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(device, t->image, &reqs);
    if (!memory_allocate(g, &reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, &t->memory) ||
        vkBindImageMemory(device, t->image, t->memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = TEXEL_FORMAT,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(device, &view_info, NULL, &t->view) != VK_SUCCESS)
        return false;
    if (!t->set) {
        VkDescriptorSetAllocateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &g->desc_layout,
        };
        if (vkAllocateDescriptorSets(device, &set_info, &t->set) != VK_SUCCESS) {
            t->set = VK_NULL_HANDLE;
            return false;
        }
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
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
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
        vkFreeDescriptorSets(g->dev.device, g->desc_pool, 1, &t->set);
    *t = (struct vulkan_texture){0};
}

void vulkan_buffer_destroy(struct render_gpu *g, struct vulkan_buffer *b)
{
    VkDevice device = g->dev.device;
    if (b->mapped)
        vkUnmapMemory(device, b->memory);
    if (b->buffer)
        vkDestroyBuffer(device, b->buffer, NULL);
    if (b->memory)
        vkFreeMemory(device, b->memory, NULL);
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
    bool ok = vkCreateBuffer(device, &info, NULL, &b->buffer) == VK_SUCCESS;
    if (ok) {
        vkGetBufferMemoryRequirements(device, b->buffer, &reqs);
        ok = memory_allocate(g, &reqs,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             false, &b->memory) &&
             vkBindBufferMemory(device, b->buffer, b->memory, 0) == VK_SUCCESS &&
             vkMapMemory(device, b->memory, 0, cap, 0, &b->mapped) == VK_SUCCESS;
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
    bool blend;
    VkColorComponentFlags mask;
} PIPELINES[PIPE_COUNT] = {
    [PIPE_GLYPH] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false, true, ALL_CHANNELS },
    [PIPE_SUBPX + 0] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false, true,
                         VK_COLOR_COMPONENT_R_BIT },
    [PIPE_SUBPX + 1] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false, true,
                         VK_COLOR_COMPONENT_G_BIT },
    [PIPE_SUBPX + 2] = { SHADER_GLYPH_VERT, SHADER_GLYPH_FRAG, false, true,
                         VK_COLOR_COMPONENT_B_BIT },
    [PIPE_SHAPE] = { SHADER_SHAPE_VERT, SHADER_SHAPE_FRAG, true, true, ALL_CHANNELS },
    [PIPE_COPY] = { SHADER_GLYPH_VERT, SHADER_COPY_FRAG, false, false, ALL_CHANNELS },
};

static const VkVertexInputAttributeDescription GLYPH_ATTRIBS[] = {
    { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_glyph_inst, rect) },
    { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_glyph_inst, uv) },
    { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_glyph_inst, color) },
    { 3, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_glyph_inst, mode) },
};

static const VkVertexInputAttributeDescription SHAPE_ATTRIBS[] = {
    { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_shape_inst, rect) },
    { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct gpu_shape_inst, color) },
    { 2, 0, VK_FORMAT_R32_SFLOAT, offsetof(struct gpu_shape_inst, radius) },
    { 3, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_shape_inst, mode) },
    { 4, 0, VK_FORMAT_R32_UINT, offsetof(struct gpu_shape_inst, blur) },
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
    /* Premultiplied source over destination. */
    VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = spec->blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = spec->mask,
    };
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
    if (vkCreateGraphicsPipelines(g->dev.device, g->pipeline_cache, 1, &info, NULL, &pipeline) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

static void pipelines_destroy(struct render_gpu *g)
{
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (g->pipelines[i])
            vkDestroyPipeline(g->dev.device, g->pipelines[i], NULL);
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
        ok = vkCreateShaderModule(g->dev.device, &info, NULL, &shaders[i]) == VK_SUCCESS;
    }
    for (int i = 0; i < PIPE_COUNT && ok; i++) {
        g->pipelines[i] = pipeline_make(g, &PIPELINES[i], shaders, format);
        ok = g->pipelines[i] != VK_NULL_HANDLE;
    }
    for (int i = 0; i < SHADER_COUNT; i++)
        if (shaders[i])
            vkDestroyShaderModule(g->dev.device, shaders[i], NULL);
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

/*
 * WSAP_RENDER_GPU: "0" or "off" gives no GPU. "software" also accepts a
 * Vulkan device that runs on the CPU, such as lavapipe. The default
 * refuses one, because the CPU renderer of this program is faster.
 */
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

/*
 * The window layer makes the VkSurfaceKHR of each window, and this layer
 * cannot ask it which extensions it needs. So the instance enables
 * VK_KHR_surface and every window system extension that the loader has.
 */
static bool instance_create(struct render_gpu *g)
{
    static const char *const window_systems[] = {
        "VK_KHR_wayland_surface",
        "VK_KHR_xlib_surface",
        "VK_KHR_xcb_surface",
    };
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || !count)
        return false;
    VkExtensionProperties *available = calloc(count, sizeof(*available));
    if (!available)
        return false;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, available) != VK_SUCCESS)
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

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "wsap_term",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = n,
        .ppEnabledExtensionNames = enabled,
    };
    return vkCreateInstance(&info, NULL, &g->dev.instance) == VK_SUCCESS;
}

/* True when the device can run the backend. Writes its graphics queue family. */
static bool device_usable(VkPhysicalDevice pd, bool software_ok, uint32_t *queue_family)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
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
    vkGetPhysicalDeviceFeatures2(pd, &features);
    if (!v13.synchronization2 || !v13.dynamicRendering)
        return false;

    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL);
    VkExtensionProperties *extensions = count ? calloc(count, sizeof(*extensions)) : NULL;
    if (!extensions)
        return false;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &count, extensions) != VK_SUCCESS)
        count = 0;
    bool swapchain = has_extension(extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    free(extensions);
    if (!swapchain)
        return false;

    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
    VkQueueFamilyProperties *families = count ? calloc(count, sizeof(*families)) : NULL;
    if (!families)
        return false;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families);
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

/*
 * Takes the first usable device in the order of the loader. That order is
 * the choice of the system: the loader and the Mesa device selection put
 * the default GPU first, and they obey DRI_PRIME and MESA_VK_DEVICE_SELECT.
 * So a terminal does not wake a second GPU on its own.
 */
static bool device_create(struct render_gpu *g, bool software_ok)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g->dev.instance, &count, NULL);
    VkPhysicalDevice *devices = count ? calloc(count, sizeof(*devices)) : NULL;
    if (!devices)
        return false;
    VkResult vr = vkEnumeratePhysicalDevices(g->dev.instance, &count, devices);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        count = 0;
    for (uint32_t i = 0; i < count && !g->dev.physical_device; i++)
        if (device_usable(devices[i], software_ok, &g->dev.queue_family))
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
    if (vkCreateDevice(g->dev.physical_device, &info, NULL, &g->dev.device) != VK_SUCCESS)
        return false;
    vkGetDeviceQueue(g->dev.device, g->dev.queue_family, 0, &g->dev.queue);
    return true;
}

/* The pools, the layouts, the sampler, and the frame slots. */
static bool objects_create(struct render_gpu *g)
{
    VkDevice device = g->dev.device;
    VkCommandPoolCreateInfo cmd_pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g->dev.queue_family,
    };
    if (vkCreateCommandPool(device, &cmd_pool, NULL, &g->cmd_pool) != VK_SUCCESS)
        return false;

    /* One set for each texture: the atlas, the UI images, and the CPU
     * pixels of each surface that needs them. */
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 64,
    };
    VkDescriptorPoolCreateInfo desc_pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 64,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    if (vkCreateDescriptorPool(device, &desc_pool, NULL, &g->desc_pool) != VK_SUCCESS)
        return false;

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
    if (vkCreateDescriptorSetLayout(device, &desc_layout, NULL, &g->desc_layout) != VK_SUCCESS)
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
    if (vkCreatePipelineLayout(device, &pipeline_layout, NULL, &g->pipeline_layout) != VK_SUCCESS)
        return false;

    /* The shaders fetch whole texels, so the filter never runs. */
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(device, &sampler, NULL, &g->sampler) != VK_SUCCESS)
        return false;

    /* The cache only makes the pipelines faster to build. */
    VkPipelineCacheCreateInfo cache = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (vkCreatePipelineCache(device, &cache, NULL, &g->pipeline_cache) != VK_SUCCESS)
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
        if (vkCreateFence(device, &fence, NULL, &slot->fence) != VK_SUCCESS ||
            !vulkan_slot_renew_semaphore(g, slot) ||
            vkAllocateCommandBuffers(device, &cmd, &slot->cmd) != VK_SUCCESS)
            return false;
    }
    return true;
}

struct render_gpu *render_gpu_new(void)
{
    enum gpu_choice choice = gpu_choice();
    if (choice == GPU_OFF)
        return NULL;
    struct render_gpu *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    const struct vector2i atlas_size = { .x = GPU_ATLAS_START_SIZE, .y = GPU_ATLAS_START_SIZE };
    if (!instance_create(g) || !device_create(g, choice == GPU_SOFTWARE) || !objects_create(g) ||
        !gpu_build_init(&g->build) || !vulkan_texture_ensure(g, &g->atlas, atlas_size) ||
        !vulkan_pipelines_ensure(g, TEXEL_FORMAT)) {
        render_gpu_free(g);
        return NULL;
    }
    return g;
}

void render_gpu_free(struct render_gpu *g)
{
    if (!g)
        return;
    VkDevice device = g->dev.device;
    if (device) {
        vkDeviceWaitIdle(device);
        /* A surface can outlive the renderer. Take its objects now. */
        while (g->surfaces)
            vulkan_surface_release(g->surfaces);
        for (int i = 0; i < FRAME_SLOTS; i++) {
            struct frame_slot *slot = &g->slots[i];
            if (slot->fence)
                vkDestroyFence(device, slot->fence, NULL);
            if (slot->acquired)
                vkDestroySemaphore(device, slot->acquired, NULL);
            vulkan_buffer_destroy(g, &slot->instances);
            vulkan_buffer_destroy(g, &slot->uploads);
        }
        vulkan_texture_destroy(g, &g->atlas);
        for (int i = 0; i < GPU_IMAGES; i++)
            vulkan_texture_destroy(g, &g->images[i]);
        pipelines_destroy(g);
        if (g->pipeline_cache)
            vkDestroyPipelineCache(device, g->pipeline_cache, NULL);
        if (g->pipeline_layout)
            vkDestroyPipelineLayout(device, g->pipeline_layout, NULL);
        if (g->sampler)
            vkDestroySampler(device, g->sampler, NULL);
        if (g->desc_layout)
            vkDestroyDescriptorSetLayout(device, g->desc_layout, NULL);
        if (g->desc_pool)
            vkDestroyDescriptorPool(device, g->desc_pool, NULL);
        /* The pool frees the command buffers of the slots. */
        if (g->cmd_pool)
            vkDestroyCommandPool(device, g->cmd_pool, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (g->dev.instance)
        vkDestroyInstance(g->dev.instance, NULL);
    gpu_build_release(&g->build);
    free(g->copies);
    free(g);
}

void *render_gpu_native_device(struct render_gpu *g)
{
    return g ? (void *)&g->dev : NULL;
}
