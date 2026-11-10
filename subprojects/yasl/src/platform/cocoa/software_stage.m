#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "platform/cocoa/software_stage.h"

#include <stdatomic.h>
#include <stdlib.h>

/* One buffer for the frame that fills it and up to two that the GPU still
 * reads. A frame that finds none of three free is dropped and draws again at
 * the next display frame. */
#define SOFTWARE_STAGES 3

struct software_stage {
    id<MTLBuffer> buffer;
    size_t capacity;
    /* A command that reads the buffer has not completed. The completion that
     * clears it runs on a GPU thread. */
    _Atomic bool busy;
};

struct software_stages {
    _Atomic int refs;
    id<MTLDevice> device;
    struct software_stage stage[SOFTWARE_STAGES];
};

struct software_stages *software_stages_new(void *device)
{
    if (!device)
        return NULL;
    struct software_stages *stages = calloc(1, sizeof(*stages));
    if (!stages)
        return NULL;
    atomic_init(&stages->refs, 1);
    stages->device = [(id<MTLDevice>)device retain];
    return stages;
}

void software_stages_free(struct software_stages *stages)
{
    if (!stages)
        return;
    if (atomic_fetch_sub(&stages->refs, 1) != 1)
        return;
    for (int i = 0; i < SOFTWARE_STAGES; i++)
        [stages->stage[i].buffer release];
    [stages->device release];
    free(stages);
}

struct software_stage *software_stage_acquire(struct software_stages *stages, size_t length)
{
    if (!stages || !length)
        return NULL;
    for (int i = 0; i < SOFTWARE_STAGES; i++) {
        struct software_stage *stage = &stages->stage[i];
        if (atomic_load(&stage->busy))
            continue;
        if (!stage->buffer || stage->capacity < length) {
            size_t capacity = length + length / 2;
            id<MTLBuffer> grown = [stages->device newBufferWithLength:capacity
                                                              options:MTLResourceStorageModeShared];
            if (!grown)
                return NULL;
            [stage->buffer release];
            stage->buffer = grown;
            stage->capacity = capacity;
        }
        atomic_store(&stage->busy, true);
        return stage;
    }
    return NULL;
}

void *software_stage_buffer(const struct software_stage *stage)
{
    return stage ? (void *)stage->buffer : NULL;
}

void software_stage_cancel(struct software_stages *stages, struct software_stage *stage)
{
    (void)stages;
    if (stage)
        atomic_store(&stage->busy, false);
}

void software_stage_finish(struct software_stages *stages, struct software_stage *stage,
                           void *command)
{
    if (!stages || !stage || !command) {
        software_stage_cancel(stages, stage);
        return;
    }
    /* The command holds one reference, so the ring can go away first. The block
     * captures no object: the stage array stays where it is. */
    atomic_fetch_add(&stages->refs, 1);
    [(id<MTLCommandBuffer>)command addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        atomic_store(&stage->busy, false);
        software_stages_free(stages);
    }];
}
