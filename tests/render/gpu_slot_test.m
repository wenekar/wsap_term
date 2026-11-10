#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "paint/framebuffer.h"
#include "render/gpu/gpu.h"
#include "render/render.h"

/*
 * Tests of the drawable that a GPU frame takes, and of the frame slot.
 *
 * A frame of a native surface has no drawable when the renderer starts. The
 * backend asks the layer for one after it builds the frame data, so a drawable
 * of the pool stays out of the pool while the CPU prepares the frame. A frame
 * that finds no free slot, no drawable, or a drawable of another size draws
 * nothing and presents nothing, and the window asks for the next frame. A frame
 * that the GPU still reads holds its slot, and the next frame asks again.
 *
 * A real device draws the frames. The drawable is a stand-in that serves a
 * texture of a size the test chooses. The test measures no time and holds no
 * command: the GPU frees a slot when it finishes the frame, so the tests retry
 * a frame that finds every slot in use.
 */

/* One texture that a frame can draw into, behind a drawable. */
@interface TestDrawable : NSObject <CAMetalDrawable>
- (instancetype)initWithDevice:(id<MTLDevice>)device size:(CGSize)size;
@end

@implementation TestDrawable {
    id<MTLTexture> _texture;
    CAMetalLayer *_layer;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device size:(CGSize)size
{
    self = [super init];
    if (!self)
        return nil;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)size.width
                                                          height:(NSUInteger)size.height
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    _texture = [device newTextureWithDescriptor:d];
    _layer = [[CAMetalLayer alloc] init];
    _layer.device = device;
    if (!_texture) {
        [self release];
        return nil;
    }
    return self;
}

- (void)dealloc
{
    [_texture release];
    [_layer release];
    [super dealloc];
}

- (id<MTLTexture>)texture { return _texture; }
- (CAMetalLayer *)layer { return _layer; }
- (NSUInteger)drawableID { return 1; }
- (CFTimeInterval)presentedTime { return 0; }
- (void)present {}
- (void)presentAtTime:(CFTimeInterval)time
{
    (void)time;
}
- (void)presentAfterMinimumDuration:(CFTimeInterval)duration
{
    (void)duration;
}
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block
{
    (void)block;
}

/*
 * Metal asks the drawable for this handler when a frame presents the drawable.
 * A layer-backed drawable of the system implements it, so a stand-in must too.
 * The handler runs here, when the frame is submitted: the test holds no
 * presentation, and a frame slot comes back when the GPU finishes the frame.
 */
- (void)addPresentScheduledHandler:(MTLDrawablePresentedHandler)block
{
    if (block)
        block(self);
}

@end

enum { CANVAS = 64 };

struct fixture {
    id<MTLDevice> device;
    struct render_gpu *gpu;
    TestDrawable *drawable;
    uint32_t *pixels;
    /* The drawable that the next frame gets, or NULL for none. */
    TestDrawable *next;
    int acquires;
};

/* The drawable of one frame, taken as late as the renderer can. */
static void *acquire(void *ud)
{
    struct fixture *f = ud;
    f->acquires++;
    return f->next;
}

/* Draws one frame with no grid and no UI: the backend clears the drawable.
 * `drawable` is what the layer offers the frame, or NULL for none. */
static enum render_gpu_status draw_with(struct fixture *f, struct framebuffer *fb,
                                        TestDrawable *drawable)
{
    static const struct render_frame frame = { .base = 0xFF203040u, .repaint = true };
    /* Every frame of a window starts with no drawable. */
    fb->drawable = NULL;
    f->acquires = 0;
    f->next = drawable;
    return render_gpu_draw(f->gpu, NULL, fb, &frame);
}

static enum render_gpu_status draw(struct fixture *f, struct framebuffer *fb)
{
    return draw_with(f, fb, f->drawable);
}

/* Draws until the backend had a free slot, so the checks below do not depend on
 * how many frames the GPU still reads. */
static enum render_gpu_status draw_in_slot(struct fixture *f, struct framebuffer *fb)
{
    for (int attempt = 0; attempt < 2000; attempt++) {
        enum render_gpu_status status = draw(f, fb);
        if (f->acquires || status != RENDER_GPU_DROPPED)
            return status;
        usleep(1000);
    }
    return RENDER_GPU_UNUSED;
}

static struct framebuffer frame_of(struct fixture *f)
{
    return (struct framebuffer){
        .pixels = f->pixels,
        .size = { .x = CANVAS, .y = CANVAS },
        .fresh = true,
        .acquire = acquire,
        .acquire_ud = f,
    };
}

/* A frame starts with no drawable. The backend asks for exactly one, after the
 * frame data is ready, and presents it. */
static void test_late_acquire(struct fixture *f)
{
    struct framebuffer fb = frame_of(f);
    assert(!fb.drawable);
    assert(draw_in_slot(f, &fb) == RENDER_GPU_PRESENTED);
    assert(f->acquires == 1);
    assert(fb.drawable == f->drawable);
}

/* No drawable means no frame, and the slot goes back to the renderer. */
static void test_no_drawable(struct fixture *f)
{
    struct framebuffer fb = frame_of(f);
    for (int attempt = 0; attempt < 2000; attempt++) {
        if (draw_with(f, &fb, NULL) == RENDER_GPU_DROPPED && f->acquires) {
            assert(!fb.drawable);
            /* The slot came back: a later frame is presented. */
            assert(draw_in_slot(f, &fb) == RENDER_GPU_PRESENTED);
            assert(f->acquires == 1);
            assert(fb.drawable == f->drawable);
            return;
        }
        usleep(1000);
    }
    assert(0 && "no frame reached the drawable step");
}

/* A drawable of another size is a resize race: the frame is dropped and nothing
 * is presented, so the window asks again with the new size. */
static void test_size_race(struct fixture *f)
{
    TestDrawable *wrong = [[TestDrawable alloc] initWithDevice:f->device
                                                          size:CGSizeMake(CANVAS * 2, CANVAS * 2)];
    assert(wrong);
    struct framebuffer fb = frame_of(f);
    for (int attempt = 0; attempt < 2000; attempt++) {
        if (draw_with(f, &fb, wrong) == RENDER_GPU_DROPPED && f->acquires) {
            assert(fb.drawable == wrong);
            [wrong release];
            /* The slot came back: a frame of the frame size is presented, and
             * it takes the drawable of that size. */
            assert(draw_in_slot(f, &fb) == RENDER_GPU_PRESENTED);
            assert(f->acquires == 1);
            assert(fb.drawable == f->drawable);
            return;
        }
        usleep(1000);
    }
    assert(0 && "no frame reached the drawable step");
}

/*
 * A frame that finds every slot in use draws nothing at once, so the main thread
 * never waits for the GPU. It takes no drawable, and the slot comes back when
 * the GPU finishes a frame.
 *
 * The GPU frees the slots on its own schedule, so the test cannot make the slots
 * full on a loaded host. It sends a burst of frames and checks a full slot only
 * when a frame finds one. It reports a burst that never filled the slots and
 * does not fail on it.
 */
static void test_slot_pressure(struct fixture *f)
{
    struct framebuffer fb = frame_of(f);
    bool full = false;
    for (int attempt = 0; attempt < 400 && !full; attempt++) {
        enum render_gpu_status status = draw(f, &fb);
        if (status == RENDER_GPU_PRESENTED) {
            /* A presented frame takes one drawable, after the frame data. */
            assert(f->acquires == 1);
            assert(fb.drawable == f->drawable);
            continue;
        }
        assert(status == RENDER_GPU_DROPPED);
        /* The frame of this size dropped before it took a drawable, so no slot
         * was free. */
        assert(!f->acquires);
        assert(!fb.drawable);
        full = true;
        /* The slots come back when the GPU is done, and the frame goes out. */
        assert(draw_in_slot(f, &fb) == RENDER_GPU_PRESENTED);
        assert(f->acquires == 1);
    }
    if (!full)
        fprintf(stderr, "skip: the GPU kept a free frame slot during the burst\n");
}

int main(void)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    struct render_gpu *gpu = render_gpu_new();
    if (!device || !gpu) {
        fprintf(stderr, "skip: no Metal device\n");
        render_gpu_free(gpu);
        if (!gpu && device)
            [device release];
        return 0;
    }
    struct fixture f = {
        .device = device,
        .gpu = gpu,
        .drawable = [[TestDrawable alloc] initWithDevice:device
                                                    size:CGSizeMake(CANVAS, CANVAS)],
        .pixels = calloc((size_t)CANVAS * CANVAS, sizeof(*f.pixels)),
    };
    assert(f.drawable && f.pixels);

    test_late_acquire(&f);
    test_no_drawable(&f);
    test_size_race(&f);
    /* The pressure test leaves a frame in flight, so it comes last. */
    test_slot_pressure(&f);

    free(f.pixels);
    [f.drawable release];
    render_gpu_free(f.gpu);
    [device release];
    return 0;
}
