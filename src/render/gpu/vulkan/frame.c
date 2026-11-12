#include "render/gpu/vulkan/internal.h"

#include <stdlib.h>
#include <string.h>

#include "paint/framebuffer.h"
#include "render/gpu/gpu.h"
#include "render/render.h"
#include "render/surface.h"
#include "util/damage.h"

/* Moves one frame to the GPU, draws it, and presents it. */

/*
 * The longest wait for a swapchain image. An image is free at once in the
 * normal case. The wait covers the short time that the compositor needs to
 * give one back. After it the frame is dropped, and the window asks again
 * at its next display tick, so a stalled compositor cannot stop the main
 * thread.
 */
#define ACQUIRE_WAIT_NS 2000000ull
/* In a resize, a dropped frame shows as a stale size. Wait longer, for the
 * frame slot and for the image. */
#define RESIZE_WAIT_NS 20000000ull

/* ---- Frame slots -------------------------------------------------------- */

bool vulkan_slot_renew_semaphore(struct render_gpu *g, struct frame_slot *slot)
{
    VkSemaphoreCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (slot->acquired)
        vkDestroySemaphore(g->dev.device, slot->acquired, NULL);
    slot->acquired = VK_NULL_HANDLE;
    return vkCreateSemaphore(g->dev.device, &info, NULL, &slot->acquired) == VK_SUCCESS;
}

/*
 * Takes a slot that the GPU does not use, or returns NULL. With `wait_ns`
 * it waits that long for the oldest slot. The fence of a free slot stays
 * signaled until frame_submit() takes it, so a frame that stops before the
 * submit leaves the slot free.
 */
static struct frame_slot *slot_acquire(struct render_gpu *g, uint64_t wait_ns)
{
    VkDevice device = g->dev.device;
    for (unsigned i = 0; i < FRAME_SLOTS; i++) {
        unsigned index = (g->next_slot + i) % FRAME_SLOTS;
        if (vkGetFenceStatus(device, g->slots[index].fence) == VK_SUCCESS) {
            g->next_slot = (index + 1) % FRAME_SLOTS;
            return &g->slots[index];
        }
    }
    struct frame_slot *oldest = &g->slots[g->next_slot];
    if (wait_ns && vkWaitForFences(device, 1, &oldest->fence, VK_TRUE, wait_ns) == VK_SUCCESS) {
        g->next_slot = (g->next_slot + 1) % FRAME_SLOTS;
        return oldest;
    }
    return NULL;
}

/* ---- Layout changes ----------------------------------------------------- */

/* The layout changes of textures that go into one barrier command. */
struct barriers {
    VkImageMemoryBarrier2 list[1 + GPU_IMAGES];
    uint32_t n;
};

static void layout_use(VkImageLayout layout, VkPipelineStageFlags2 *stage, VkAccessFlags2 *access)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        *access = VK_ACCESS_2_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    default:
        /* Undefined and present: no stage of this command buffer uses the image. */
        *stage = VK_PIPELINE_STAGE_2_NONE;
        *access = VK_ACCESS_2_NONE;
        break;
    }
}

static VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    layout_use(from, &barrier.srcStageMask, &barrier.srcAccessMask);
    layout_use(to, &barrier.dstStageMask, &barrier.dstAccessMask);
    return barrier;
}

/* Adds the change of a texture to layout `to`. The texture records it. */
static void barriers_add(struct barriers *bs, struct vulkan_texture *t, VkImageLayout to)
{
    if (t->layout == to)
        return;
    bs->list[bs->n++] = image_barrier(t->image, t->layout, to);
    t->layout = to;
}

static void barriers_flush(VkCommandBuffer cmd, struct barriers *bs)
{
    if (!bs->n)
        return;
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = bs->n,
        .pImageMemoryBarriers = bs->list,
    };
    vkCmdPipelineBarrier2(cmd, &info);
    bs->n = 0;
}

/* ---- The pass of a frame ------------------------------------------------ */

static struct vulkan_drawable *frame_drawable(struct framebuffer *fb)
{
    if (!fb->drawable && fb->acquire)
        fb->drawable = fb->acquire(fb->acquire_ud);
    return fb->drawable;
}

/* The part of the canvas that the swapchain image holds. On X11 the image
 * can be smaller than the canvas for a short time. */
static struct vector2i target_bounds(const struct gpu_surface *gs, struct vector2i canvas)
{
    return (struct vector2i){
        .x = canvas.x < (int)gs->extent.width ? canvas.x : (int)gs->extent.width,
        .y = canvas.y < (int)gs->extent.height ? canvas.y : (int)gs->extent.height,
    };
}

static void set_scissor(VkCommandBuffer cmd, struct rect2i r)
{
    VkRect2D scissor = {
        .offset = { r.x, r.y },
        .extent = { (uint32_t)r.width, (uint32_t)r.height },
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

/*
 * Starts the command buffer of the slot. The caller records its uploads
 * next, and then calls pass_begin().
 */
static bool frame_begin(struct frame_slot *slot)
{
    VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    return vkBeginCommandBuffer(slot->cmd, &info) == VK_SUCCESS;
}

/*
 * Starts drawing into a swapchain image. `clear` NULL means: the pass
 * writes every pixel, so the old pixels do not matter. The viewport is the
 * canvas, so one unit is one pixel also when the image is smaller.
 */
static void pass_begin(struct render_gpu *g, VkCommandBuffer cmd, const struct gpu_surface *gs,
                       uint32_t image_index, struct vector2i canvas,
                       const VkClearColorValue *clear)
{
    /* The image is in use from the stage that waits for the acquire semaphore. */
    VkImageMemoryBarrier2 to_target = image_barrier(gs->chain.images[image_index],
                                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_target.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_target,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = gs->chain.views[image_index],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    if (clear)
        color.clearValue.color = *clear;
    VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .extent = gs->extent },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(cmd, &rendering);

    VkViewport viewport = {
        .width = (float)canvas.x,
        .height = (float)canvas.y,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    struct push_constants push = { .viewport_size = { (float)canvas.x, (float)canvas.y } };
    vkCmdPushConstants(cmd, g->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
}

/*
 * Ends the pass, submits the frame, and presents the image. On failure the
 * frame did not reach the GPU, and the slot is free again.
 */
static bool frame_submit(struct render_gpu *g, struct gpu_surface *gs, struct frame_slot *slot,
                         uint32_t image_index)
{
    VkDevice device = g->dev.device;
    vkCmdEndRendering(slot->cmd);
    VkImageMemoryBarrier2 to_present = image_barrier(gs->chain.images[image_index],
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_present,
    };
    vkCmdPipelineBarrier2(slot->cmd, &dependency);
    if (vkEndCommandBuffer(slot->cmd) != VK_SUCCESS)
        return false;

    VkCommandBufferSubmitInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = slot->cmd,
    };
    VkSemaphoreSubmitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = slot->acquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = gs->chain.rendered[image_index],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commands,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    vkResetFences(device, 1, &slot->fence);
    if (vkQueueSubmit2(g->dev.queue, 1, &submit, slot->fence) != VK_SUCCESS) {
        /* No submit signals the fence now. Give the slot a signaled one. */
        VkFenceCreateInfo fence = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        vkDestroyFence(device, slot->fence, NULL);
        slot->fence = VK_NULL_HANDLE;
        vkCreateFence(device, &fence, NULL, &slot->fence);
        return false;
    }

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &gs->chain.rendered[image_index],
        .swapchainCount = 1,
        .pSwapchains = &gs->chain.handle,
        .pImageIndices = &image_index,
    };
    VkResult presented = vkQueuePresentKHR(g->dev.queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR)
        gs->out_of_date = true;
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        gs->recheck = true;
    return true;
}

/*
 * A frame stopped after it acquired its image. The semaphore of the slot
 * can hold a signal, and the swapchain keeps the image. A new semaphore and
 * a new swapchain put both right.
 */
static void frame_abandon(struct render_gpu *g, struct gpu_surface *gs, struct frame_slot *slot)
{
    vulkan_slot_renew_semaphore(g, slot);
    gs->out_of_date = true;
}

/* ---- Drawing a built frame ---------------------------------------------- */

/* Makes each texture of the frame fit what the builder asks for. */
static bool textures_sync(struct render_gpu *g)
{
    const struct gpu_build *b = &g->build;
    const struct vector2i atlas_size = { .x = b->atlas.size, .y = b->atlas.size };
    if (!vulkan_texture_ensure(g, &g->atlas, atlas_size))
        return false;
    for (int i = 0; i < GPU_IMAGES; i++)
        if (b->images[i].frame == b->frame &&
            !vulkan_texture_ensure(g, &g->images[i], b->images[i].size))
            return false;
    return true;
}

/* Copies the new glyph pixels and the new image pixels of the frame into
 * the upload buffer of the slot. */
static bool uploads_stage(struct render_gpu *g, struct frame_slot *slot)
{
    const struct gpu_build *b = &g->build;
    size_t total = b->atlas.stage_used;
    for (int i = 0; i < GPU_IMAGES; i++) {
        if (!gpu_image_needs_upload(b, i))
            continue;
        g->image_offsets[i] = total;
        total += (size_t)b->images[i].size.x * (size_t)b->images[i].size.y * 4;
    }
    if (!total)
        return true;
    if (b->atlas.n_uploads > g->cap_copies) {
        VkBufferImageCopy *copies = realloc(g->copies, b->atlas.n_uploads * sizeof(*copies));
        if (!copies)
            return false;
        g->copies = copies;
        g->cap_copies = b->atlas.n_uploads;
    }
    if (!vulkan_buffer_ensure(g, &slot->uploads, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    uint8_t *dst = slot->uploads.mapped;
    if (b->atlas.stage_used)
        memcpy(dst, b->atlas.stage, b->atlas.stage_used);
    for (int i = 0; i < GPU_IMAGES; i++)
        if (gpu_image_needs_upload(b, i))
            memcpy(dst + g->image_offsets[i], b->images[i].pixels,
                   (size_t)b->images[i].size.x * (size_t)b->images[i].size.y * 4);
    return true;
}

/* Records the copies from the upload buffer into the textures. After it,
 * every texture of the frame can be sampled. */
static void uploads_record(struct render_gpu *g, struct frame_slot *slot)
{
    const struct gpu_build *b = &g->build;
    VkCommandBuffer cmd = slot->cmd;
    struct barriers barriers = {0};

    if (b->atlas.n_uploads)
        barriers_add(&barriers, &g->atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (int i = 0; i < GPU_IMAGES; i++)
        if (gpu_image_needs_upload(b, i))
            barriers_add(&barriers, &g->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barriers_flush(cmd, &barriers);

    if (b->atlas.n_uploads) {
        for (uint32_t i = 0; i < b->atlas.n_uploads; i++) {
            const struct gpu_upload *u = &b->atlas.uploads[i];
            g->copies[i] = (VkBufferImageCopy){
                .bufferOffset = u->offset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { u->x, u->y, 0 },
                .imageExtent = { u->w, u->h, 1 },
            };
        }
        vkCmdCopyBufferToImage(cmd, slot->uploads.buffer, g->atlas.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b->atlas.n_uploads,
                               g->copies);
    }
    for (int i = 0; i < GPU_IMAGES; i++) {
        if (!gpu_image_needs_upload(b, i))
            continue;
        VkBufferImageCopy copy = {
            .bufferOffset = g->image_offsets[i],
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)b->images[i].size.x, (uint32_t)b->images[i].size.y, 1 },
        };
        vkCmdCopyBufferToImage(cmd, slot->uploads.buffer, g->images[i].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    /* A new atlas without uploads also moves here: a draw can bind it. */
    barriers_add(&barriers, &g->atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (int i = 0; i < GPU_IMAGES; i++)
        if (b->images[i].frame == b->frame)
            barriers_add(&barriers, &g->images[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barriers_flush(cmd, &barriers);
}

/*
 * The recorded uploads never ran. Their textures hold unknown pixels, and
 * the builder loads all of them again: gpu_build_abort() forgets the atlas,
 * and the images of the frame stay dirty.
 */
static void uploads_forget(struct render_gpu *g)
{
    const struct gpu_build *b = &g->build;
    g->atlas.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    for (int i = 0; i < GPU_IMAGES; i++)
        if (gpu_image_needs_upload(b, i))
            g->images[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

/* Records the draw commands of the frame, in order. */
static void commands_record(struct render_gpu *g, struct frame_slot *slot,
                            const size_t offsets[GPU_ST_COUNT], struct vector2i bounds)
{
    const struct gpu_build *b = &g->build;
    VkCommandBuffer cmd = slot->cmd;
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    VkDeviceSize bound_offset = (VkDeviceSize)-1;
    struct rect2i bound_scissor = {0};
    uint32_t bound_channel = 0;

    for (uint32_t i = 0; i < b->n_cmds; i++) {
        const struct gpu_cmd *c = &b->cmds[i];
        struct rect2i scissor = c->scissor;
        if (!c->count || !rect2i_clip(&scissor, bounds))
            continue;
        if (!rect2i_eq(scissor, bound_scissor)) {
            set_scissor(cmd, scissor);
            bound_scissor = scissor;
        }
        /* The shape shaders sample nothing. */
        if (c->kind != GPU_CMD_SHAPE) {
            VkDescriptorSet set = c->image ? g->images[c->image - 1].set : g->atlas.set;
            if (set != bound_set) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipeline_layout,
                                        0, 1, &set, 0, NULL);
                bound_set = set;
            }
        }
        VkDeviceSize offset = offsets[c->stream];
        if (offset != bound_offset) {
            vkCmdBindVertexBuffers(cmd, 0, 1, &slot->instances.buffer, &offset);
            bound_offset = offset;
        }
        int passes = c->kind == GPU_CMD_SUBPX ? 3 : 1;
        for (int pass = 0; pass < passes; pass++) {
            VkPipeline pipeline = g->pipelines[c->kind == GPU_CMD_SHAPE ? PIPE_SHAPE
                                               : c->kind == GPU_CMD_SUBPX ? PIPE_SUBPX + pass
                                               : PIPE_GLYPH];
            if (pipeline != bound_pipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound_pipeline = pipeline;
            }
            uint32_t channel = (uint32_t)pass;
            if (c->kind == GPU_CMD_SUBPX && channel != bound_channel) {
                /* The update names every stage of the push constant range. */
                vkCmdPushConstants(cmd, g->pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   (uint32_t)offsetof(struct push_constants, channel),
                                   (uint32_t)sizeof(channel), &channel);
                bound_channel = channel;
            }
            vkCmdDraw(cmd, 6, c->count, 0, c->start);
        }
    }
}

/* Draws and presents the frame that the builder holds. */
static enum render_gpu_status draw_built(struct render_gpu *g, struct gpu_surface *gs,
                                         struct frame_slot *slot, struct framebuffer *fb,
                                         uint32_t base, uint64_t wait_ns)
{
    /* Everything that can fail on memory comes first. Up to the drawable,
     * a failure gives the frame to the CPU. */
    size_t offsets[GPU_ST_COUNT];
    size_t instance_bytes = gpu_build_instance_layout(&g->build, offsets);
    if (!textures_sync(g) ||
        !vulkan_buffer_ensure(g, &slot->instances, instance_bytes,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !uploads_stage(g, slot))
        return RENDER_GPU_UNUSED;
    gpu_build_copy_instances(&g->build, offsets, slot->instances.mapped);

    struct vulkan_drawable *drawable = frame_drawable(fb);
    if (!drawable || !drawable->surface)
        return RENDER_GPU_DROPPED;
    bool tearing = drawable->unpaced || fb->transaction;
    uint32_t image_index = 0;
    if (!vulkan_swapchain_ensure(g, gs, drawable->surface, fb->size, tearing, false) ||
        !vulkan_swapchain_acquire(g, gs, slot, drawable->surface, fb->size, tearing, wait_ns,
                                  &image_index))
        return RENDER_GPU_DROPPED;

    if (!frame_begin(slot)) {
        frame_abandon(g, gs, slot);
        return RENDER_GPU_DROPPED;
    }
    uploads_record(g, slot);
    float color[4];
    gpu_color(base, 1.0f, color);
    const VkClearColorValue clear = { .float32 = { color[0], color[1], color[2], color[3] } };
    pass_begin(g, slot->cmd, gs, image_index, fb->size, &clear);
    commands_record(g, slot, offsets, target_bounds(gs, fb->size));
    if (!frame_submit(g, gs, slot, image_index)) {
        uploads_forget(g);
        frame_abandon(g, gs, slot);
        return RENDER_GPU_DROPPED;
    }
    return RENDER_GPU_PRESENTED;
}

/* True when the frame changes the size of the surface. */
static bool frame_resizes(const struct gpu_surface *gs, const struct framebuffer *fb)
{
    return fb->transaction || !vector2i_eq(gs->request_size, fb->size);
}

enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame)
{
    if (!g || !s || !fb || !frame || (!fb->drawable && !fb->acquire) || fb->size.x <= 0 ||
        fb->size.y <= 0)
        return RENDER_GPU_UNUSED;
    struct gpu_surface *gs = vulkan_surface_of(g, s);
    if (!gs)
        return RENDER_GPU_UNUSED;
    bool resizing = frame_resizes(gs, fb);
    struct frame_slot *slot = slot_acquire(g, resizing ? RESIZE_WAIT_NS : 0);
    if (!slot)
        return RENDER_GPU_DROPPED;
    if (!gpu_build_frame(&g->build, frame, fb->size))
        return RENDER_GPU_UNUSED;
    enum render_gpu_status status =
        draw_built(g, gs, slot, fb, frame->base, resizing ? RESIZE_WAIT_NS : ACQUIRE_WAIT_NS);
    if (status == RENDER_GPU_PRESENTED)
        gpu_build_commit(&g->build);
    else
        gpu_build_abort(&g->build);
    return status;
}

/* ---- Presenting CPU pixels ---------------------------------------------- */

static bool present_pixels(struct render_gpu *g, struct gpu_surface *gs, struct frame_slot *slot,
                           struct framebuffer *fb, const struct damage_list *damage,
                           uint64_t wait_ns)
{
    struct vulkan_drawable *drawable = frame_drawable(fb);
    if (!drawable || !drawable->surface)
        return false;
    bool tearing = drawable->unpaced || fb->transaction;
    if (!vulkan_swapchain_ensure(g, gs, drawable->surface, fb->size, tearing, false))
        return false;

    /* A texture without known pixels takes every pixel. */
    bool whole = !gs->pixels.image || !vector2i_eq(gs->pixels.size, fb->size) ||
                 gs->pixels.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    struct rect2i rects[DAMAGE_RECTS_MAX];
    size_t bytes = 0;
    int n = gpu_pixels_rects(fb, damage, whole, rects, &bytes);
    /* The quad of the copy shares the instance buffer of the slot. */
    struct gpu_glyph_inst quad;
    gpu_pixels_quad(fb->size, &quad);
    if (!vulkan_texture_ensure(g, &gs->pixels, fb->size) ||
        !vulkan_buffer_ensure(g, &slot->instances, sizeof(quad),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        (bytes && !vulkan_buffer_ensure(g, &slot->uploads, bytes,
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT)))
        return false;
    memcpy(slot->instances.mapped, &quad, sizeof(quad));
    gpu_pixels_pack(fb, rects, n, slot->uploads.mapped);

    uint32_t image_index = 0;
    if (!vulkan_swapchain_acquire(g, gs, slot, drawable->surface, fb->size, tearing, wait_ns,
                                  &image_index))
        return false;
    if (!frame_begin(slot)) {
        frame_abandon(g, gs, slot);
        return false;
    }
    VkCommandBuffer cmd = slot->cmd;
    struct barriers barriers = {0};
    if (n) {
        VkBufferImageCopy copies[DAMAGE_RECTS_MAX];
        size_t offset = 0;
        for (int i = 0; i < n; i++) {
            copies[i] = (VkBufferImageCopy){
                .bufferOffset = offset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { rects[i].x, rects[i].y, 0 },
                .imageExtent = { (uint32_t)rects[i].width, (uint32_t)rects[i].height, 1 },
            };
            offset += (size_t)rects[i].width * (size_t)rects[i].height * 4;
        }
        barriers_add(&barriers, &gs->pixels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barriers_flush(cmd, &barriers);
        vkCmdCopyBufferToImage(cmd, slot->uploads.buffer, gs->pixels.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)n, copies);
    }
    barriers_add(&barriers, &gs->pixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barriers_flush(cmd, &barriers);

    pass_begin(g, cmd, gs, image_index, fb->size, NULL);
    set_scissor(cmd, (struct rect2i){ .size = target_bounds(gs, fb->size) });
    VkDeviceSize offset = 0;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipeline_layout, 0, 1,
                            &gs->pixels.set, 0, NULL);
    vkCmdBindVertexBuffers(cmd, 0, 1, &slot->instances.buffer, &offset);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipelines[PIPE_COPY]);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    if (!frame_submit(g, gs, slot, image_index)) {
        /* The copies never ran. The next present loads every pixel. */
        gs->pixels.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        frame_abandon(g, gs, slot);
        return false;
    }
    return true;
}

bool render_gpu_present(struct render_gpu *g, struct render_surface *s, struct framebuffer *fb,
                        const struct damage_list *damage)
{
    if (!g || !s || !fb || (!fb->drawable && !fb->acquire) || !fb->pixels ||
        fb->size.x <= 0 || fb->size.y <= 0)
        return false;
    struct gpu_surface *gs = vulkan_surface_of(g, s);
    if (!gs)
        return false;
    bool resizing = frame_resizes(gs, fb);
    struct frame_slot *slot = slot_acquire(g, resizing ? RESIZE_WAIT_NS : 0);
    return slot &&
           present_pixels(g, gs, slot, fb, damage, resizing ? RESIZE_WAIT_NS : ACQUIRE_WAIT_NS);
}
