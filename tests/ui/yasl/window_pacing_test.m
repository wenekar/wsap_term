/*
 * Tests of the window display cadence on macOS.
 *
 * A paced window draws once for each display tick. The tearing option reaches
 * the layer. A frame that did not submit is asked for again, and that retry
 * waits for the display too, even when the window draws unpaced.
 *
 * The test drives the display link itself, so it does not wait for the screen.
 */

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <assert.h>
#include <stdio.h>

#include "ui/yasl/internal.h"
#include "ui/yasl/metal.h"
#include "ui/yasl/stats.h"
#include "ui/yasl/window.h"
#include "util/damage.h"
#include "yasl/yasl.h"

@interface NSView (YaslPacingTest)
- (void)displayLinkFired:(id)link;
@end

struct probe {
    struct yasl_loop *loop;
    struct window_host host;
    struct window_config config;
    struct window *w;
    /* Frames that the composer drew. */
    int frames;
    /* What the last frame reported when it presented. */
    bool presented;
    /* The next frame reports that it did not submit. */
    bool fail_next;
    /* The frames of this probe present their own drawable. */
    bool own_drawable;
    id<MTLCommandQueue> queue;
};

static void compose(struct window *w, struct window_frame *frame, void *ud)
{
    struct probe *p = ud;
    struct damage_list damage = {0};
    damage_add_all(&damage);
    p->presented = !p->fail_next;
    p->fail_next = false;
    p->frames++;
    (void)window_present(w, frame, &damage, p->presented);
}

/* Draws a clear and presents it, so the display reports the frame. */
static void presenting_compose(struct window *w, struct window_frame *frame, void *ud)
{
    struct probe *p = ud;
    id<CAMetalDrawable> drawable = frame->target.acquire
        ? (id<CAMetalDrawable>)frame->target.acquire(frame->target.acquire_ud)
        : nil;
    if (drawable) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
    }
    p->frames++;
    (void)window_present(w, frame, NULL, drawable != nil);
}

/* The display period of the window, in nanoseconds. */
static uint64_t interval_ns(struct probe *p)
{
    return yasl_surface_frame_interval_ns(window_surface(p->w));
}

/* Steps the loop without a display tick. */
static void steps(struct probe *p, int count)
{
    for (int i = 0; i < count; i++)
        assert(yasl_loop_step(p->loop, 0) >= 0);
}

/* Answers the pending frame intent the way the screen does. */
static void tick(struct probe *p)
{
    struct yasl_native native = {0};
    assert(yasl_surface_get_native(window_surface(p->w), &native) == 0);
    assert(native.ns_view);
    [(NSView *)native.ns_view displayLinkFired:nil];
}

/* Steps the loop until the window drew `want` frames. It does not tick. */
static bool frames_reach(struct probe *p, int want)
{
    for (int i = 0; i < 20 && p->frames < want; i++)
        assert(yasl_loop_step(p->loop, 0) >= 0);
    return p->frames >= want;
}

static void open_window(struct probe *p, bool tearing, void *device)
{
    p->loop = yasl_loop_create();
    assert(p->loop);
    p->config = (struct window_config){
        .size = { .x = 360, .y = 220 },
        .tearing = tearing,
    };
    window_host_init(&p->host, p->loop, &p->config, NULL);
    if (p->own_drawable)
        p->queue = [(id<MTLDevice>)device newCommandQueue];
    p->w = window_open(&p->host, &(struct window_spec){
        .title = "pacing test",
        .graphics_device = device,
        .compose = p->own_drawable ? presenting_compose : compose,
        .compose_ud = p,
        .size = { .x = 360, .y = 220 },
    });
    assert(p->w && window_surface(p->w));
}

static void close_window(struct probe *p)
{
    window_host_destroy(&p->host);
    assert(yasl_loop_destroy(p->loop) == 0);
    [p->queue release];
}

/* The first frame of a new window may answer at once, because nothing was
 * answered before it. Wait for it, so the checks below start from a busy
 * display. */
static bool wait_first_frame(struct probe *p)
{
    for (int i = 0; i < 8 && p->frames == 0; i++) {
        steps(p, 1);
        if (p->frames)
            break;
        tick(p);
        steps(p, 2);
    }
    return p->frames > 0;
}

static void test_tearing_wiring(void *device)
{
    const bool tearing[] = {false, true};
    for (size_t i = 0; i < sizeof tearing / sizeof tearing[0]; i++) {
        struct probe p = {0};
        open_window(&p, tearing[i], device);
        CAMetalLayer *layer = (CAMetalLayer *)yasl_metal_get_layer(window_surface(p.w));
        assert(layer);
        assert(layer.displaySyncEnabled == (tearing[i] ? NO : YES));
        close_window(&p);
    }
}

/*
 * One display tick answers one coalesced frame intent, and each intent makes
 * one frame. The screen also ticks on its own, so a count is the oracle here:
 * after a frame, no intent is pending, so a further tick makes no frame.
 */
static void test_link_delivery(void *device)
{
    struct probe p = {0};
    open_window(&p, false, device);
    /* The display period of the window is usable for a timer. */
    uint64_t interval = interval_ns(&p);
    assert(interval >= 1000000ull && interval <= 1000000000ull);
    window_redraw(p.w);
    if (!wait_first_frame(&p)) {
        printf("window: no drawable in this session; the cadence checks are skipped\n");
        close_window(&p);
        return;
    }
    steps(&p, 4);
    int base = p.frames;
    /* One request, one tick, one frame. */
    window_redraw(p.w);
    tick(&p);
    assert(frames_reach(&p, base + 1));
    steps(&p, 4);
    assert(p.frames == base + 1);
    /* Three requests between two ticks still make one frame. */
    base = p.frames;
    window_redraw(p.w);
    window_redraw(p.w);
    window_redraw(p.w);
    tick(&p);
    assert(frames_reach(&p, base + 1));
    steps(&p, 4);
    assert(p.frames == base + 1);
    /* A tick with no request makes no frame. */
    base = p.frames;
    tick(&p);
    steps(&p, 4);
    assert(p.frames == base);
    close_window(&p);
}

/* A frame that did not submit is asked for again, and reaches the screen. The
 * counts stay apart: a draw that does not submit is a drop, and a draw that
 * submits is not a presentation. */
static void test_dropped_frame_retry(void *device)
{
    struct probe p = {0};
    open_window(&p, false, device);
    window_redraw(p.w);
    if (!wait_first_frame(&p)) {
        printf("window: no drawable in this session; the retry check is skipped\n");
        close_window(&p);
        return;
    }
    steps(&p, 4);
    int base = p.frames;
    uint64_t submitted = window_stats(p.w)->frames_submitted;
    uint64_t dropped = window_stats(p.w)->frames_dropped;
    uint64_t presented = window_stats(p.w)->frames_presented;
    p.fail_next = true;
    window_redraw(p.w);
    tick(&p);
    assert(frames_reach(&p, base + 1));
    assert(!p.presented);
    assert(window_stats(p.w)->frames_dropped == dropped + 1);
    assert(window_stats(p.w)->frames_submitted == submitted);
    /* No drawable reached the display here, so no frame is a presentation. */
    assert(window_stats(p.w)->frames_presented == presented);
    /* No new request from the test: the retry drives the next frame. */
    tick(&p);
    assert(frames_reach(&p, base + 2));
    assert(p.presented);
    assert(window_stats(p.w)->frames_submitted == submitted + 1);
    assert(window_stats(p.w)->frames_presented == presented);
    steps(&p, 4);
    assert(p.frames == base + 2);
    close_window(&p);
}

/* An unpaced window draws the frame that it is asked for at once. */
static void test_unpaced_draw(void *device)
{
    struct probe p = {0};
    open_window(&p, true, device);
    window_redraw(p.w);
    if (!wait_first_frame(&p)) {
        printf("window: no drawable in this session; the unpaced checks are skipped\n");
        close_window(&p);
        return;
    }
    steps(&p, 4);
    int base = p.frames;
    window_redraw(p.w);
    assert(frames_reach(&p, base + 1));
    steps(&p, 4);
    assert(p.frames == base + 1);
    /* A retry of an unpaced window still waits for the display. */
    base = p.frames;
    p.fail_next = true;
    window_redraw(p.w);
    assert(frames_reach(&p, base + 1));
    assert(!p.presented);
    tick(&p);
    assert(frames_reach(&p, base + 2));
    assert(p.presented);
    close_window(&p);
}

/* Waits until the window counts a presentation. The display reports on its own
 * thread, so the test drives frames until the count moves. */
static bool wait_presented(struct probe *p)
{
    for (int i = 0; i < 60; i++) {
        window_redraw(p->w);
        tick(p);
        steps(p, 2);
        if (window_stats(p->w)->frames_presented)
            return true;
        [NSThread sleepForTimeInterval:0.005];
    }
    return false;
}

/* The display reports a frame that it showed, and only that frame counts as a
 * presentation. The test presents its own drawable, so it needs no renderer. */
static void test_presentation_report(void *device)
{
    struct probe p = { .own_drawable = true };
    open_window(&p, false, device);
    window_redraw(p.w);
    if (!wait_first_frame(&p)) {
        printf("window: no drawable in this session; the presentation check is skipped\n");
        close_window(&p);
        return;
    }
    steps(&p, 4);
    assert(window_stats(p.w)->frames_submitted > 0);
    if (!wait_presented(&p)) {
        printf("session: no display reports a presentation; the presentation check is skipped\n");
        close_window(&p);
        return;
    }
    /* A presentation follows a submission of the same window, and a submitted
     * frame alone is not a presentation. */
    assert(window_stats(p.w)->frames_presented <= window_stats(p.w)->frames_submitted);
    close_window(&p);
}

/*
 * A frame that the renderer drops after it took its drawable. The renderer
 * encodes inside an autorelease pool of its own, so the frame must keep the
 * drawable alive after that pool is gone: the CPU fallback then draws into the
 * same drawable.
 */
struct drawn_frame {
    /* Borrowed: the frame under test owns the drawable. */
    id<CAMetalDrawable> drawable;
    id<MTLTexture> texture;
    /* The frame ownership while the frame ran. */
    NSUInteger held_inside;
    bool usable_after_pool;
};

static void dropping_compose(const struct window_metal_frame *frame, void *ud)
{
    struct drawn_frame *f = ud;
    @autoreleasepool {
        /* The inner pool of the renderer: the drawable is acquired inside it,
         * and the encoder fails, so this pool is drained before the CPU
         * fallback runs. */
        f->drawable = frame->acquire
            ? (id<CAMetalDrawable>)frame->acquire(frame->acquire_ud)
            : nil;
        assert(f->drawable);
        f->texture = f->drawable.texture;
        f->held_inside = [f->drawable retainCount];
        /* The test holds its own reference, so the checks below read a live
         * object even when the frame does not keep one. */
        [f->drawable retain];
    }
    /* Nothing was submitted: this is RENDER_GPU_UNUSED, and the CPU pixels are
     * presented into the same drawable after the pool above is gone. */
    f->usable_after_pool = f->drawable.texture == f->texture;
}

/* The frame owns its drawable for the whole draw, and releases it once. */
static void test_drawable_lifetime(void *device)
{
    struct probe p = {0};
    open_window(&p, false, device);
    CAMetalLayer *layer = (CAMetalLayer *)yasl_metal_get_layer(window_surface(p.w));
    assert(layer);
    struct drawn_frame f = {0};
    if (!window_metal_draw(layer, window_surface(p.w),
                           (struct window_metal_watch){0}, dropping_compose, &f)) {
        printf("window: no drawable in this session; the drawable check is skipped\n");
        close_window(&p);
        return;
    }
    assert(f.usable_after_pool);
    assert(f.drawable.texture == f.texture);
    /* The frame held one reference of the drawable, and it released exactly
     * one: the test reference is the only one that is left after the call. */
    NSUInteger after = [f.drawable retainCount];
    assert(f.held_inside == after + 1);
    [f.drawable release];
    close_window(&p);
}

/* The native surface of the window that owns the display feedback. */
static struct window_native *main_native(struct window *w)
{
    for (int i = 0; i < w->native_count; i++)
        if (w->natives[i].surface == window_surface(w))
            return &w->natives[i];
    return NULL;
}

/* The counts of a window are current when they are read. A report that the
 * display sends while the window draws nothing does not wait for another draw.
 */
static void test_idle_presentation_stats(void *device)
{
    /* One submit, then a read of the count with no draw in between. A frame
     * that the GPU drops asks for another frame, so an attempt that draws
     * during the wait is repeated instead of counted. */
    struct probe p = { .own_drawable = true };
    open_window(&p, false, device);
    window_redraw(p.w);
    if (wait_first_frame(&p)) {
        bool seen = false;
        for (int attempt = 0; attempt < 8 && !seen; attempt++) {
            int want = p.frames + 1;
            window_redraw(p.w);
            if (!frames_reach(&p, want)) {
                tick(&p);
                steps(&p, 2);
            }
            if (p.frames != want)
                continue;
            int drew = p.frames;
            uint64_t presented = window_stats(p.w)->frames_presented;
            for (int i = 0; i < 40 && !seen; i++) {
                [NSThread sleepForTimeInterval:0.005];
                steps(&p, 1);
                if (p.frames != drew)
                    break;
                seen = window_stats(p.w)->frames_presented > presented;
            }
        }
        if (!seen)
            printf("session: no display reports a presentation; the idle count check is used\n");
    }
    close_window(&p);

    /* The counter path of every display report, read with no draw at all. This
     * is the call that the presented handler makes. */
    struct probe q = {0};
    open_window(&q, false, device);
    /* One frame, so the window owns the counter that the display reports into.
     * This composer takes no drawable, so no display reports a presentation. */
    window_redraw(q.w);
    if (!wait_first_frame(&q)) {
        printf("window: no drawable in this session; the idle count check is skipped\n");
        close_window(&q);
        return;
    }
    steps(&q, 4);
    struct window_native *n = main_native(q.w);
    assert(n && n->watch);
    int drew = q.frames;
    uint64_t presented = window_stats(q.w)->frames_presented;
    present_watch_note_presented(n->watch);
    assert(window_stats(q.w)->frames_presented == presented + 1);
    assert(q.frames == drew);
    close_window(&q);
}

int main(void)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    assert(device);
    test_tearing_wiring(device);
    test_link_delivery(device);
    test_dropped_frame_retry(device);
    test_unpaced_draw(device);
    test_presentation_report(device);
    test_drawable_lifetime(device);
    test_idle_presentation_stats(device);
    [device release];
    return 0;
}
