#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include "ui/yasl/gpu_surface.h"

#include "ui/yasl/stats.h"
#include "loop/latency_probe.h"
#include "yasl/yasl.h"

void *window_gpu_attach(struct yasl_surface *s, void *device)
{
    if (!device)
        return NULL;
    CAMetalLayer *layer = (CAMetalLayer *)yasl_metal_get_layer(s);
    if (!layer)
        return NULL;
    layer.device = (id<MTLDevice>)device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    /* Two drawables, not the default of three. With three, a window that
     * draws at each refresh showed its frames one refresh later: about 8 ms
     * more from a key press to the screen on a 120 Hz panel. Flood
     * throughput was the same with two and with three. Two drawables also
     * need one drawable less memory while the window draws. */
    layer.maximumDrawableCount = 2;
    return layer;
}

void window_gpu_detach(void *layer, struct yasl_surface *s)
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
    struct window_gpu_watch watch;
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

bool window_gpu_draw(void *layer, struct yasl_surface *s,
                       struct window_gpu_watch watch, bool transaction,
                       window_gpu_draw_fn draw, void *ud)
{
    CAMetalLayer *metal = (CAMetalLayer *)layer;
    CGSize size = metal.drawableSize;
    struct vector2i canvas = { .x = (int)size.width, .y = (int)size.height };
    if (canvas.x <= 0 || canvas.y <= 0)
        return false;
    transaction = transaction || in_live_resize(s);
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
        struct window_gpu_frame frame = {
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

static const char kCursorOverlayKey;

@interface YaslCursorOverlay : NSObject
@property (nonatomic, assign) CALayer *layer;
@property (nonatomic, assign) BOOL hasContent;
@property (nonatomic, assign) BOOL isBitmap;
@property (nonatomic, assign) uint64_t contentKey;
@property (nonatomic, assign) uint32_t solidColor;
@end

@implementation YaslCursorOverlay
- (void)dealloc
{
    [_layer removeFromSuperlayer];
    [_layer release];
    _layer = nil;
    [super dealloc];
}
@end

static NSView *surface_ns_view(struct yasl_surface *s)
{
    if (!s)
        return nil;
    struct yasl_native native = {0};
    if (yasl_surface_get_native(s, &native) != 0 || !native.ns_view)
        return nil;
    return (NSView *)native.ns_view;
}

static YaslCursorOverlay *cursor_overlay_get(NSView *view, bool create)
{
    YaslCursorOverlay *ov = objc_getAssociatedObject(view, &kCursorOverlayKey);
    if (ov || !create)
        return ov;
    CALayer *root = view.layer;
    if (!root)
        return nil;
    ov = [[YaslCursorOverlay alloc] init];
    CALayer *layer = [[CALayer layer] retain];
    layer.actions = @{
        @"hidden": [NSNull null],
        @"frame": [NSNull null],
        @"bounds": [NSNull null],
        @"position": [NSNull null],
        @"contents": [NSNull null],
        @"backgroundColor": [NSNull null],
    };
    layer.autoresizingMask = kCALayerMaxXMargin | kCALayerMinYMargin;
    layer.hidden = YES;
    ov.layer = layer;
    [root addSublayer:layer];
    objc_setAssociatedObject(view, &kCursorOverlayKey, ov, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [ov release];
    return ov;
}

bool window_gpu_cursor_supported(struct yasl_surface *s)
{
    NSView *view = surface_ns_view(s);
    return view != nil && view.layer != nil;
}

bool window_gpu_set_cursor(struct yasl_surface *s, bool active, bool visible,
                           struct rect2i rect, uint32_t color,
                           const uint32_t *pixels, uint64_t key)
{
    NSView *view = surface_ns_view(s);
    CALayer *root = view ? view.layer : nil;
    if (!root)
        return false;
    YaslCursorOverlay *ov = cursor_overlay_get(view, active);
    if (!ov)
        return false;

    @autoreleasepool {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        if (!active || rect.width <= 0 || rect.height <= 0) {
            ov.layer.hidden = YES;
            [CATransaction commit];
            return active == false;
        }
        if (ov.layer.superlayer != root)
            [root addSublayer:ov.layer];

        CGFloat scale = root.contentsScale > 0.0 ? root.contentsScale
            : (view.window ? view.window.backingScaleFactor : 1.0);
        if (scale < 1.0)
            scale = 1.0;
        CGFloat x = (CGFloat)rect.x / scale;
        CGFloat w = (CGFloat)rect.width / scale;
        CGFloat h = (CGFloat)rect.height / scale;
        CGFloat y = root.bounds.size.height - ((CGFloat)rect.y + (CGFloat)rect.height) / scale;
        ov.layer.contentsScale = scale;
        ov.layer.frame = CGRectMake(x, y, w, h);

        if (pixels) {
            /* Key 0 names no pixels, so it never matches the last upload. */
            if (!key || !ov.hasContent || !ov.isBitmap || ov.contentKey != key) {
                NSData *data = [NSData dataWithBytes:pixels
                                              length:(NSUInteger)rect.width * (NSUInteger)rect.height * 4];
                CGDataProviderRef prov = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
                CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                CGImageRef img = prov && srgb
                    ? CGImageCreate((size_t)rect.width, (size_t)rect.height, 8, 32,
                                    (size_t)rect.width * 4, srgb,
                                    kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst,
                                    prov, NULL, false, kCGRenderingIntentDefault)
                    : NULL;
                ov.layer.backgroundColor = NULL;
                ov.layer.contents = img ? (__bridge id)img : nil;
                ov.layer.opaque = YES;
                if (img) CGImageRelease(img);
                if (srgb) CGColorSpaceRelease(srgb);
                if (prov) CGDataProviderRelease(prov);
                ov.hasContent = img != NULL;
                ov.isBitmap = YES;
                ov.contentKey = key;
            }
        } else if (!ov.hasContent || ov.isBitmap || ov.solidColor != color) {
            CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGFloat rgba[4] = {
                (CGFloat)((color >> 16) & 0xff) / 255.0,
                (CGFloat)((color >> 8) & 0xff) / 255.0,
                (CGFloat)(color & 0xff) / 255.0,
                (CGFloat)((color >> 24) & 0xff) / 255.0,
            };
            CGColorRef cg = srgb ? CGColorCreate(srgb, rgba) : NULL;
            ov.layer.contents = nil;
            ov.layer.backgroundColor = cg;
            ov.layer.opaque = (color >> 24) == 0xff;
            if (cg) CGColorRelease(cg);
            if (srgb) CGColorSpaceRelease(srgb);
            ov.hasContent = cg != NULL;
            ov.isBitmap = NO;
            ov.solidColor = color;
        }
        ov.layer.hidden = !visible;
        [CATransaction commit];
    }
    return true;
}

bool window_gpu_set_cursor_visible(struct yasl_surface *s, bool visible)
{
    NSView *view = surface_ns_view(s);
    YaslCursorOverlay *ov = view ? cursor_overlay_get(view, false) : nil;
    if (!ov || !ov.layer || !ov.hasContent)
        return false;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    ov.layer.hidden = !visible;
    [CATransaction commit];
    return true;
}

void window_gpu_clear_cursor(struct yasl_surface *s)
{
    NSView *view = surface_ns_view(s);
    if (!view)
        return;
    objc_setAssociatedObject(view, &kCursorOverlayKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
