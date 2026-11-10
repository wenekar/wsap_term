#import <Metal/Metal.h>

#include "platform/cocoa/software_stage.h"

#include <assert.h>
#include <stdio.h>

/*
 * Tests of the staging buffers of the CPU pixels of a surface.
 *
 * A blit reads its buffer after the call that commits it returns. The ring
 * hands a buffer out again only after that command completed, so the CPU never
 * writes a buffer that the GPU still reads. With every buffer in use a frame
 * gets none, and the frame comes back at the next display frame. The ring
 * outlives the surface, so a command that completes after the surface is gone
 * is safe.
 *
 * A real device runs the commands. The test commits them itself and waits for
 * them, so nothing here depends on the speed of the GPU.
 */

#define LENGTH 4096

/* A command that reads `buffer`. The test commits it. */
static id<MTLCommandBuffer> blit_from(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                      void *buffer)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:LENGTH / 4
                                                          height:1
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    assert(texture);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    assert(command && blit);
    [blit copyFromBuffer:(id<MTLBuffer>)buffer
            sourceOffset:0
       sourceBytesPerRow:LENGTH
     sourceBytesPerImage:LENGTH
              sourceSize:MTLSizeMake(LENGTH / 4, 1, 1)
               toTexture:texture
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    /* The command holds the texture. */
    [texture release];
    return command;
}

/* Commits `command` and returns when the GPU is done with it. */
static void run(id<MTLCommandBuffer> command)
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [command addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        (void)buffer;
        dispatch_semaphore_signal(done);
    }];
    [command commit];
    long waited = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20000000000ll));
    assert(waited == 0);
    dispatch_release(done);
}

/* The ring is bounded, and it is deep enough that a frame does not wait for the
 * frame before it. A buffer that a command reads comes back only when that
 * command completed. */
static void test_completion_owns(struct software_stages *stages, id<MTLDevice> device,
                                 id<MTLCommandQueue> queue)
{
    enum { MAX = 8 };
    struct software_stage *stage[MAX];
    int count = 0;
    while (count < MAX && (stage[count] = software_stage_acquire(stages, LENGTH)) != NULL) {
        assert(software_stage_buffer(stage[count]));
        for (int j = 0; j < count; j++)
            assert(software_stage_buffer(stage[j]) != software_stage_buffer(stage[count]));
        count++;
    }
    /* One buffer for the frame and one that the GPU still reads. */
    assert(count >= 2);
    /* The ring does not grow without limit. */
    assert(count < MAX);
    assert(!software_stage_acquire(stages, LENGTH));

    id<MTLCommandBuffer> command = blit_from(device, queue, software_stage_buffer(stage[0]));
    software_stage_finish(stages, stage[0], command);
    /* The command is not committed, so the GPU does not read the buffer and it
     * is not free either. */
    assert(!software_stage_acquire(stages, LENGTH));

    run(command);
    /* The completion returned that buffer, and no other. */
    struct software_stage *back = software_stage_acquire(stages, LENGTH);
    assert(back == stage[0]);
    assert(!software_stage_acquire(stages, LENGTH));

    for (int i = 1; i < count; i++)
        software_stage_cancel(stages, stage[i]);
    software_stage_cancel(stages, back);
}

/* A larger frame gets a larger buffer. */
static void test_grow(struct software_stages *stages)
{
    struct software_stage *stage = software_stage_acquire(stages, LENGTH * 4);
    assert(stage);
    assert(software_stage_buffer(stage));
    software_stage_cancel(stages, stage);
}

/* A frame that starts no command gives its buffer back at once. */
static void test_cancel(id<MTLDevice> device)
{
    struct software_stages *stages = software_stages_new(device);
    assert(stages);
    struct software_stage *stage = software_stage_acquire(stages, LENGTH);
    assert(stage);
    software_stage_cancel(stages, stage);
    assert(software_stage_acquire(stages, LENGTH) == stage);
    software_stages_free(stages);
}

/* The surface can go away while a command still reads a buffer. */
static void test_outlives_surface(id<MTLDevice> device, id<MTLCommandQueue> queue)
{
    struct software_stages *stages = software_stages_new(device);
    assert(stages);
    struct software_stage *stage = software_stage_acquire(stages, LENGTH);
    assert(stage);
    id<MTLCommandBuffer> command = blit_from(device, queue, software_stage_buffer(stage));
    software_stage_finish(stages, stage, command);
    /* The window drops its reference before the GPU is done. */
    software_stages_free(stages);
    run(command);
}

int main(void)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = device ? [device newCommandQueue] : nil;
    if (!device || !queue) {
        fprintf(stderr, "skip: no Metal device\n");
        [queue release];
        [device release];
        return 0;
    }
    struct software_stages *stages = software_stages_new(device);
    assert(stages);
    test_completion_owns(stages, device, queue);
    test_grow(stages);
    software_stages_free(stages);
    test_cancel(device);
    test_outlives_surface(device, queue);
    [queue release];
    [device release];
    return 0;
}
