#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/QuartzCore.h>

#include "ui/yasl/metal.h"

#include "ui/yasl/stats.h"
#include "loop/latency_probe.h"
#include "yasl/yasl.h"

void *window_metal_attach(struct yasl_surface *s, void *device)
{
    if (!device)
        return NULL;
    CAMetalLayer *layer = (CAMetalLayer *)yasl_metal_get_layer(s);
    if (!layer)
        return NULL;
    layer.device = (id<MTLDevice>)device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    return layer;
}

void window_metal_detach(void *layer, struct yasl_surface *s)
{
    (void)layer;
    (void)s;
}

static bool in_live_resize(struct yasl_surface *s)
{
    struct yasl_native native = {0};
    if (yasl_surface_get_native(s, &native) != 0 || !native.ns_view)
        return false;
    return ((NSView *)native.ns_view).inLiveResize;
}

/*
 * One reference of a present watch, held by one presented handler. A block
 * retains the object that it captures when the display copies it, and the
 * display releases that object when it drops the block. The watch therefore
 * outlives the window that reported it, and the last report frees it.
 */
@interface WindowPresentRef : NSObject
{
    struct present_watch *watch;
}
- (instancetype)initWithWatch:(struct present_watch *)w;
- (struct present_watch *)presentWatch;
@end

@implementation WindowPresentRef
- (instancetype)initWithWatch:(struct present_watch *)w
{
    self = [super init];
    if (self)
        watch = present_watch_ref(w);
    return self;
}
- (struct present_watch *)presentWatch { return watch; }
- (void)dealloc
{
    present_watch_unref(watch);
    [super dealloc];
}
@end

/* The values that one presented handler reports. The handler copies them, so
 * it holds no pointer to the frame, the window, or the layer. */
struct present_report {
    /* The latency probe sample of the frame, or 0. */
    uint64_t sample;
    uint32_t refresh_ns;
    uint32_t flags;
};

/*
 * presentedTime is in the time base of CACurrentMediaTime(), in seconds, and it
 * excludes the time that the machine slept. The probe counts nanoseconds of the
 * loop clock, which includes that time, so the two clocks differ. The offset
 * between them now maps one value to the other.
 */
static uint64_t presented_time_ns(CFTimeInterval presented, uint64_t received_ns)
{
    if (presented <= 0.0)
        return 0;
    double offset = (double)received_ns - CACurrentMediaTime() * 1e9;
    double ns = presented * 1e9 + offset;
    return ns > 0.0 ? (uint64_t)(ns + 0.5) : 0;
}

/*
 * Runs on the display thread when the drawable is presented or discarded. It
 * reads the copied report and the shared probe fields only, so a window that
 * closed after the frame still costs nothing and no main-thread API runs here.
 */
static void report_presented(struct present_watch *presents, struct present_report report,
                             id<MTLDrawable> drawable)
{
    uint64_t received_ns = yasl_time_ns();
    uint64_t presented_ns = presented_time_ns(drawable.presentedTime, received_ns);
    /* A discarded frame reached no display, so it is not a presentation. */
    if (presented_ns)
        present_watch_note_presented(presents);
    latency_probe_presented(report.sample, presented_ns, received_ns, report.refresh_ns,
                            report.flags);
}

/* One drawable for one frame. A second call gives the drawable of the first,
 * so a frame submits once. */
struct acquire_state {
    CAMetalLayer *layer;
    struct yasl_surface *surface;
    struct window_metal_watch watch;
    /* The layer paces its presentation at a display refresh. */
    bool paced;
    id<CAMetalDrawable> drawable;
};

/* Attaches the presented handler of the frame. The sample of the latency probe
 * is captured here, on the thread that submits the frame, so feedback that
 * arrives late or out of order cannot complete another sample. */
static void watch_presented(struct acquire_state *state)
{
    struct present_report report = {
        .sample = state->watch.sample ? latency_probe_frame_target() : 0,
        .refresh_ns = (uint32_t)yasl_surface_frame_interval_ns(state->surface),
        .flags = state->paced ? LATENCY_PRESENTATION_VSYNC : 0,
    };
    WindowPresentRef *ref = [[WindowPresentRef alloc] initWithWatch:state->watch.presents];
    id<MTLDrawable> drawable = state->drawable;
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        report_presented([ref presentWatch], report, presented);
    }];
    [ref release];
}

/* Acquires and retains the next CAMetalDrawable for the current frame. */
static void *acquire_drawable(void *ud)
{
    struct acquire_state *state = ud;
    if (!state->drawable) {
        state->drawable = [[state->layer nextDrawable] retain];
        if (state->drawable)
            watch_presented(state);
    }
    return state->drawable;
}

bool window_metal_draw(void *layer, struct yasl_surface *s,
                       struct window_metal_watch watch,
                       window_metal_draw_fn draw, void *ud)
{
    CAMetalLayer *metal = (CAMetalLayer *)layer;
    CGSize size = metal.drawableSize;
    struct vector2i canvas = { .x = (int)size.width, .y = (int)size.height };
    if (canvas.x <= 0 || canvas.y <= 0)
        return false;
    bool transaction = in_live_resize(s);
    @autoreleasepool {
        metal.presentsWithTransaction = transaction;
        /* The frame data is built against the size of the next drawable. The
         * backend takes the drawable when it encodes, and it drops the frame
         * when the drawable has another size. */
        struct acquire_state state = {
            .layer = metal,
            .surface = s,
            .watch = watch,
            /* A paced layer presents at a display refresh. A live resize
             * presents inside the Core Animation transaction, so that frame
             * waits for no refresh. */
            .paced = metal.displaySyncEnabled && !transaction,
        };
        struct window_metal_frame frame = {
            .drawable = NULL,
            .size = canvas,
            .transaction = transaction,
            .acquire = acquire_drawable,
            .acquire_ud = &state,
        };
        draw(&frame, ud);
        if (state.drawable && transaction)
            [CATransaction flush];
        /* The frame is submitted or dropped, and the layer keeps a drawable that
         * a display still needs. Drop the reference of the frame once. */
        [state.drawable release];
        state.drawable = nil;
        frame.drawable = NULL;
    }
    return true;
}
