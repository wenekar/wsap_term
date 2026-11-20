#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#if YASL_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

#include "platform/cocoa/api.h"
#include "common/backend.h"
#include "common/public_internal.h"
#include "common/private_native_cocoa.h"
#include "common/timing.h"
#include "common/loop.h"
#include "platform/cocoa/keycodes.h"
#include "common/keyboard.h"
#include "keymap.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>
#include <objc/message.h>
#include "platform/cocoa/software_stage.h"

#include <objc/runtime.h>

#define LINK_IDLE_FRAMES 8
/* A display tick answers a frame intent. When no tick comes, the fallback
 * answers it after two measured frame periods, but not sooner than 16 ms and
 * not later than 250 ms. A live link always answers first. */
#define TICK_FALLBACK_MIN_NS 16000000ull
#define TICK_FALLBACK_MAX_NS 250000000ull
/* Application-defined event subtype that only wakes a waiting dispatch. */
#define LOOP_WAKE_SUBTYPE 0x5953
/* Wait bound while some loop descriptor cannot be watched with kqueue. */
#define LOOP_FALLBACK_MS 10
#define SCROLL_POINTS_PER_LINE 10.0

/* Values of yasl_private_pointer_event.scroll_source and scroll_phase. They
 * match the public scroll enums. */
enum {
    SCROLL_SOURCE_WHEEL  = 1,
    SCROLL_SOURCE_FINGER = 2,
    SCROLL_PHASE_BEGIN   = 1,
    SCROLL_PHASE_UPDATE  = 2,
    SCROLL_PHASE_END     = 3,
};

enum {
    DEVICE_LCTL   = 0x0001,
    DEVICE_LSHIFT = 0x0002,
    DEVICE_RSHIFT = 0x0004,
    DEVICE_LCMD   = 0x0008,
    DEVICE_RCMD   = 0x0010,
    DEVICE_LALT   = 0x0020,
    DEVICE_RALT   = 0x0040,
    DEVICE_RCTL   = 0x2000,
};

@interface YaslView : NSView <NSTextInputClient>
@property (nonatomic, assign) struct cocoa_surface *surface;
@property (nonatomic, assign) BOOL cpuLayer;
- (instancetype)initWithFrame:(NSRect)frameRect cpuLayer:(BOOL)cpuLayer;
@end

@interface YaslAppDelegate : NSObject <NSApplicationDelegate>
@end

@interface YaslWindow : NSWindow
@property (nonatomic, assign) struct cocoa_surface *surface;
@end

/* The delegate of a YASL window. It is a plain object, not an
 * NSWindowController, because AppKit refits the frame of a window that a
 * window controller owns when a drag takes it to another display.
 * Note (2026-11-20): macOS 27.2 refits during the drag, so the window jumps
 * away from the pointer. */
@interface YaslWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) struct cocoa_surface *surface;
@property (nonatomic, assign) NSWindow *window;
@property (nonatomic, assign) BOOL fullscreenTransition;
@end

#include "common/input.h"

enum { COCOA_CPU_SLOTS = 3 };
/* The time without a commit after which a CPU surface frees its spare slots. */
#define CPU_TRIM_IDLE_NS (150ull * 1000000ull)

struct cocoa_cpu_slot {
    IOSurfaceRef surface;
    struct yasl_private_vector2i size;
    bool locked;
};

struct cocoa_surface {
    struct yasl_surface base;
    YaslWindow *window;
    YaslWindowDelegate *delegate;
    YaslView *view;
    CALayer *layer;
    CAMetalLayer *metal_layer;
    id<MTLCommandQueue> software_queue;
    /* The staging buffers of the CPU pixels. See software_stage.h. */
    struct software_stages *software_stages;
    struct cocoa_cpu_slot cpu_slots[COCOA_CPU_SLOTS];
    bool use_iosurface;
    bool has_shown;
    bool direct_acquired;
    bool present_acquired;
    int shown_slot;
    int direct_slot;
    int present_slot;
    CADisplayLink *link;

    struct cocoa_surface *parent;
    bool is_popup;
    /* Creation order of a popup, so the newest one is the topmost. */
    unsigned popup_order;
    struct yasl_private_vector2i content_offset;

    const struct yasl_private_window_cbs *cbs;
    void *ud;
    uint64_t frames_skipped;
    uint64_t move_trace_ns, place_trace_ns;

    char *preedit;
    size_t preedit_begin, preedit_end;
    /* The selected range of the preedit, in UTF-8 bytes; -1 if unknown. */
    int preedit_cursor_begin, preedit_cursor_end;
    NSUInteger marked_utf16_location;
    uint64_t preedit_revision, preedit_generation;
    struct yasl_private_rect2i ime_caret;

    struct yasl_private_vector2i size;
    uint32_t scale_120;
    uint32_t bg_color;
    uint32_t *pixels;
    size_t pixel_capacity;

    id<CAMetalDrawable> drawable;
    bool transaction;
    struct yasl_private_frame frame;
    struct yasl_private_damage_list damage;
    bool did_present;
    /* Extent of the frame in pixels, valid while did_present is set. */
    struct yasl_private_vector2i shown_size;
    unsigned idle_frames;
    /* One coalesced frame intent of an application-drawn surface waits for the
     * next display tick. A fallback timer answers it when no tick comes. */
    bool want_draw;
    yasl_private_timer_id tick_timer;
    yasl_private_timer_id trim_timer;
    /* The loop time of the last IOSurface commit. */
    uint64_t cpu_commit_ns;
    bool frame_due;
    bool unpaced;
    bool rendering;
    bool closed;
    bool destroyed;
    struct cocoa_surface *dead_next;

    struct input_queue input;
    struct input_queue text_events;
    uint64_t text_key_id;
    unsigned key_insertions;
    bool interpreting_key;
    bool key_composing;
    enum yasl_key_text key_text_input[256];
    uint32_t key_consumed_mods[256];
    struct yasl_private_vector2i pointer;
    double scroll_x;
    uint32_t mods;
    uint32_t buttons;
    enum yasl_private_cursor cursor;
    /* The pointer is locked: pos holds its last position and motion arrives as
     * device deltas. */
    bool pointer_locked;
    enum yasl_private_pointer_device tablet_device;
    bool tablet_down;
    uint64_t pen_device_id, pen_tool_id;
    uint32_t pen_buttons;

    /* Retained mouse-down of the press in progress, for interactive moves. */
    NSEvent *mouse_down;
    /* The primary press in progress fell in a title bar area: YASL moves the
     * window, and the press reaches the application as no event. */
    bool titlebar_press;
    struct yasl_private_frame_clock clock;
};

struct cocoa_display {
    struct yasl_private_display base;
    struct cocoa_surface **windows;
    int count;
    int capacity;
    char *app_id;
    struct yasl_loop *loop;
    id event_monitor;
    NSCursor *hidden_cursor;
    /* Diagonal resize cursors, made on first use. */
    NSCursor *diagonal_cursors[2];
    YaslAppDelegate *delegate;
    NSPoint cascade;
    struct cocoa_surface *dead;
    unsigned popup_serial;
    struct {
        bool active;
        struct cocoa_surface *origin;
        struct cocoa_surface *carried;
        NSPoint carry_offset;
        struct cocoa_surface *hover;
        struct yasl_private_vector2i hover_pos;
        struct yasl_private_drag_listener listener;
        void *ud;
        NSImage *image;
        NSWindow *image_window;
        NSPoint image_hotspot;
    } drag;
    int depth;
    bool open;
    bool pumping;
    bool owns_loop;
};

static struct cocoa_display darwin_ctx;

static unsigned screen_id(NSScreen *screen)
{
    return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

static void trace_move_screens(void)
{
    if (!darwin_ctx.base.trace_moves)
        return;
    for (NSScreen *screen in NSScreen.screens) {
        NSRect frame = screen.frame;
        NSRect visible = screen.visibleFrame;
        yasl_private_move_trace(&darwin_ctx.base, NULL, "screen",
                "id=%u scale=%g frame=(%g,%g %gx%g) visible=(%g,%g %gx%g)",
                screen_id(screen), screen.backingScaleFactor,
                NSMinX(frame), NSMinY(frame), NSWidth(frame), NSHeight(frame),
                NSMinX(visible), NSMinY(visible), NSWidth(visible), NSHeight(visible));
    }
}

static bool move_trace_sample(uint64_t *last)
{
    uint64_t now = yasl_private_loop_now_ns();
    if (*last && now - *last < 40000000)
        return false;
    *last = now;
    return true;
}

struct move_request {
    NSPoint mouse, grab, origin;
};

static void trace_window_move(struct cocoa_surface *w, const char *stage,
                              const struct move_request *request)
{
    if (!darwin_ctx.base.trace_moves || !w || !w->window || w->destroyed || w->closed)
        return;
    NSWindow *win = w->window;
    NSRect frame = win.frame;
    NSEvent *event = NSApp.currentEvent;
    NSInteger subtype = event.type == NSEventTypeAppKitDefined ? event.subtype : 0;
    NSPoint mouse = request ? request->mouse : NSEvent.mouseLocation;
    char placement[160] = "";
    if (request)
        snprintf(placement, sizeof(placement), " requested=(%g,%g) grab=(%g,%g)",
                 request->origin.x, request->origin.y, request->grab.x, request->grab.y);
    yasl_private_move_trace(&darwin_ctx.base, &w->base, stage,
            "win=%ld screen=%u backing=%g scale120=%u mouse=(%g,%g) frame=(%g,%g %gx%g) "
            "event=%lu subtype=%ld event_win=%ld held=%lu carry=%d%s",
            (long)win.windowNumber, screen_id(win.screen), win.backingScaleFactor,
            w->scale_120, mouse.x, mouse.y, NSMinX(frame), NSMinY(frame),
            NSWidth(frame), NSHeight(frame), (unsigned long)event.type,
            (long)subtype, (long)event.windowNumber, (unsigned long)NSEvent.pressedMouseButtons,
            darwin_ctx.drag.carried == w, placement);
}

/* The wait of the fallback that answers a frame intent when no display tick
 * comes. Two frame periods is longer than one tick, so a live link always
 * answers first. */
static uint64_t tick_wait_ns(const struct cocoa_surface *w)
{
    uint64_t wait = yasl_private_clock_refresh_ns(w->clock.refresh_ns) * 2;
    if (wait < TICK_FALLBACK_MIN_NS)
        wait = TICK_FALLBACK_MIN_NS;
    if (wait > TICK_FALLBACK_MAX_NS)
        wait = TICK_FALLBACK_MAX_NS;
    return wait;
}

static void cancel_tick_timer(struct cocoa_surface *w)
{
    if (!w->tick_timer)
        return;
    yasl_private_surface_del_timer(&w->base, w->tick_timer);
    w->tick_timer = 0;
}

/* Answers the frame intent when no display tick arrived, so a request never
 * waits for a tick that cannot come. */
static uint64_t tick_fallback(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    struct cocoa_surface *w = ud;
    w->tick_timer = 0;
    if (!w->want_draw)
        return 0;
    w->want_draw = false;
    yasl_private_surface_notify_frame_shown(&w->base);
    if (darwin_ctx.loop)
        yasl_private_loop_kick(darwin_ctx.loop);
    return 0;
}

static void drag_end(struct cocoa_display *d, enum yasl_private_drag_end how)
{
    if (!d || !d->drag.active) return;
    struct yasl_private_drag_listener l = d->drag.listener;
    void *ud = d->drag.ud;
    struct cocoa_surface *hover = d->drag.hover;
    struct yasl_private_vector2i pos = d->drag.hover_pos;
    trace_window_move(d->drag.carried ? d->drag.carried : d->drag.origin,
                      how == YASL_PRIVATE_DRAG_DROPPED ? "drag-drop" : "drag-end", NULL);

    d->drag.active = false;
    d->drag.listener = (struct yasl_private_drag_listener){0};
    d->drag.hover = NULL;
    d->drag.origin = NULL;
    d->drag.carried = NULL;

    if (d->drag.image_window) {
        [d->drag.image_window close];
        [d->drag.image_window release];
        d->drag.image_window = nil;
    }
    [d->drag.image release];
    d->drag.image = nil;

    if (hover && !hover->destroyed) {
        darwin_ctx.depth++;
        if (how == YASL_PRIVATE_DRAG_DROPPED)
            yasl_private_surface_notify_drag(&hover->base, YASL_PRIVATE_DRAG_DROP, pos);
        else
            yasl_private_surface_notify_drag(&hover->base, YASL_PRIVATE_DRAG_LEAVE, pos);
        darwin_ctx.depth--;
    }
    if (l.end)
        l.end(ud, how);
}

static void render_and_commit(struct cocoa_surface *w);

/* Grows by half, so a live resize that moves one pixel at a time does not
 * reallocate each step. */
static size_t grown_capacity(size_t cap, size_t need)
{
    size_t grown = cap + cap / 2;
    return grown > need ? grown : need;
}

static void reap_dead(struct cocoa_display *d);
static void push_pen(struct cocoa_surface *w, NSEvent *event, unsigned action, bool contact);
static void apply_pointer_lock(struct cocoa_surface *w);
static void dismiss_popup_press(const struct cocoa_surface *pressed);
static bool place_under_pointer(struct cocoa_surface *w, NSPoint at);

static void set_state(struct cocoa_surface *w, uint32_t bit, bool on)
{
    if (!w || w->destroyed || w->closed)
        return;
    uint32_t next = on ? (w->base.state | bit) : (w->base.state & ~bit);
    if (next == w->base.state)
        return;
    w->base.state = next;
    darwin_ctx.depth++;
    yasl_private_surface_notify_state(&w->base, next);
    darwin_ctx.depth--;
}

static void check_pointer_reset(struct cocoa_surface *w)
{
    if (!w || w->closed) return;
    if (w->buttons != 0 || w->tablet_down || w->tablet_device != YASL_PRIVATE_PTR_DEV_MOUSE) {
        if (w->tablet_device != YASL_PRIVATE_PTR_DEV_MOUSE)
            push_pen(w, nil, YASL_PRIVATE_PEN_CANCEL, false);
        struct yasl_private_event ev = {
            .type = YASL_PRIVATE_EVENT_INPUT_RESET,
            .surface = &w->base,
        };
        input_queue_push(&w->input, &ev);
        w->buttons = 0;
        w->tablet_down = false;
        w->tablet_device = YASL_PRIVATE_PTR_DEV_MOUSE;
        w->pen_device_id = w->pen_tool_id = 0;
    }
}

static void push_focus(struct cocoa_surface *w, bool focused)
{
    if (!w || w->closed)
        return;
    if (!focused) {
        check_pointer_reset(w);
        memset(w->key_text_input, 0, sizeof(w->key_text_input));
        memset(w->key_consumed_mods, 0, sizeof(w->key_consumed_mods));
    }
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_FOCUS,
        .surface = &w->base,
        .focus = { .focused = focused },
    };
    input_queue_push(&w->input, &ev);
    darwin_ctx.depth++;
    set_state(w, YASL_PRIVATE_STATE_ACTIVATED, focused);
    apply_pointer_lock(w);
    if (!w->destroyed && !w->closed)
        cocoa_surface_schedule_redraw(w);
    darwin_ctx.depth--;
}

static void cpu_unlock_slot(struct cocoa_cpu_slot *slot)
{
    if (slot->locked && slot->surface) {
        IOSurfaceUnlock(slot->surface, 0, NULL);
        slot->locked = false;
    }
}

static void cpu_release_slot(struct cocoa_cpu_slot *slot)
{
    cpu_unlock_slot(slot);
    if (slot->surface) {
        CFRelease(slot->surface);
        slot->surface = NULL;
    }
    slot->size = (struct yasl_private_vector2i){0};
}

static void cpu_release_all_slots(struct cocoa_surface *w)
{
    for (int i = 0; i < COCOA_CPU_SLOTS; i++)
        cpu_release_slot(&w->cpu_slots[i]);
    w->has_shown = false;
    w->direct_acquired = false;
    w->present_acquired = false;
}

static bool cpu_trim_idle_slots(struct cocoa_surface *w)
{
    if (!w || !w->use_iosurface)
        return true;
    bool spare_busy = false;
    for (int i = 0; i < COCOA_CPU_SLOTS; i++) {
        struct cocoa_cpu_slot *slot = &w->cpu_slots[i];
        if ((w->has_shown && i == w->shown_slot) ||
            (w->direct_acquired && i == w->direct_slot) ||
            (w->present_acquired && i == w->present_slot))
            continue;
        if (slot->surface) {
            if (!IOSurfaceIsInUse(slot->surface))
                cpu_release_slot(slot);
            else
                spare_busy = true;
        }
    }
    return !spare_busy;
}

static void cancel_trim_timer(struct cocoa_surface *w)
{
    if (!w->trim_timer)
        return;
    yasl_private_surface_del_timer(&w->base, w->trim_timer);
    w->trim_timer = 0;
}

static uint64_t cpu_trim_timer_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct cocoa_surface *w = ud;
    if (!w || w->closed || w->destroyed) {
        if (w) w->trim_timer = 0;
        return 0;
    }
    /* A commit came after the timer started. Wait for the rest of the idle time. */
    uint64_t idle_ns = yasl_private_loop_now_ns() - w->cpu_commit_ns;
    if (idle_ns < CPU_TRIM_IDLE_NS)
        return CPU_TRIM_IDLE_NS - idle_ns;
    if (cpu_trim_idle_slots(w)) {
        w->trim_timer = 0;
        return 0;
    }
    return CPU_TRIM_IDLE_NS;
}

/* Frees the spare slots after the surface shows no new frame for
 * CPU_TRIM_IDLE_NS. A surface that draws keeps its spare slots, because the
 * window server holds the last frames for some time and the next frame needs
 * a free slot. A trim at each commit would allocate an IOSurface for almost
 * every frame. */
static void schedule_cpu_trim(struct cocoa_surface *w)
{
    if (!w || !w->use_iosurface || w->closed || w->destroyed || w->trim_timer)
        return;
    w->trim_timer = yasl_private_surface_add_timer(&w->base, CPU_TRIM_IDLE_NS,
                                                   cpu_trim_timer_cb, w);
}

static void cpu_trim_stale_slots(struct cocoa_surface *w)
{
    if (!w->use_iosurface)
        return;
    for (int i = 0; i < COCOA_CPU_SLOTS; i++) {
        struct cocoa_cpu_slot *slot = &w->cpu_slots[i];
        if ((w->has_shown && i == w->shown_slot) ||
            (w->direct_acquired && i == w->direct_slot) ||
            (w->present_acquired && i == w->present_slot))
            continue;
        if (slot->surface &&
            (slot->size.x != w->size.x || slot->size.y != w->size.y) &&
            !IOSurfaceIsInUse(slot->surface))
            cpu_release_slot(slot);
    }
}

/* Tags a CPU surface as sRGB. The Metal layer of a GPU surface is sRGB too.
 * A surface with no tag can show its values with no color match, and the
 * same pixel then has a different color on a wide-gamut display. */
static void cpu_tag_srgb(IOSurfaceRef surface)
{
    static CFPropertyListRef srgb_plist;
    if (!srgb_plist) {
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (srgb) {
            srgb_plist = CGColorSpaceCopyPropertyList(srgb);
            CGColorSpaceRelease(srgb);
        }
    }
    if (srgb_plist)
        IOSurfaceSetValue(surface, kIOSurfaceColorSpace, srgb_plist);
}

static int cpu_acquire_slot(struct cocoa_surface *w, uint32_t **out_pixels, int *out_stride_bytes)
{
    if (!w || !w->use_iosurface || w->size.x <= 0 || w->size.y <= 0 ||
        (size_t)w->size.x > (size_t)INT_MAX / sizeof(uint32_t)) {
        errno = EINVAL;
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < COCOA_CPU_SLOTS; i++) {
        if ((w->has_shown && i == w->shown_slot) ||
            (w->direct_acquired && i == w->direct_slot) ||
            (w->present_acquired && i == w->present_slot))
            continue;
        struct cocoa_cpu_slot *slot = &w->cpu_slots[i];
        if (slot->surface && !IOSurfaceIsInUse(slot->surface)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        for (int i = 0; i < COCOA_CPU_SLOTS; i++) {
            if ((w->has_shown && i == w->shown_slot) ||
                (w->direct_acquired && i == w->direct_slot) ||
                (w->present_acquired && i == w->present_slot))
                continue;
            if (!w->cpu_slots[i].surface) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        errno = EAGAIN;
        return -1;
    }
    struct cocoa_cpu_slot *slot = &w->cpu_slots[idx];
    if (slot->surface && (slot->size.x != w->size.x || slot->size.y != w->size.y))
        cpu_release_slot(slot);
    size_t bpr = (size_t)w->size.x * sizeof(uint32_t);
    if (!slot->surface) {
        NSDictionary *props = @{
            (id)kIOSurfaceWidth: @(w->size.x),
            (id)kIOSurfaceHeight: @(w->size.y),
            (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfaceBytesPerRow: @(bpr),
            (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
        };
        slot->surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        if (!slot->surface) {
            errno = ENOMEM;
            return -1;
        }
        cpu_tag_srgb(slot->surface);
    }
    if (IOSurfaceLock(slot->surface, 0, NULL) != kIOReturnSuccess) {
        cpu_release_slot(slot);
        errno = EIO;
        return -1;
    }
    size_t actual_bpr = IOSurfaceGetBytesPerRow(slot->surface);
    void *addr = IOSurfaceGetBaseAddress(slot->surface);
    if (!addr || actual_bpr != bpr) {
        IOSurfaceUnlock(slot->surface, 0, NULL);
        cpu_release_slot(slot);
        errno = actual_bpr != bpr ? ENOTSUP : ENOMEM;
        return -1;
    }
    slot->locked = true;
    slot->size = w->size;
    *out_pixels = (uint32_t *)addr;
    *out_stride_bytes = (int)bpr;
    return idx;
}

static void cpu_commit_slot(struct cocoa_surface *w, int idx)
{
    struct cocoa_cpu_slot *slot = &w->cpu_slots[idx];
    cpu_unlock_slot(slot);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    w->layer.contents = (__bridge id)slot->surface;
    [CATransaction commit];
    if (w->view && w->view.inLiveResize)
        [CATransaction flush];
    w->shown_slot = idx;
    w->has_shown = true;
    w->shown_size = w->size;
    w->cpu_commit_ns = yasl_private_loop_now_ns();
    yasl_private_clock_presented(&w->clock, w->cpu_commit_ns, false);
    schedule_cpu_trim(w);
}

static CGFloat surface_scale(const struct cocoa_surface *w)
{
    return w && w->scale_120 ? (CGFloat)w->scale_120 / YASL_PRIVATE_SCALE_UNIT : 1.0;
}

static void sync_size(struct cocoa_surface *w, bool force)
{
    if (!w || w->destroyed || w->closed || !w->view || !w->window)
        return;
    uint64_t start = yasl_private_loop_now_ns();
    NSRect bounds = w->view.bounds;
    /* The popup uses its parent's pixel coordinates, even if AppKit puts
     * the popup on another display. Use that scale for its buffer and input. */
    CGFloat scale = w->parent ? surface_scale(w->parent)
                              : MAX(1.0, w->window.backingScaleFactor);
    struct yasl_private_vector2i size = {
        .x = (int)lround(bounds.size.width * scale),
        .y = (int)lround(bounds.size.height * scale),
    };
    uint32_t scale_120 = (uint32_t)lround(scale * YASL_PRIVATE_SCALE_UNIT);
    bool rescaled = force || scale_120 != w->scale_120;
    bool resized = force || (size.x != w->size.x || size.y != w->size.y);
    bool layer_rescaled = w->layer && w->layer.contentsScale != scale;
    if (!rescaled && !resized && !layer_rescaled)
        return;
    darwin_ctx.depth++;
    if (layer_rescaled)
        w->layer.contentsScale = scale;
    if (!rescaled && !resized) {
        darwin_ctx.depth--;
        return;
    }
    w->size = size;
    w->scale_120 = scale_120;
    if (w->metal_layer)
        w->metal_layer.drawableSize = CGSizeMake(size.x, size.y);
    cpu_trim_stale_slots(w);
    /* Finish local bookkeeping before publishing geometry and requesting a frame. */
    yasl_private_clock_note_resize(&w->clock, yasl_private_loop_now_ns() - start);
    yasl_private_surface_notify_configure(&w->base, (struct yasl_private_geometry){
            .size = size, .scale_120 = scale_120,
        });
    /* A popup takes its scale from the parent, which can change while the popup
     * keeps its display. */
    if (rescaled)
        for (int i = 0; i < darwin_ctx.count; i++)
            if (darwin_ctx.windows[i]->parent == w)
                sync_size(darwin_ctx.windows[i], false);
    /* During a live resize the refresh callback draws and presents the frame. */
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU || w->view.inLiveResize)
        yasl_private_surface_notify_refresh(&w->base);
    darwin_ctx.depth--;
}

/* Reports the system title bar over the content of an overlay toplevel: its
 * height and the area of the window buttons. Call it after a change of size,
 * full screen state, or screen. */
static void sync_titlebar(struct cocoa_surface *w)
{
    if (!w || w->destroyed || w->closed || w->is_popup || !w->view || !w->window)
        return;
    struct yasl_private_titlebar titlebar = {0};
    NSWindow *window = w->window;
    if ((window.styleMask & NSWindowStyleMaskFullSizeContentView) &&
        !(window.styleMask & NSWindowStyleMaskFullScreen)) {
        NSRect bounds = w->view.bounds;
        NSRect layout = [w->view convertRect:window.contentLayoutRect fromView:nil];
        CGFloat top = NSHeight(bounds);
        titlebar.height = (int)lround(MAX(0.0, NSMaxY(bounds) - NSMaxY(layout)));
        NSRect controls = NSZeroRect;
        for (NSWindowButton kind = NSWindowCloseButton; kind <= NSWindowZoomButton; kind++) {
            NSButton *button = [window standardWindowButton:kind];
            if (button && !button.hidden)
                controls = NSUnionRect(controls, [w->view convertRect:button.bounds
                                                             fromView:button]);
        }
        if (!NSIsEmptyRect(controls)) {
            int x0 = (int)floor(NSMinX(controls)), x1 = (int)ceil(NSMaxX(controls));
            int y0 = (int)floor(top - NSMaxY(controls)), y1 = (int)ceil(top - NSMinY(controls));
            titlebar.controls = (struct yasl_private_rect2i){
                .x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0,
            };
        }
    }
    darwin_ctx.depth++;
    yasl_private_surface_notify_titlebar(&w->base, titlebar);
    darwin_ctx.depth--;
}

/* Returns true when `event` is a primary press in a title bar area of a
 * windowed toplevel. Call it for each press, and in acceptsFirstMouse:. A
 * Control press is a secondary click, so it returns false. */
static bool is_titlebar_press(const struct cocoa_surface *w, NSEvent *event)
{
    if (!w || w->is_popup || w->closed || !w->view || !w->window || !event ||
        event.type != NSEventTypeLeftMouseDown ||
        (event.modifierFlags & NSEventModifierFlagControl) ||
        (w->window.styleMask & NSWindowStyleMaskFullScreen))
        return false;
    NSPoint p = [w->view convertPoint:event.locationInWindow fromView:nil];
    return yasl_private_surface_in_titlebar_area(&w->base, p.x, NSHeight(w->view.bounds) - p.y);
}

/* Zooms, fills, or minimizes `window`, as the system setting for a double
 * click on a title bar asks. Call it on the second press of a double click
 * in a title bar area. */
static void titlebar_double_click(NSWindow *window)
{
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *action = [defaults stringForKey:@"AppleActionOnDoubleClick"];
    if (!action && [defaults boolForKey:@"AppleMiniaturizeOnDoubleClick"])
        action = @"Minimize";
    if ([action isEqualToString:@"None"])
        return;
    if ([action isEqualToString:@"Minimize"]) {
        if (window.styleMask & NSWindowStyleMaskMiniaturizable)
            [window miniaturize:nil];
        return;
    }
    if (!(window.styleMask & NSWindowStyleMaskResizable))
        return;
    /* "Fill" tiles the window to the screen. Only a private method does it. */
    SEL fill = NSSelectorFromString(@"_zoomFill:");
    if ([action isEqualToString:@"Fill"] && [window respondsToSelector:fill])
        [window performSelector:fill withObject:nil];
    else
        [window zoom:nil];
}

static unsigned cocoa_mods(NSEventModifierFlags m)
{
    unsigned mods = 0;
    if (m & NSEventModifierFlagShift) mods |= YASL_PRIVATE_MOD_SHIFT;
    if (m & NSEventModifierFlagOption) mods |= YASL_PRIVATE_MOD_ALT;
    if (m & NSEventModifierFlagControl) mods |= YASL_PRIVATE_MOD_CTRL;
    if (m & NSEventModifierFlagCommand) mods |= YASL_PRIVATE_MOD_SUPER;
    if (m & NSEventModifierFlagCapsLock) mods |= YASL_PRIVATE_MOD_CAPS;
    return mods;
}

/* Decode a single Unicode scalar, never a lone UTF-16 surrogate. Layout labels
 * are optional: multi-scalar mappings and native function-key characters are 0. */
static uint32_t cocoa_key_label(NSString *characters)
{
    if (!characters.length) return 0;
    unichar first = [characters characterAtIndex:0];
    if (characters.length == 1)
        return yasl_private_key_label(first);
    if (characters.length == 2 && first >= 0xd800 && first <= 0xdbff) {
        unichar second = [characters characterAtIndex:1];
        if (second >= 0xdc00 && second <= 0xdfff)
            return yasl_private_key_label(0x10000 + ((first - 0xd800) << 10) + second - 0xdc00);
    }
    return 0;
}

static struct yasl_private_key_event cocoa_key_event(NSEvent *ev, enum yasl_key_action action)
{
    struct yasl_private_key_event k = {
        .key = yasl_private_key_from_cocoa(ev.keyCode),
        .action = action,
        .mods = cocoa_mods(ev.modifierFlags),
        .native_code = ev.keyCode,
        .text_input = YASL_KEY_TEXT_NONE,
    };
    /* The characters properties are valid only on key down and key up events. */
    if (ev.type == NSEventTypeKeyDown || ev.type == NSEventTypeKeyUp) {
        k.codepoint = cocoa_key_label([ev charactersByApplyingModifiers:0]);
        k.shifted = cocoa_key_label([ev charactersByApplyingModifiers:NSEventModifierFlagShift]);
    }
    return k;
}

static void on_key_event(struct cocoa_surface *w, NSEvent *ev, enum yasl_key_action action)
{
    w->mods = cocoa_mods(ev.modifierFlags);
    struct yasl_private_key_event k = cocoa_key_event(ev, action);
    if (action == YASL_KEY_RELEASE && ev.keyCode < 256) {
        if (w->key_text_input[ev.keyCode] != YASL_KEY_TEXT_UNSPECIFIED)
            k.text_input = w->key_text_input[ev.keyCode];
        k.consumed_mods = w->key_consumed_mods[ev.keyCode];
        w->key_text_input[ev.keyCode] = YASL_KEY_TEXT_NONE;
        w->key_consumed_mods[ev.keyCode] = 0;
    }
    struct yasl_private_event input_ev = {
        .type = YASL_PRIVATE_EVENT_KEY,
        .surface = &w->base,
        .key = k,
    };
    input_queue_push(&w->input, &input_ev);
}

static bool modifier_down(NSEvent *ev)
{
    NSEventModifierFlags f = ev.modifierFlags;
    switch (ev.keyCode) {
    case kVK_Shift: return f & DEVICE_LSHIFT;
    case kVK_RightShift: return f & DEVICE_RSHIFT;
    case kVK_Control: return f & DEVICE_LCTL;
    case kVK_RightControl: return f & DEVICE_RCTL;
    case kVK_Option: return f & DEVICE_LALT;
    case kVK_RightOption: return f & DEVICE_RALT;
    case kVK_Command: return f & DEVICE_LCMD;
    case kVK_RightCommand: return f & DEVICE_RCMD;
    case kVK_CapsLock: return f & NSEventModifierFlagCapsLock;
    default: return false;
    }
}

static int cocoa_button(NSInteger number)
{
    switch (number) {
    case 0: return YASL_PRIVATE_BUTTON_PRIMARY;
    case 1: return YASL_PRIVATE_BUTTON_SECONDARY;
    case 2: return YASL_PRIVATE_BUTTON_MIDDLE;
    case 3: return YASL_PRIVATE_BUTTON_BACK;
    case 4: return YASL_PRIVATE_BUTTON_FORWARD;
    default: return -1;
    }
}

static struct yasl_private_vector2i event_pos(struct cocoa_surface *w, NSEvent *ev)
{
    NSPoint p = [w->view convertPoint:ev.locationInWindow fromView:nil];
    CGFloat scale = surface_scale(w);
    return (struct yasl_private_vector2i){
        .x = (int)floor(p.x * scale),
        .y = (int)floor((w->view.bounds.size.height - p.y) * scale),
    };
}

/* The device movement of a locked pointer event, in device pixels. AppKit gives
 * the deltas in points with y up; the surface y grows down. */
static struct yasl_private_vector2d locked_delta(struct cocoa_surface *w, NSEvent *ev)
{
    CGFloat scale = surface_scale(w);
    return (struct yasl_private_vector2d){ .x = ev.deltaX * scale, .y = -ev.deltaY * scale };
}

/* The source of a scroll. A precise device scrolls with a finger; a wheel
 * reports notches. */
static unsigned scroll_source(NSEvent *event, bool precise)
{
    return precise || event.momentumPhase != NSEventPhaseNone ? SCROLL_SOURCE_FINGER
                                                              : SCROLL_SOURCE_WHEEL;
}

/* The phase of a scroll gesture. Momentum is the tail of a finger scroll, and a
 * cancelled gesture ends, because the events have no cancel phase. */
static unsigned scroll_phase(NSEventPhase phase, NSEventPhase momentum)
{
    if (phase == NSEventPhaseNone)
        phase = momentum;
    switch (phase) {
    case NSEventPhaseBegan: return SCROLL_PHASE_BEGIN;
    case NSEventPhaseChanged: return SCROLL_PHASE_UPDATE;
    case NSEventPhaseEnded: return SCROLL_PHASE_END;
    case NSEventPhaseCancelled: return SCROLL_PHASE_END;
    default: return 0;
    }
}

static void update_drag_hover(struct cocoa_display *d, NSPoint screenPoint)
{
    if (!d || !d->drag.active) return;

    NSInteger excludeNum = 0;
    if (d->drag.carried && d->drag.carried->window)
        excludeNum = d->drag.carried->window.windowNumber;
    else if (d->drag.image_window)
        excludeNum = d->drag.image_window.windowNumber;

    NSInteger winNum = [NSWindow windowNumberAtPoint:screenPoint belowWindowWithWindowNumber:excludeNum];
    struct cocoa_surface *target = NULL;
    struct yasl_private_vector2i pos = {0};

    if (winNum > 0) {
        NSWindow *nswin = [NSApp windowWithWindowNumber:winNum];
        if (nswin && [nswin isKindOfClass:[YaslWindow class]]) {
            struct cocoa_surface *candidate = ((YaslWindow *)nswin).surface;
            if (candidate && candidate != d->drag.carried && !candidate->closed && candidate->view) {
                NSPoint winPoint = [candidate->window convertPointFromScreen:screenPoint];
                NSPoint viewPoint = [candidate->view convertPoint:winPoint fromView:nil];
                if (NSPointInRect(viewPoint, candidate->view.bounds)) {
                    CGFloat scale = surface_scale(candidate);
                    pos.x = (int)floor(viewPoint.x * scale);
                    pos.y = (int)floor((candidate->view.bounds.size.height - viewPoint.y) * scale);
                    target = candidate;
                }
            }
        }
        if (!target) {
            for (int i = 0; i < d->count; i++) {
                struct cocoa_surface *s = d->windows[i];
                if (s != d->drag.carried && !s->closed && s->window && s->window.windowNumber == winNum && s->view) {
                    NSPoint winPoint = [s->window convertPointFromScreen:screenPoint];
                    NSPoint viewPoint = [s->view convertPoint:winPoint fromView:nil];
                    if (NSPointInRect(viewPoint, s->view.bounds)) {
                        CGFloat scale = surface_scale(s);
                        pos.x = (int)floor(viewPoint.x * scale);
                        pos.y = (int)floor((s->view.bounds.size.height - viewPoint.y) * scale);
                        target = s;
                    }
                    break;
                }
            }
        }
    }

    if (target != d->drag.hover) {
        if (d->drag.hover) {
            struct cocoa_surface *old = d->drag.hover;
            struct yasl_private_vector2i old_pos = d->drag.hover_pos;
            d->drag.hover = NULL;
            if (!old->destroyed) {
                darwin_ctx.depth++;
                yasl_private_surface_notify_drag(&old->base, YASL_PRIVATE_DRAG_LEAVE, old_pos);
                darwin_ctx.depth--;
            }
            if (!d->drag.active) return;
        }
        if (target && target->destroyed) target = NULL;
        d->drag.hover = target;
        d->drag.hover_pos = pos;
        if (target && !target->destroyed) {
            darwin_ctx.depth++;
            yasl_private_surface_notify_drag(&target->base, YASL_PRIVATE_DRAG_OVER, pos);
            darwin_ctx.depth--;
        }
    } else if (target && !target->destroyed) {
        d->drag.hover_pos = pos;
        darwin_ctx.depth++;
        yasl_private_surface_notify_drag(&target->base, YASL_PRIVATE_DRAG_OVER, pos);
        darwin_ctx.depth--;
    }
}

static uint64_t event_integer(NSEvent *event, const char *name)
{
    SEL selector = sel_registerName(name);
    if (![event respondsToSelector:selector]) return 0;
    return (uint64_t)((NSInteger (*)(id, SEL))objc_msgSend)(event, selector);
}

static double event_double(NSEvent *event, const char *name)
{
    SEL selector = sel_registerName(name);
    if (![event respondsToSelector:selector]) return 0;
    return ((double (*)(id, SEL))objc_msgSend)(event, selector);
}

static NSEventSubtype tablet_subtype(NSEvent *ev)
{
    if (!ev)
        return NSEventSubtypeMouseEvent;
    switch (ev.type) {
    case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp:
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
        return ev.subtype;
    default:
        return NSEventSubtypeMouseEvent;
    }
}

static void push_pen(struct cocoa_surface *w, NSEvent *event, unsigned action, bool contact)
{
    if (!w || w->closed || w->tablet_device == YASL_PRIVATE_PTR_DEV_MOUSE) return;
    struct yasl_private_pen_sample pen = {
        .device_id = w->pen_device_id,
        .tool_id = w->pen_tool_id,
        .x = w->pointer.x,
        .y = w->pointer.y,
        .tool = w->tablet_device == YASL_PRIVATE_PTR_DEV_ERASER ? 2u : 1u,
        .action = action,
        .in_contact = contact,
    };
    if (event) {
        if (w->view && [event respondsToSelector:@selector(locationInWindow)]) {
            NSPoint point = [w->view convertPoint:event.locationInWindow fromView:nil];
            CGFloat scale = surface_scale(w);
            pen.x = point.x * scale;
            pen.y = (w->view.bounds.size.height - point.y) * scale;
        }
        if (event.type == NSEventTypeTabletPoint || tablet_subtype(event) == NSEventSubtypeTabletPoint) {
            if (event.buttonMask & NSEventButtonMaskPenTip)
                pen.buttons |= YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY);
            if (event.buttonMask & NSEventButtonMaskPenLowerSide)
                pen.buttons |= YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_SECONDARY);
            if (event.buttonMask & NSEventButtonMaskPenUpperSide)
                pen.buttons |= YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_MIDDLE);
            pen.pressure = fmax(0.0, fmin(1.0, event.pressure));
            pen.axes_valid |= YASL_PRIVATE_PEN_PRESSURE;
            pen.axes_changed |= YASL_PRIVATE_PEN_PRESSURE;
            NSPoint tilt = event.tilt;
            pen.tilt_x = fmax(-90.0, fmin(90.0, tilt.x * 90.0));
            pen.tilt_y = fmax(-90.0, fmin(90.0, -tilt.y * 90.0));
            pen.axes_valid |= YASL_PRIVATE_PEN_TILT;
            pen.axes_changed |= YASL_PRIVATE_PEN_TILT;
            if ([event respondsToSelector:@selector(rotation)]) {
                pen.rotation = event_double(event, "rotation");
                pen.axes_valid |= YASL_PRIVATE_PEN_ROTATION;
                pen.axes_changed |= YASL_PRIVATE_PEN_ROTATION;
            }
            if ([event respondsToSelector:@selector(tangentialPressure)]) {
                pen.tangential = event_double(event, "tangentialPressure");
                pen.axes_valid |= YASL_PRIVATE_PEN_TANGENTIAL;
                pen.axes_changed |= YASL_PRIVATE_PEN_TANGENTIAL;
            }
        }
        uint64_t device = event_integer(event, "pointingDeviceID");
        uint64_t tool = event_integer(event, "pointingDeviceSerialNumber");
        if (device) pen.device_id = device;
        if (tool) pen.tool_id = tool;
    }
    if (action == YASL_PRIVATE_PEN_DOWN) {
        pen.buttons |= YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY);
        pen.changed_button = YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY);
    } else if (action == YASL_PRIVATE_PEN_UP) {
        pen.buttons &= ~YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY);
        pen.changed_button = YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY);
    }
    if (action == YASL_PRIVATE_PEN_LEAVE || action == YASL_PRIVATE_PEN_CANCEL) {
        pen.buttons = 0;
        pen.in_contact = false;
        pen.pressure = 0;
        w->pen_buttons = 0;
    } else {
        uint32_t changed = pen.buttons ^ w->pen_buttons;
        if (action == YASL_PRIVATE_PEN_MOVE && changed) {
            action = YASL_PRIVATE_PEN_BUTTON;
            pen.changed_button = changed & (~changed + 1u);
        }
        w->pen_buttons = pen.buttons;
    }
    pen.action = action;
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_PEN,
        .surface = &w->base,
        .pen = pen,
    };
    input_queue_push(&w->input, &ev);
}

static void push_pointer(struct cocoa_surface *w, NSEvent *ev, enum yasl_private_event_type type,
                         int button)
{
    if (!w || w->closed)
        return;
    if ((type == YASL_PRIVATE_EVENT_POINTER_DOWN || type == YASL_PRIVATE_EVENT_POINTER_UP) &&
        (button < 0 || button > 4))
        return;
    bool proximity = ev.type == NSEventTypeTabletProximity ||
                     tablet_subtype(ev) == NSEventSubtypeTabletProximity;
    if (proximity) {
        if (ev.isEnteringProximity) {
            w->tablet_device = ev.pointingDeviceType == NSPointingDeviceTypeEraser ?
                YASL_PRIVATE_PTR_DEV_ERASER : ev.pointingDeviceType == NSPointingDeviceTypePen ?
                YASL_PRIVATE_PTR_DEV_PEN : YASL_PRIVATE_PTR_DEV_MOUSE;
            w->pen_device_id = event_integer(ev, "pointingDeviceID");
            w->pen_tool_id = event_integer(ev, "pointingDeviceSerialNumber");
            if (w->tablet_device != YASL_PRIVATE_PTR_DEV_MOUSE)
                push_pen(w, ev, YASL_PRIVATE_PEN_ENTER, false);
            return;
        }
        type = YASL_PRIVATE_EVENT_POINTER_LEAVE;
    }
    w->mods = cocoa_mods(ev.modifierFlags);
    if (type != YASL_PRIVATE_EVENT_POINTER_LEAVE && !w->pointer_locked)
        w->pointer = event_pos(w, ev);

    enum yasl_private_pointer_device device = YASL_PRIVATE_PTR_DEV_MOUSE;
    struct yasl_private_pointer_pen pen = {0};

    if (type == YASL_PRIVATE_EVENT_POINTER_LEAVE && w->tablet_device != YASL_PRIVATE_PTR_DEV_MOUSE) {
        device = w->tablet_device;
        w->tablet_down = false;
    } else if (ev.type == NSEventTypeTabletPoint || tablet_subtype(ev) == NSEventSubtypeTabletPoint) {
        /* pointingDeviceType belongs to proximity events. Remember that
         * event's tool so erasers and zero-pressure hover stay classified. */
        device = w->tablet_device == YASL_PRIVATE_PTR_DEV_ERASER ? YASL_PRIVATE_PTR_DEV_ERASER : YASL_PRIVATE_PTR_DEV_PEN;
        float press = (float)ev.pressure;
        if (press > 1.0f) press = 1.0f;
        if (press > 0.0f)
            pen.pressure = (uint16_t)(press * 65535.0f);

        NSPoint tilt = ev.tilt;
        pen.tilt_x = (int8_t)lround(fmax(-1, fmin(1, tilt.x)) * 90.0);
        pen.tilt_y = (int8_t)lround(fmax(-1, fmin(1, -tilt.y)) * 90.0);
        if (button == 0 && type == YASL_PRIVATE_EVENT_POINTER_DOWN) w->tablet_down = true;
        if (button == 0 && type == YASL_PRIVATE_EVENT_POINTER_UP) {
            w->tablet_down = false;
            pen.pressure = 0;
        }
    }

    if (type == YASL_PRIVATE_EVENT_POINTER_DOWN && button >= 0 && button < 32)
        w->buttons |= (1u << button);
    else if (type == YASL_PRIVATE_EVENT_POINTER_UP && button >= 0 && button < 32)
        w->buttons &= ~(1u << button);

    struct yasl_private_event input_ev = {
        .type = type,
        .surface = &w->base,
        .pointer = {
            .pos = w->pointer,
            .button = (type == YASL_PRIVATE_EVENT_POINTER_DOWN || type == YASL_PRIVATE_EVENT_POINTER_UP) ?
                      (enum yasl_private_pointer_button)button : YASL_PRIVATE_BUTTON_PRIMARY,
            .device = device,
            .mods = w->mods,
            .pen = pen,
        },
    };
    if (w->pointer_locked && type == YASL_PRIVATE_EVENT_POINTER_MOTION)
        input_ev.pointer.delta = locked_delta(w, ev);
    input_queue_push(&w->input, &input_ev);
    if (type == YASL_PRIVATE_EVENT_POINTER_LEAVE && device != YASL_PRIVATE_PTR_DEV_MOUSE) {
        push_pen(w, ev, YASL_PRIVATE_PEN_LEAVE, false);
    } else if (ev.type == NSEventTypeTabletPoint || tablet_subtype(ev) == NSEventSubtypeTabletPoint) {
        unsigned action = button == YASL_PRIVATE_BUTTON_PRIMARY && type == YASL_PRIVATE_EVENT_POINTER_DOWN ? YASL_PRIVATE_PEN_DOWN :
            button == YASL_PRIVATE_BUTTON_PRIMARY && type == YASL_PRIVATE_EVENT_POINTER_UP ? YASL_PRIVATE_PEN_UP :
            YASL_PRIVATE_PEN_MOVE;
        push_pen(w, ev, action, w->tablet_down);
    }
    if (type == YASL_PRIVATE_EVENT_POINTER_MOTION && darwin_ctx.drag.active) {
        if (darwin_ctx.drag.carried) {
            place_under_pointer(darwin_ctx.drag.carried, darwin_ctx.drag.carry_offset);
        } else if (darwin_ctx.drag.image_window) {
            NSWindow *window = darwin_ctx.drag.image_window;
            NSPoint mouse = [NSEvent mouseLocation];
            NSPoint hotspot = darwin_ctx.drag.image_hotspot;
            [window setFrameOrigin:NSMakePoint(mouse.x - hotspot.x,
                mouse.y - NSHeight(window.frame) + hotspot.y)];
        }
        update_drag_hover(&darwin_ctx, [NSEvent mouseLocation]);
    } else if (type == YASL_PRIVATE_EVENT_POINTER_UP && darwin_ctx.drag.active) {
        update_drag_hover(&darwin_ctx, [NSEvent mouseLocation]);
        drag_end(&darwin_ctx, YASL_PRIVATE_DRAG_DROPPED);
    }
    if (proximity) {
        w->tablet_device = YASL_PRIVATE_PTR_DEV_MOUSE;
        w->tablet_down = false;
        w->pen_device_id = w->pen_tool_id = 0;
    }
}

/* macOS has no public diagonal resize cursor. It has the two diagonal arrows as
 * template symbols, so the cursor starts from those. */
static NSCursor *cocoa_symbol_cursor(NSString *name, NSCursor **cache)
{
    if (*cache)
        return *cache;
    NSImage *image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (!image)
        return [NSCursor arrowCursor];
    image.template = YES;
    *cache = [[NSCursor alloc] initWithImage:image
                                     hotSpot:NSMakePoint(image.size.width / 2,
                                                         image.size.height / 2)];
    return *cache ?: [NSCursor arrowCursor];
}

static NSCursor *cocoa_cursor(enum yasl_private_cursor cursor)
{
    switch (cursor) {
    case YASL_PRIVATE_CURSOR_TEXT: return [NSCursor IBeamCursor];
    case YASL_PRIVATE_CURSOR_LINK: return [NSCursor pointingHandCursor];
    case YASL_PRIVATE_CURSOR_CROSSHAIR: return [NSCursor crosshairCursor];
    case YASL_PRIVATE_CURSOR_MOVE: return [NSCursor openHandCursor];
    case YASL_PRIVATE_CURSOR_RESIZE_NWSE:
        return cocoa_symbol_cursor(@"arrow.up.left.and.arrow.down.right",
                                  &darwin_ctx.diagonal_cursors[0]);
    case YASL_PRIVATE_CURSOR_RESIZE_NESW:
        return cocoa_symbol_cursor(@"arrow.up.right.and.arrow.down.left",
                                  &darwin_ctx.diagonal_cursors[1]);
    case YASL_PRIVATE_CURSOR_RESIZE_EW: return [NSCursor resizeLeftRightCursor];
    case YASL_PRIVATE_CURSOR_RESIZE_NS: return [NSCursor resizeUpDownCursor];
    case YASL_PRIVATE_CURSOR_HIDDEN:
        if (!darwin_ctx.hidden_cursor) {
            NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
            [image lockFocus];
            [[NSColor clearColor] set];
            NSRectFillUsingOperation(NSMakeRect(0, 0, 1, 1), NSCompositingOperationCopy);
            [image unlockFocus];
            darwin_ctx.hidden_cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
            [image release];
        }
        return darwin_ctx.hidden_cursor;
    case YASL_PRIVATE_CURSOR_ARROW:
    default: return [NSCursor arrowCursor];
    }
}

/* Shows the cursor that the surface selects now. A locked surface hides it,
 * because its position no longer follows the mouse. */
static void apply_cursor(struct cocoa_surface *w)
{
    if (!w || w->closed || w->destroyed || !w->window)
        return;
    [cocoa_cursor(w->pointer_locked ? YASL_PRIVATE_CURSOR_HIDDEN : w->cursor) set];
}

/* A locked surface hides the cursor and disconnects it from the mouse, so the
 * cursor stays in place. Motion then reports raw device movement. The lock
 * holds only while the surface has key focus, as it does on Wayland. */
static void apply_pointer_lock(struct cocoa_surface *w)
{
    if (!w || w->closed || w->destroyed || !w->window)
        return;
    bool locked = w->pointer_locked && w->window.isKeyWindow;
    CGAssociateMouseAndMouseCursorPosition(locked ? 0 : 1);
    if (!locked && !w->window.isKeyWindow)
        return;
    apply_cursor(w);
}

@implementation YaslAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    yasl_private_display_notify_event(&darwin_ctx.base, YASL_PRIVATE_EVENT_DISPLAY_QUIT);
    return NSTerminateCancel;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible
{
    (void)sender;
    if (!visible)
        yasl_private_display_notify_event(&darwin_ctx.base, YASL_PRIVATE_EVENT_DISPLAY_REOPEN);
    return YES;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return NO;
}
/* A display mode change alters the refresh rate of a screen. Wake the link so
 * it measures the new cadence, and apply the size that the change made. A
 * frame that already waits still waits for its tick. */
- (void)applicationDidChangeScreenParameters:(NSNotification *)notification
{
    (void)notification;
    trace_move_screens();
    for (int i = 0; i < darwin_ctx.count; i++) {
        struct cocoa_surface *w = darwin_ctx.windows[i];
        if (w->destroyed || w->closed)
            continue;
        w->idle_frames = 0;
        if (w->link.paused)
            w->link.paused = NO;
        sync_size(w, false);
    }
}
@end

static struct yasl_private_vector2i dragging_pos(YaslView *view, id<NSDraggingInfo> sender)
{
    NSPoint p = [view convertPoint:[sender draggingLocation] fromView:nil];
    CGFloat scale = surface_scale(view.surface);
    return (struct yasl_private_vector2i){
        .x = (int)floor(p.x * scale),
        .y = (int)floor((view.bounds.size.height - p.y) * scale),
    };
}

/* A drag names its data with a pasteboard type, which is a UTI, and the drop
 * types of a surface name MIME types. This function gives the MIME name of the
 * pasteboard types that a window takes. A type that is absent has no MIME name,
 * so a drop can never select it.
 *
 * The pasteboard type objects belong to the system framework, and an element of
 * a static initializer must be a compile-time constant. A static table cannot
 * hold them, so this function compares the type against each constant name. It
 * returns a constant string, so the bytes outlive the call. */
static NSString *drop_mime_of_type(NSString *type)
{
    if ([type isEqualToString:NSPasteboardTypeFileURL]) return @"text/uri-list";
    if ([type isEqualToString:NSPasteboardTypeURL]) return @"text/uri-list";
    if ([type isEqualToString:NSPasteboardTypeString]) return @"text/plain";
    /* Some sources name UTF-8 text with its MIME type. */
    if ([type isEqualToString:@"text/plain;charset=utf-8"]) return @"text/plain";
    if ([type isEqualToString:NSPasteboardTypeHTML]) return @"text/html";
    if ([type isEqualToString:NSPasteboardTypeRTF]) return @"text/rtf";
    if ([type isEqualToString:NSPasteboardTypePNG]) return @"image/png";
    if ([type isEqualToString:NSPasteboardTypeTIFF]) return @"image/tiff";
    if ([type isEqualToString:NSPasteboardTypePDF]) return @"application/pdf";
    return nil;
}

/* The pasteboard types that a drop can offer. A surface only takes the ones
 * that its drop types select. */
static NSArray<NSPasteboardType> *drop_pasteboard_types(void)
{
    return @[NSPasteboardTypeFileURL, NSPasteboardTypeURL, NSPasteboardTypeString,
             @"text/plain;charset=utf-8",
             NSPasteboardTypeHTML, NSPasteboardTypeRTF, NSPasteboardTypePNG,
             NSPasteboardTypeTIFF, NSPasteboardTypePDF];
}

/* The pasteboard type that a drop on the view reads and its MIME name. Returns
 * false when the drop types of the surface refuse the drag. */
static bool drop_pick(struct cocoa_surface *w, NSPasteboard *pb, NSString **type_out,
                      char mime_out[YASL_PRIVATE_DROP_MIME_MAX])
{
    enum { DROP_TYPES_MAX = 32 };
    NSArray<NSPasteboardType> *types = pb.types;
    const char *offered[DROP_TYPES_MAX];
    size_t count = 0;
    NSString *uri_list = nil;
    for (NSUInteger i = 0; i < types.count && count < DROP_TYPES_MAX; i++) {
        NSString *mime = drop_mime_of_type(types[i]);
        if (!mime)
            continue;
        if (!uri_list && [mime isEqualToString:@"text/uri-list"])
            uri_list = types[i];
        /* The MIME name is a constant, so its bytes outlive this call. */
        offered[count++] = mime.UTF8String;
    }
    /* A source can offer file URLs through a type that has no MIME name. */
    if (!uri_list && [pb canReadItemWithDataConformingToTypes:@[NSPasteboardTypeFileURL]]) {
        uri_list = NSPasteboardTypeFileURL;
        if (count < DROP_TYPES_MAX)
            offered[count++] = "text/uri-list";
    }
    if (!count)
        return false;
    struct cocoa_surface *root = w;
    while (root->parent)
        root = root->parent;
    const char *picked = yasl_private_surface_pick_drop_type(&root->base, offered, count);
    if (!picked)
        return false;
    NSString *type = uri_list && !strcmp(picked, "text/uri-list") ? uri_list : nil;
    for (NSUInteger i = 0; !type && i < types.count; i++) {
        NSString *mime = drop_mime_of_type(types[i]);
        if (mime && !strcmp(mime.UTF8String, picked))
            type = types[i];
    }
    if (!type)
        return false;
    *type_out = type;
    snprintf(mime_out, YASL_PRIVATE_DROP_MIME_MAX, "%s", picked);
    return true;
}

/* Delivers the data of a picked drop type as a DROP event. A URI list arrives
 * as CRLF-separated file URLs; any other type arrives as its bytes. The event
 * carries the surface-relative device-pixel position of the release. */
static bool push_drop(struct cocoa_surface *w, NSPasteboard *pb, NSString *type,
                      const char *mime, struct yasl_private_vector2i pos)
{
    NSData *data = nil;
    if (!strcmp(mime, "text/uri-list")) {
        NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]]
            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
        NSMutableString *list = [NSMutableString string];
        for (NSURL *url in urls) {
            [list appendString:url.absoluteString];
            [list appendString:@"\r\n"];
        }
        data = [list dataUsingEncoding:NSUTF8StringEncoding];
    } else {
        data = [pb dataForType:type];
        /* A source can hold text only as a string value, not as data. */
        if (!data.length) {
            NSString *text = [pb stringForType:type];
            if (text)
                data = [text dataUsingEncoding:NSUTF8StringEncoding];
        }
    }
    if (!data.length)
        return false;
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_DROP,
        .surface = &w->base,
        .text = { .data = data.bytes, .len = data.length, .pos = pos },
    };
    snprintf(ev.text.mime, sizeof(ev.text.mime), "%s", mime);
    bool pushed = input_queue_push(&w->input, &ev);
    if (pushed)
        cocoa_surface_schedule_redraw(w);
    return pushed;
}

@implementation YaslView

- (instancetype)initWithFrame:(NSRect)frameRect cpuLayer:(BOOL)cpuLayer
{
    self = [super initWithFrame:frameRect];
    if (self) {
        self.cpuLayer = cpuLayer;
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        self.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
        [self registerForDraggedTypes:drop_pasteboard_types()];
    }
    return self;
}

- (instancetype)initWithFrame:(NSRect)frameRect
{
    return [self initWithFrame:frameRect cpuLayer:NO];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    struct cocoa_surface *w = self.surface;
    NSString *type = nil;
    char mime[YASL_PRIVATE_DROP_MIME_MAX];
    if (!w || w->closed || !drop_pick(w, [sender draggingPasteboard], &type, mime))
        return NSDragOperationNone;
    struct yasl_private_vector2i pos = dragging_pos(self, sender);
    darwin_ctx.depth++;
    yasl_private_surface_notify_drop_mime(&w->base, YASL_PRIVATE_DROP_OVER, pos, mime);
    darwin_ctx.depth--;
    return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    return [self draggingEntered:sender];
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
    struct cocoa_surface *w = self.surface;
    NSString *type = nil;
    char mime[YASL_PRIVATE_DROP_MIME_MAX];
    if (!w || w->closed || !drop_pick(w, [sender draggingPasteboard], &type, mime))
        return;
    struct yasl_private_vector2i pos = dragging_pos(self, sender);
    darwin_ctx.depth++;
    yasl_private_surface_notify_drop(&w->base, YASL_PRIVATE_DROP_LEAVE, pos);
    darwin_ctx.depth--;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    struct cocoa_surface *w = self.surface;
    if (!w) return NO;
    struct yasl_private_vector2i pos = dragging_pos(self, sender);
    NSPasteboard *pb = [sender draggingPasteboard];
    NSString *type = nil;
    char mime[YASL_PRIVATE_DROP_MIME_MAX];
    BOOL ok = drop_pick(w, pb, &type, mime) && push_drop(w, pb, type, mime, pos);
    if (!w->destroyed) {
        darwin_ctx.depth++;
        yasl_private_surface_notify_drop(&w->base, YASL_PRIVATE_DROP_LEAVE, pos);
        darwin_ctx.depth--;
    }
    return ok;
}

- (CALayer *)makeBackingLayer
{
    if (self.cpuLayer) {
        CALayer *layer = [CALayer layer];
        layer.opaque = YES;
        layer.contentsGravity = kCAGravityTopLeft;
        return layer;
    }
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.allowsNextDrawableTimeout = YES;
    layer.opaque = YES;
    layer.contentsGravity = kCAGravityTopLeft;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    return layer;
}

- (BOOL)isOpaque { return !self.surface || !self.surface->is_popup; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }

- (void)updateLayer
{
    struct cocoa_surface *w = self.surface;
    if (w && !w->closed && w->base.presenter != YASL_PRIVATE_PRESENTER_CPU)
        yasl_private_surface_notify_refresh(&w->base);
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    sync_size(self.surface, false);
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    trace_window_move(self.surface, "backing-change", NULL);
    sync_size(self.surface, false);
}

- (void)viewWillStartLiveResize
{
    [super viewWillStartLiveResize];
    struct cocoa_surface *w = self.surface;
    /* Layers that the app owns through Metal or Vulkan keep their own setting. */
    if (w && w->base.presenter == YASL_PRIVATE_PRESENTER_CPU && w->metal_layer)
        w->metal_layer.presentsWithTransaction = YES;
}

- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    struct cocoa_surface *w = self.surface;
    if (!w)
        return;
    if (w->base.presenter == YASL_PRIVATE_PRESENTER_CPU && w->metal_layer)
        w->metal_layer.presentsWithTransaction = NO;
    cocoa_surface_schedule_redraw(w);
}

- (void)displayLinkFired:(CADisplayLink *)link
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->closed)
        return;
    double period = link ? yasl_private_clock_link_period(link.timestamp, link.duration,
                                                         link.targetTimestamp)
                         : 0.0;
    if (period > 0.0)
        w->clock.refresh_ns = period * 1e9;
    w->frame_due = true;
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        /* The application draws this surface. One tick answers one coalesced
         * frame intent, so a sustained draw follows the display. */
        if (!w->want_draw || (w->base.state & YASL_PRIVATE_STATE_SUSPENDED)) {
            if (++w->idle_frames >= LINK_IDLE_FRAMES)
                link.paused = YES;
            return;
        }
        w->idle_frames = 0;
        w->want_draw = false;
        cancel_tick_timer(w);
        yasl_private_surface_notify_frame_shown(&w->base);
        if (darwin_ctx.loop)
            yasl_private_loop_kick(darwin_ctx.loop);
        return;
    }
    if (!w->want_draw || (w->base.state & YASL_PRIVATE_STATE_SUSPENDED)) {
        if (++w->idle_frames >= LINK_IDLE_FRAMES) {
            link.paused = YES;
            schedule_cpu_trim(w);
        }
        return;
    }
    w->idle_frames = 0;
    if (darwin_ctx.loop)
        yasl_private_loop_kick(darwin_ctx.loop);
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (self.trackingAreas.count > 0)
        return;
    NSTrackingArea *area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingCursorUpdate | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:area];
    [area release];
}

- (void)cursorUpdate:(NSEvent *)event
{
    (void)event;
    struct cocoa_surface *w = self.surface;
    if (w)
        apply_cursor(w);
    else
        [cocoa_cursor(YASL_PRIVATE_CURSOR_ARROW) set];
}

- (void)keyDown:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (!w) return;
    darwin_ctx.depth++;
    w->mods = cocoa_mods(event.modifierFlags);
    struct yasl_private_key_event key = cocoa_key_event(event,
        event.isARepeat ? YASL_KEY_REPEAT : YASL_KEY_PRESS);
    w->interpreting_key = true;
    w->key_composing = w->preedit != NULL;
    w->key_insertions = 0;
    if (++w->text_key_id == 0) ++w->text_key_id;
    [self interpretKeyEvents:@[event]];
    w->interpreting_key = false;
    if (!w->destroyed && !w->closed) {
        if (w->key_composing || w->key_insertions > 1) {
            key.text_input = YASL_KEY_TEXT_COMPOSITION;
        } else if (w->key_insertions == 1) {
            key.text_input = YASL_KEY_TEXT_COMMIT;
            key.text_id = w->text_key_id;
            key.consumed_mods = key.mods &
                (YASL_PRIVATE_MOD_ALT | YASL_PRIVATE_MOD_CTRL | YASL_PRIVATE_MOD_SUPER);
        }
        if (event.keyCode < 256) {
            w->key_text_input[event.keyCode] = key.text_input;
            w->key_consumed_mods[event.keyCode] = key.consumed_mods;
        }
        input_queue_push(&w->input, &(struct yasl_private_event){
            .type = YASL_PRIVATE_EVENT_KEY, .surface = &w->base, .key = key,
        });
        struct yasl_private_event edit;
        while (input_queue_pop(&w->text_events, &edit)) {
            if (!key.text_id) {
                if (edit.type == YASL_PRIVATE_EVENT_TEXT)
                    edit.text.text_key_id = 0;
                else if (edit.type == YASL_PRIVATE_EVENT_TEXT_EDIT)
                    edit.text_edit.text_key_id = 0;
            }
            input_queue_push(&w->input, &edit);
        }
    }
    darwin_ctx.depth--;
}

- (void)keyUp:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (w)
        on_key_event(w, event, YASL_KEY_RELEASE);
}

- (void)flagsChanged:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (!w) return;
    w->mods = cocoa_mods(event.modifierFlags);
    on_key_event(w, event, modifier_down(event) ? YASL_KEY_PRESS : YASL_KEY_RELEASE);
}

/* The view gets each press, also in the title bar band of an overlay window.
 * The window server must not start a drag from the view by itself. If it
 * does, the drag that YASL starts for the same press loses its move start,
 * and AppKit then refits the frame at each move. AppKit asks the private
 * method _opaqueRectForWindowMoveWhenInTitlebar which parts of the band the
 * window server must leave to the view. */
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (NSRect)_opaqueRectForWindowMoveWhenInTitlebar { return self.bounds; }

/* A press in a title bar area moves an inactive window at once, as a press
 * on a system title bar does. */
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return is_titlebar_press(self.surface, event);
}

- (void)mouseDown:(NSEvent *)event
{
    [self.window makeFirstResponder:self];
    /* Right and other buttons also arrive here. Keep the press for interactive moves. */
    struct cocoa_surface *w = self.surface;
    if (w && event.type == NSEventTypeLeftMouseDown)
        w->titlebar_press = is_titlebar_press(w, event);
    if (w && w->titlebar_press) {
        /* Start the drag while AppKit dispatches the press. It is the only
         * drag, because the view keeps the window server out of the band. */
        if (event.clickCount == 2) {
            titlebar_double_click(w->window);
        } else if (w->window.movable) {
            trace_window_move(w, "titlebar-drag", NULL);
            [w->window performWindowDragWithEvent:event];
        }
        return;
    }
    if (w) {
        [event retain];
        [w->mouse_down release];
        w->mouse_down = event;
    }
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_DOWN, cocoa_button(event.buttonNumber));
}
- (void)rightMouseDown:(NSEvent *)event { [self mouseDown:event]; }
- (void)otherMouseDown:(NSEvent *)event { [self mouseDown:event]; }

- (void)mouseUp:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (w && w->titlebar_press && event.type == NSEventTypeLeftMouseUp) {
        w->titlebar_press = false;
        return;
    }
    if (w) {
        [w->mouse_down release];
        w->mouse_down = nil;
    }
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_UP, cocoa_button(event.buttonNumber));
}
- (void)rightMouseUp:(NSEvent *)event { [self mouseUp:event]; }
- (void)otherMouseUp:(NSEvent *)event { [self mouseUp:event]; }

- (void)mouseMoved:(NSEvent *)event
{
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_MOTION, 0);
}

- (void)mouseDragged:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (w && w->titlebar_press)
        return;
    push_pointer(w, event, YASL_PRIVATE_EVENT_POINTER_MOTION, 0);
}
- (void)rightMouseDragged:(NSEvent *)event
{
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_MOTION, 0);
}
- (void)otherMouseDragged:(NSEvent *)event
{
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_MOTION, 0);
}

- (void)mouseExited:(NSEvent *)event
{
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_LEAVE, 0);
}

- (void)tabletProximity:(NSEvent *)event
{
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_MOTION, 0);
}

- (void)tabletPoint:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (!w) return;
    bool down = (event.buttonMask & NSEventButtonMaskPenTip) != 0;
    enum yasl_private_event_type type = down == w->tablet_down ? YASL_PRIVATE_EVENT_POINTER_MOTION :
        down ? YASL_PRIVATE_EVENT_POINTER_DOWN : YASL_PRIVATE_EVENT_POINTER_UP;
    push_pointer(w, event, type, 0);
}

- (void)scrollWheel:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (!w) return;
    double dx = -event.scrollingDeltaX, dy = -event.scrollingDeltaY;
    bool precise = event.hasPreciseScrollingDeltas;
    CGFloat scale = surface_scale(w);
    /* A precise device reports pixels; every device reports line offsets. */
    struct yasl_private_vector2d pixels = {dx * scale, dy * scale};
    unsigned valid = YASL_PRIVATE_SCROLL_LINES;
    if (precise) {
        dx /= SCROLL_POINTS_PER_LINE;
        dy /= SCROLL_POINTS_PER_LINE;
        valid |= YASL_PRIVATE_SCROLL_PIXELS;
    } else {
        pixels = (struct yasl_private_vector2d){0, 0};
    }
    unsigned phase = scroll_phase(event.phase, event.momentumPhase);
    /* A gesture end can arrive with no delta, and it still ends the gesture. */
    if (!isfinite(dx) || !isfinite(dy) ||
        ((dx == 0 && dy == 0) && phase != SCROLL_PHASE_END)) return;
    w->mods = cocoa_mods(event.modifierFlags);
    w->pointer = event_pos(w, event);
    struct yasl_private_event input_ev = {
        .type = YASL_PRIVATE_EVENT_POINTER_SCROLL,
        .surface = &w->base,
        .pointer = {
            .pos = w->pointer,
            .mods = w->mods,
            .scroll = {dx, dy},
            .pixel_scroll = pixels,
            .scroll_valid = (uint8_t)valid,
            .scroll_source = (uint8_t)scroll_source(event, precise),
            .scroll_phase = (uint8_t)phase,
            .scroll_momentum = event.momentumPhase != NSEventPhaseNone,
        },
    };
    input_queue_push(&w->input, &input_ev);
}

static NSString *plain_string(id string)
{
    return [string isKindOfClass:[NSAttributedString class]]
        ? [(NSAttributedString *)string string] : (NSString *)string;
}

static bool text_utf16_to_utf8(NSString *text, NSUInteger index, size_t *byte_offset)
{
    if (!text || index > text.length) return false;
    const char *prefix = [[text substringToIndex:index] UTF8String];
    if (!prefix) return false;
    *byte_offset = strlen(prefix);
    return true;
}

static bool text_utf8_to_utf16(const char *text, size_t byte_offset, NSUInteger *index)
{
    size_t len = text ? strlen(text) : 0;
    if (byte_offset > len) return false;
    NSString *prefix = [[NSString alloc] initWithBytes:text length:byte_offset
                                              encoding:NSUTF8StringEncoding];
    if (!prefix) return false;
    *index = prefix.length;
    [prefix release];
    return true;
}

static bool text_edit_range(struct cocoa_surface *w, NSRange range, bool use_marked,
                            size_t *begin, size_t *end)
{
    if (range.location == NSNotFound) {
        if (use_marked && w->preedit) {
            if (w->preedit_revision != w->base.text_revision ||
                w->preedit_generation != w->base.text_generation)
                return false;
            *begin = w->preedit_begin;
            *end = w->preedit_end;
        } else {
            *begin = w->base.text_begin;
            *end = w->base.text_end;
        }
        return *begin <= *end && *end <= w->base.text_length;
    }
    NSString *surrounding = [NSString stringWithUTF8String:w->base.text_surrounding ?: ""];
    size_t a, b;
    if (!surrounding || range.location > surrounding.length ||
        range.length > surrounding.length - range.location ||
        !text_utf16_to_utf8(surrounding, range.location, &a) ||
        !text_utf16_to_utf8(surrounding, range.location + range.length, &b))
        return false;
    *begin = a;
    *end = b;
    return true;
}

static void push_text_edit(struct cocoa_surface *w, const char *text, size_t len,
                           size_t begin, size_t end, bool insertion)
{
    if (!w->base.text_input_active || begin > end || end > w->base.text_length) return;
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_TEXT_EDIT,
        .surface = &w->base,
        .text_edit = {
            .data = text ?: "", .len = len,
            .revision = w->base.text_revision,
            .generation = w->base.text_generation,
            .begin = begin, .end = end,
            .text_key_id = w->interpreting_key && insertion ? w->text_key_id : 0,
        },
    };
    input_queue_push(w->interpreting_key ? &w->text_events : &w->input, &ev);
}

static void push_composition(struct cocoa_surface *w, const char *text, int begin, int end)
{
    struct yasl_private_event event = {
        .type = YASL_PRIVATE_EVENT_COMPOSITION, .surface = &w->base,
        .composition = {
            .data = text ?: "", .len = text ? strlen(text) : 0,
            .begin = begin, .end = end, .active = text && text[0],
            .revision = w->base.text_revision, .generation = w->base.text_generation,
        },
    };
    input_queue_push(w->interpreting_key ? &w->text_events : &w->input, &event);
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->destroyed || w->closed) return;
    const char *utf8 = plain_string(string).UTF8String;
    size_t len = utf8 ? strlen(utf8) : 0;
    if (!w->base.text_input_active) {
        if (!w->interpreting_key) return;
        if (!len) {
            w->key_composing = true;
            return;
        }
        bool had_preedit = w->preedit != NULL;
        w->key_insertions++;
        input_queue_push(&w->text_events, &(struct yasl_private_event){
            .type = YASL_PRIVATE_EVENT_TEXT, .surface = &w->base,
            .text = { .data = utf8, .len = len, .text_key_id = w->text_key_id },
        });
        /* The commit ends the preedit of a dead key. */
        free(w->preedit);
        w->preedit = NULL;
        w->preedit_cursor_begin = w->preedit_cursor_end = -1;
        if (had_preedit)
            push_composition(w, NULL, -1, -1);
        return;
    }
    size_t begin, end;
    if (!text_edit_range(w, replacementRange, true, &begin, &end)) return;
    bool had_preedit = w->preedit != NULL;
    if (w->interpreting_key) {
        if (len) w->key_insertions++;
        else w->key_composing = true;
    }
    darwin_ctx.depth++;
    push_text_edit(w, utf8, len, begin, end, true);
    free(w->preedit);
    w->preedit = NULL;
    w->preedit_begin = w->preedit_end = 0;
    w->preedit_cursor_begin = w->preedit_cursor_end = -1;
    if (had_preedit)
        push_composition(w, NULL, -1, -1);
    if (!w->destroyed && !w->closed)
        cocoa_surface_schedule_redraw(w);
    darwin_ctx.depth--;
}

- (void)doCommandBySelector:(SEL)selector
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->destroyed || w->closed || !w->base.text_input_active) return;
    NSString *surrounding = [NSString stringWithUTF8String:w->base.text_surrounding ?: ""];
    NSUInteger begin, end;
    if (!sel_isEqual(selector, @selector(deleteBackward)) &&
        !sel_isEqual(selector, @selector(deleteForward))) return;
    if (!text_utf8_to_utf16(w->base.text_surrounding ?: "", w->base.text_begin, &begin) ||
        !text_utf8_to_utf16(w->base.text_surrounding ?: "", w->base.text_end, &end)) return;
    if (begin == end && sel_isEqual(selector, @selector(deleteBackward)) && begin > 0) {
        NSRange composed = [surrounding rangeOfComposedCharacterSequenceAtIndex:begin - 1];
        begin = composed.location;
    } else if (begin == end && sel_isEqual(selector, @selector(deleteForward)) && end < surrounding.length) {
        NSRange composed = [surrounding rangeOfComposedCharacterSequenceAtIndex:end];
        end = NSMaxRange(composed);
    }
    size_t byte_begin, byte_end;
    if (text_utf16_to_utf8(surrounding, begin, &byte_begin) &&
        text_utf16_to_utf8(surrounding, end, &byte_end) && byte_begin != byte_end)
        push_text_edit(w, "", 0, byte_begin, byte_end, false);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->destroyed || w->closed) return;
    NSString *str = plain_string(string);
    if (w->interpreting_key) w->key_composing = true;
    /* A new preedit starts at no marked location until a text field gives one. */
    w->marked_utf16_location = 0;
    /* A dead key composes before the surface has a text field, so the preedit
     * feedback does not depend on one. The range into the surrounding text is
     * the only part that needs the text field. */
    if (w->base.text_input_active) {
        size_t range_begin, range_end;
        if (!text_edit_range(w, replacementRange, true, &range_begin, &range_end)) return;
        w->preedit_begin = range_begin;
        w->preedit_end = range_end;
        w->preedit_revision = w->base.text_revision;
        w->preedit_generation = w->base.text_generation;
        if (!text_utf8_to_utf16(w->base.text_surrounding ?: "", range_begin,
                                &w->marked_utf16_location))
            w->marked_utf16_location = 0;
    }
    const char *utf8 = str.length ? str.UTF8String : NULL;
    free(w->preedit);
    w->preedit = utf8 ? strdup(utf8) : NULL;
    int begin = -1, end = -1;
    if (w->preedit && selectedRange.location != NSNotFound &&
        selectedRange.location <= str.length && selectedRange.length <= str.length - selectedRange.location) {
        NSString *prefix = [str substringToIndex:selectedRange.location];
        NSString *selection = [str substringWithRange:selectedRange];
        const char *prefix_utf8 = prefix.UTF8String;
        const char *selection_utf8 = selection.UTF8String;
        if (prefix_utf8 && selection_utf8) {
            begin = (int)strlen(prefix_utf8);
            end = begin + (int)strlen(selection_utf8);
        }
    }
    darwin_ctx.depth++;
    push_composition(w, w->preedit, begin, end);
    w->preedit_cursor_begin = begin;
    w->preedit_cursor_end = end;
    if (!w->destroyed && !w->closed)
        cocoa_surface_schedule_redraw(w);
    darwin_ctx.depth--;
}

- (void)unmarkText
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->destroyed || w->closed || !w->preedit) return;
    free(w->preedit);
    w->preedit = NULL;
    w->preedit_cursor_begin = w->preedit_cursor_end = -1;
    darwin_ctx.depth++;
    push_composition(w, NULL, -1, -1);
    if (!w->destroyed && !w->closed)
        cocoa_surface_schedule_redraw(w);
    darwin_ctx.depth--;
}

- (NSRange)selectedRange
{
    struct cocoa_surface *w = self.surface;
    if (!w || !w->base.text_input_active) return NSMakeRange(NSNotFound, 0);
    NSUInteger begin, end;
    if (!text_utf8_to_utf16(w->base.text_surrounding ?: "", w->base.text_begin, &begin) ||
        !text_utf8_to_utf16(w->base.text_surrounding ?: "", w->base.text_end, &end))
        return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(begin, end - begin);
}
- (NSRange)markedRange
{
    struct cocoa_surface *w = self.surface;
    if (!w || !w->preedit)
        return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(w->marked_utf16_location, [NSString stringWithUTF8String:w->preedit].length);
}
- (BOOL)hasMarkedText { return self.surface && self.surface->preedit != NULL; }
- (nullable NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                         actualRange:(nullable NSRangePointer)actualRange
{
    (void)range; (void)actualRange;
    return nil;
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange
{
    (void)range; (void)actualRange;
    struct cocoa_surface *w = self.surface;
    if (!w || !w->window) return NSZeroRect;
    CGFloat scale = surface_scale(w);
    NSRect r = NSMakeRect(w->ime_caret.x / scale,
        self.bounds.size.height - (w->ime_caret.y + w->ime_caret.height) / scale,
        w->ime_caret.width / scale, w->ime_caret.height / scale);
    return [w->window convertRectToScreen:[self convertRect:r toView:nil]];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return 0; }

@end

@implementation YaslWindow

- (NSRect)constrainFrameRect:(NSRect)frame toScreen:(NSScreen *)screen
{
    NSRect result = [super constrainFrameRect:frame toScreen:screen];
    if (darwin_ctx.base.trace_moves && self.surface && !NSEqualRects(frame, result)) {
        NSPoint mouse = NSEvent.mouseLocation;
        yasl_private_move_trace(&darwin_ctx.base, &self.surface->base, "constrain",
                "win=%ld screen=%u mouse=(%g,%g) in=(%g,%g %gx%g) out=(%g,%g %gx%g)",
                (long)self.windowNumber, screen_id(screen), mouse.x, mouse.y,
                NSMinX(frame), NSMinY(frame), NSWidth(frame), NSHeight(frame),
                NSMinX(result), NSMinY(result), NSWidth(result), NSHeight(result));
    }
    return result;
}

- (void)sendEvent:(NSEvent *)event
{
    darwin_ctx.depth++;
    @try {
        if (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown ||
            event.type == NSEventTypeOtherMouseDown) {
            dismiss_popup_press(self.surface);
        }
        /* AppKit keeps Control+Tab and Control+Shift+Tab for window tabbing and
         * key view navigation. It drops the press before the first responder
         * sees it. Send the press to the first responder so the application can
         * bind the key. AppKit tests menu key equivalents before this call, so
         * a menu item still wins. */
        if (event.type == NSEventTypeKeyDown && event.keyCode == kVK_Tab &&
            (event.modifierFlags & NSEventModifierFlagControl)) {
            NSResponder *responder = self.firstResponder;
            if ([responder respondsToSelector:@selector(keyDown:)]) {
                [responder keyDown:event];
                return;
            }
        }
        [super sendEvent:event];
    } @finally {
        darwin_ctx.depth--;
    }
}

/* Borderless windows cannot take focus unless they say so. Toplevels may; popups
 * never take focus from their parent. */
- (BOOL)canBecomeKeyWindow
{
    return self.surface && !self.surface->is_popup;
}

- (BOOL)canBecomeMainWindow
{
    return self.surface && !self.surface->is_popup;
}

@end

@implementation YaslWindowDelegate

- (void)windowDidBecomeKey:(NSNotification *)notification
{
    (void)notification;
    if (self.surface) push_focus(self.surface, true);
}

- (void)windowDidMiniaturize:(NSNotification *)notification
{
    (void)notification;
    if (self.surface) set_state(self.surface, YASL_PRIVATE_STATE_MINIMIZED, true);
}

- (void)windowDidDeminiaturize:(NSNotification *)notification
{
    (void)notification;
    if (self.surface) set_state(self.surface, YASL_PRIVATE_STATE_MINIMIZED, false);
}

- (void)windowDidResignKey:(NSNotification *)notification
{
    (void)notification;
    if (self.surface) push_focus(self.surface, false);
}

- (void)windowDidChangeOcclusionState:(NSNotification *)notification
{
    (void)notification;
    struct cocoa_surface *w = self.surface;
    if (!w) return;
    bool visible = (self.window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    bool was_suspended = (w->base.state & YASL_PRIVATE_STATE_SUSPENDED) != 0;
    darwin_ctx.depth++;
    set_state(w, YASL_PRIVATE_STATE_SUSPENDED, !visible);
    if (!w->destroyed && !w->closed) {
        if (visible && was_suspended) {
            cocoa_surface_schedule_redraw(w);
        } else if (!visible) {
            w->link.paused = YES;
            schedule_cpu_trim(w);
        }
    }
    darwin_ctx.depth--;
}

- (void)windowWillEnterFullScreen:(NSNotification *)notification
{
    (void)notification;
    self.fullscreenTransition = YES;
}

- (void)windowWillExitFullScreen:(NSNotification *)notification
{
    (void)notification;
    self.fullscreenTransition = YES;
}

- (void)syncWindowState
{
    self.fullscreenTransition = NO;
    struct cocoa_surface *w = self.surface;
    if (!w || w->closed)
        return;
    darwin_ctx.depth++;
    bool fullscreen = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    set_state(w, YASL_PRIVATE_STATE_FULLSCREEN, fullscreen);
    set_state(w, YASL_PRIVATE_STATE_MAXIMIZED, !fullscreen && self.window.isZoomed);
    set_state(w, YASL_PRIVATE_STATE_CSD, !(self.window.styleMask & NSWindowStyleMaskTitled));
    sync_size(w, false);
    sync_titlebar(w);
    darwin_ctx.depth--;
}

- (void)windowDidEnterFullScreen:(NSNotification *)notification { [self syncWindowState]; }
- (void)windowDidExitFullScreen:(NSNotification *)notification { [self syncWindowState]; }
- (void)windowDidFailToEnterFullScreen:(NSWindow *)window { (void)window; [self syncWindowState]; }
- (void)windowDidFailToExitFullScreen:(NSWindow *)window { (void)window; [self syncWindowState]; }

- (void)windowDidResize:(NSNotification *)notification
{
    (void)notification;
    if (!self.fullscreenTransition)
        [self syncWindowState];
}

- (void)windowWillMove:(NSNotification *)notification
{
    (void)notification;
    trace_window_move(self.surface, "will-move", NULL);
}

- (void)windowDidMove:(NSNotification *)notification
{
    (void)notification;
    struct cocoa_surface *w = self.surface;
    if (darwin_ctx.base.trace_moves && w && move_trace_sample(&w->move_trace_ns))
        trace_window_move(w, "did-move", NULL);
}

- (void)windowDidChangeScreen:(NSNotification *)notification
{
    (void)notification;
    struct cocoa_surface *w = self.surface;
    trace_window_move(w, "screen-change", NULL);
    sync_size(w, false);
    sync_titlebar(w);
    /* The link follows the window to its new screen. Wake it so it measures
     * the new cadence. A frame that already waits still waits for its tick. */
    if (w && !w->closed) {
        w->idle_frames = 0;
        if (w->link.paused)
            w->link.paused = NO;
    }
}

/* The close button is a request, as on Wayland and X11: the app destroys the
 * surface to comply. Programmatic closes come through windowWillClose instead. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    struct cocoa_surface *w = self.surface;
    if (w && !w->closed) {
        darwin_ctx.depth++;
        yasl_private_surface_notify_close(&w->base);
        darwin_ctx.depth--;
    }
    return NO;
}

- (void)windowWillStartLiveResize:(NSNotification *)notification
{
    (void)notification;
    darwin_ctx.base.modal = true;
}

- (void)windowDidEndLiveResize:(NSNotification *)notification
{
    (void)notification;
    darwin_ctx.base.modal = false;
}

- (void)windowWillClose:(NSNotification *)notification
{
    (void)notification;
    struct cocoa_surface *w = self.surface;
    if (!w || w->closed)
        return;
    darwin_ctx.depth++;
    w->closed = true;
    [w->link invalidate];
    yasl_private_surface_notify_close(&w->base);
    darwin_ctx.depth--;
}

@end

bool cocoa_surface_present(struct cocoa_surface *w, const struct yasl_private_framebuffer *fb,
                          const struct yasl_private_damage_list *damage)
{
    if (!w || w->base.presenter != YASL_PRIVATE_PRESENTER_CPU || w->closed || !w->rendering || w->did_present || !fb || !damage ||
        fb->pixels != w->frame.target.pixels || fb->size.x != w->size.x || fb->size.y != w->size.y)
        return false;
    if (w->use_iosurface) {
        if (!w->present_acquired)
            return false;
        cpu_commit_slot(w, w->present_slot);
        w->present_acquired = false;
        w->did_present = true;
        return true;
    }
    if (!w->drawable)
        return false;
    if (w->software_queue) {
        if (fb->pixels != w->pixels || fb->size.x != w->size.x || fb->size.y != w->size.y)
            return false;
        NSUInteger row_bytes = ((NSUInteger)w->size.x * 4 + 255) & ~(NSUInteger)255;
        NSUInteger length = row_bytes * (NSUInteger)w->size.y;
        if (!w->software_stages)
            w->software_stages = software_stages_new(w->metal_layer.device);
        /* The blit of the last frame can still read its buffer, so the frame
         * takes a buffer that no started command reads. Every buffer is in use:
         * the frame draws nothing and comes back at the next display frame. */
        struct software_stage *stage = software_stage_acquire(w->software_stages, length);
        if (!stage) return false;
        id<MTLBuffer> buffer = (id<MTLBuffer>)software_stage_buffer(stage);
        if (!buffer) {
            software_stage_cancel(w->software_stages, stage);
            return false;
        }
        for (int y = 0; y < w->size.y; y++)
            memcpy((char *)buffer.contents + (NSUInteger)y * row_bytes,
                   w->pixels + (size_t)y * w->size.x, (size_t)w->size.x * 4);
        id<MTLCommandBuffer> command = [w->software_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!command || !blit) {
            software_stage_cancel(w->software_stages, stage);
            return false;
        }
        id<CAMetalDrawable> drawable = w->drawable;
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:row_bytes
           sourceBytesPerImage:length sourceSize:MTLSizeMake(w->size.x, w->size.y, 1)
                    toTexture:drawable.texture destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        /* The command reads the buffer from the commit on and returns it when it
         * completes. */
        software_stage_finish(w->software_stages, stage, command);
        if (w->transaction) {
            /* The layer presents with the Core Animation transaction of the live
             * resize. The drawable must go out after the command is scheduled. */
            [command commit];
            [command waitUntilScheduled];
            [drawable present];
        } else {
            [command presentDrawable:drawable];
            [command commit];
        }
    }
    yasl_private_clock_presented(&w->clock, yasl_private_loop_now_ns(), false);
    w->did_present = true;
    w->shown_size = w->size;
    return true;
}

/* render_and_commit draws only when this holds; readiness uses it too, so a
 * window that cannot draw never keeps the loop from waiting. */
static bool draw_possible(const struct cocoa_surface *w)
{
    return w->base.presenter == YASL_PRIVATE_PRESENTER_CPU && !w->destroyed && !w->closed &&
           w->cbs->draw && !w->rendering && w->size.x > 0 && w->size.y > 0 &&
           !(w->base.state & YASL_PRIVATE_STATE_SUSPENDED);
}

static void render_and_commit(struct cocoa_surface *w)
{
    if (!draw_possible(w))
        return;

    if (w->use_iosurface) {
        uint32_t *px = NULL;
        int stride = 0;
        int slot = cpu_acquire_slot(w, &px, &stride);
        if (slot < 0) {
            w->want_draw = true;
            w->frames_skipped++;
            return;
        }
        darwin_ctx.depth++;
        w->rendering = true;
        w->want_draw = false;
        w->frame_due = false;
        w->did_present = false;
        w->present_slot = slot;
        w->present_acquired = true;
        w->transaction = w->view.inLiveResize;
        w->damage = (struct yasl_private_damage_list){0};
        w->frame = (struct yasl_private_frame){
            .surface = &w->base,
            .target = {
                .generation = yasl_private_frame_begin(&w->base),
                .pixels = px,
                .size = w->size,
                .stride_bytes = stride,
            },
        };
        yasl_private_surface_draw(&w->base, &w->frame);
        if (w->present_acquired) {
            cpu_unlock_slot(&w->cpu_slots[w->present_slot]);
            w->present_acquired = false;
        }
        if (!w->destroyed && !w->closed && !w->did_present) {
            w->want_draw = true;
            w->frames_skipped++;
        }
        w->rendering = false;
        darwin_ctx.depth--;
        return;
    }

    size_t count = (size_t)w->size.x * (size_t)w->size.y;
    if (w->software_queue && count > w->pixel_capacity) {
        size_t capacity = grown_capacity(w->pixel_capacity, count);
        uint32_t *pixels = realloc(w->pixels, capacity * sizeof(*pixels));
        if (!pixels) return;
        w->pixels = pixels;
        w->pixel_capacity = capacity;
    }
    darwin_ctx.depth++;
    w->rendering = true;
    w->want_draw = false;
    w->frame_due = false;
    w->did_present = false;
    id<CAMetalDrawable> drawable = w->metal_layer ? [w->metal_layer nextDrawable] : nil;
    w->drawable = drawable;
    w->transaction = w->metal_layer ? w->metal_layer.presentsWithTransaction : false;
    w->damage = (struct yasl_private_damage_list){0};
    w->frame = (struct yasl_private_frame){
        .surface = &w->base,
        .target = {
            .generation = yasl_private_frame_begin(&w->base),
            .pixels = w->software_queue ? w->pixels : NULL,
            .size = w->size,
        },
    };
    if (drawable)
        yasl_private_surface_draw(&w->base, &w->frame);
    if (!w->destroyed && !w->closed && !w->did_present) {
        w->want_draw = true;
        w->frames_skipped++;
    }
    w->drawable = nil;
    w->rendering = false;
    if (w->transaction)
        [CATransaction flush];
    darwin_ctx.depth--;
}

static void pump_window(struct cocoa_surface *w)
{
    if (!w || w->destroyed || w->closed || !w->want_draw || !w->cbs->draw)
        return;
    if (w->link && !w->frame_due && !w->unpaced)
        return;
    @autoreleasepool { render_and_commit(w); }
}

static void loop_ready(CFFileDescriptorRef fd, CFOptionFlags flags, void *info)
{
    (void)fd;
    (void)flags;
    (void)info;
    NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:LOOP_WAKE_SUBTYPE
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
}

/* A kqueue mirroring the loop's poll set, or -1. complete is false when some
 * descriptor could not be registered. */
static int loop_kqueue(struct yasl_loop *loop, bool *complete)
{
    *complete = true;
    int kq = kqueue();
    if (kq < 0) {
        *complete = false;
        return -1;
    }
    const struct pollfd *fds;
    int n = yasl_private_loop_poll_set(loop, &fds);
    for (int i = 0; i < n; i++) {
        struct kevent change;
        if (fds[i].events & POLLIN) {
            EV_SET(&change, fds[i].fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
            if (kevent(kq, &change, 1, NULL, 0, NULL) < 0) *complete = false;
        }
        if (fds[i].events & POLLOUT) {
            EV_SET(&change, fds[i].fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
            if (kevent(kq, &change, 1, NULL, 0, NULL) < 0) *complete = false;
        }
    }
    return kq;
}

static bool is_loop_wake(NSEvent *ev)
{
    return ev.type == NSEventTypeApplicationDefined && ev.subtype == LOOP_WAKE_SUBTYPE;
}

/* Before the wait: draw due windows. True when a window can draw now. */
bool cocoa_display_prepare(struct cocoa_display *d)
{
    assert([NSThread isMainThread]);
    cocoa_display_flush(d);
    for (int i = 0; i < d->count; i++) {
        struct cocoa_surface *w = d->windows[i];
        if (w->want_draw && (!w->link || w->frame_due || w->unpaced) && draw_possible(w))
            return true;
    }
    return false;
}

/* The loop's wait. The loop has already shortened timeout_ms to its next
 * timer, and reads descriptor readiness after this returns. */
int cocoa_display_wait(struct cocoa_display *d, int timeout_ms)
{
    assert([NSThread isMainThread]);
    if (d->pumping)
        return -EBUSY;
    d->pumping = true;
    d->depth++;
    int count = 0;
    @try {
        /* AppKit waits on Mach ports, not on loop descriptors. While waiting,
         * a run loop source on a kqueue of the loop's descriptors posts a wake
         * event, so kicks, posts, signals, and watches end the wait. */
        CFFileDescriptorRef wake_fd = NULL;
        CFRunLoopSourceRef wake_source = NULL;
        if (timeout_ms != 0 && d->loop) {
            bool complete;
            int kq = loop_kqueue(d->loop, &complete);
            if (kq >= 0) {
                wake_fd = CFFileDescriptorCreate(kCFAllocatorDefault, kq, true, loop_ready, NULL);
                if (!wake_fd) close(kq);
            }
            if (wake_fd) {
                CFFileDescriptorEnableCallBacks(wake_fd, kCFFileDescriptorReadCallBack);
                wake_source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, wake_fd, 0);
            }
            if (wake_source)
                CFRunLoopAddSource(CFRunLoopGetCurrent(), wake_source, kCFRunLoopDefaultMode);
            else
                complete = false;
            if (!complete && (timeout_ms < 0 || timeout_ms > LOOP_FALLBACK_MS))
                timeout_ms = LOOP_FALLBACK_MS;
        }

        NSDate *limitDate;
        if (timeout_ms == 0)
            limitDate = [NSDate distantPast];
        else if (timeout_ms < 0)
            limitDate = [NSDate distantFuture];
        else
            limitDate = [NSDate dateWithTimeIntervalSinceNow:(double)timeout_ms / 1000.0];

        @autoreleasepool {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:limitDate
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (wake_source) {
                CFRunLoopRemoveSource(CFRunLoopGetCurrent(), wake_source, kCFRunLoopDefaultMode);
                CFRelease(wake_source);
            }
            if (wake_fd) {
                CFFileDescriptorInvalidate(wake_fd);
                CFRelease(wake_fd);
            }
            while (ev) {
                if (!is_loop_wake(ev)) {
                    count++;
                    [NSApp sendEvent:ev];
                    [NSApp updateWindows];
                }
                ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
            }
        }
    } @finally {
        d->depth--;
        d->pumping = false;
    }
    return count;
}

void cocoa_display_flush(struct cocoa_display *d)
{
    assert([NSThread isMainThread]);
    if (!d) return;
    d->depth++;
    for (int i = d->count - 1; i >= 0; i--) {
        if (i < d->count)
            pump_window(d->windows[i]);
    }
    @autoreleasepool {
        [NSApp updateWindows];
    }
    if (--d->depth == 0)
        reap_dead(d);
}

void cocoa_surface_schedule_redraw(struct cocoa_surface *w)
{
    if (!w || w->closed) return;
    w->idle_frames = 0;
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        /* The application draws this surface itself. The next display tick
         * answers the intent, so a sustained draw follows the display. A
         * fallback timer answers it when the window is off screen, so a
         * request never waits for a tick that cannot come. The tick is the
         * usual answer, so a stale interval cannot hold the draw back. */
        /* A paused link means that the window drew nothing for a while, so
         * there is no display cadence in flight. Answer at once, and the wake
         * draws without a wait. From the next frame on, the link runs again
         * and its ticks pace the draws. A link that still runs, or that cannot
         * run here, keeps the answer on the tick or on the fallback. */
        uint64_t now = yasl_private_loop_now_ns();
        uint64_t interval = yasl_private_clock_refresh_ns(w->clock.refresh_ns);
        if (w->link.paused && !w->unpaced && w->base.frame_requested && !w->want_draw &&
            (!w->base.frame_answered_ns || now - w->base.frame_answered_ns >= interval)) {
            w->idle_frames = 0;
            cancel_tick_timer(w);
            /* Answer this intent at once, then let the link run again: its ticks
             * pace the draws that follow. A link that stayed paused would keep
             * the answer of every later request here, with no display cadence.
             * A link that occlusion paused stays paused. */
            if (!(w->base.state & YASL_PRIVATE_STATE_SUSPENDED))
                w->link.paused = NO;
            yasl_private_surface_notify_frame_shown(&w->base);
            yasl_private_loop_wake(darwin_ctx.loop);
            return;
        }
        w->want_draw = true;
        if (w->base.frame_requested && !w->tick_timer)
            w->tick_timer = yasl_private_surface_add_timer(&w->base, tick_wait_ns(w),
                                                          tick_fallback, w);
        /* Occlusion pauses the link. The exposure handler asks for a frame
         * again, which unpauses it here. */
        if (!(w->base.state & YASL_PRIVATE_STATE_SUSPENDED) && w->link.paused)
            w->link.paused = NO;
        yasl_private_loop_wake(darwin_ctx.loop);
        return;
    }
    w->want_draw = true;
    if (w->link.paused) {
        w->frame_due = true;
        w->link.paused = NO;
    }
    yasl_private_loop_wake(darwin_ctx.loop);
}

/* The popup that was created last. A popup takes the input grab, and a newer
 * popup takes it from an older one. */
static struct cocoa_surface *topmost_popup(struct cocoa_display *d)
{
    struct cocoa_surface *top = NULL;
    for (int i = 0; i < d->count; i++) {
        struct cocoa_surface *w = d->windows[i];
        if (!w->is_popup || w->closed || w->destroyed || !w->window)
            continue;
        if (!top || w->popup_order > top->popup_order)
            top = w;
    }
    return top;
}

/* A press dismisses the topmost popup unless the press is in it, as the pointer
 * grab does on X11. A press arrives at a YASL window of this application, so a
 * press in another application keeps the popup. */
static void dismiss_popup_press(const struct cocoa_surface *pressed)
{
    struct cocoa_surface *top = topmost_popup(&darwin_ctx);
    if (!top || top == pressed)
        return;
    darwin_ctx.depth++;
    yasl_private_surface_notify_close(&top->base);
    darwin_ctx.depth--;
}

/*
 * AppKit's screen layout manager refits a window that a move takes to another
 * display: _windowMoved: calls _setFrameAfterMove:, which calls
 * _setFrame:fromAdjustmentToScreen:anchorIfNeeded:animate:. The refit moves
 * the window to the frame that it last had on that display, or to a
 * proportional place on it. A YASL window skips the refit of a move while
 * the primary button is down, so a dragged window stays under the pointer.
 * AppKit still refits at the release. It also refits when a display comes or
 * goes, because that refit does not come from a move. With
 * WSAP_TRACE_WINDOW_MOVE=1, each refit writes a trace line.
 * Note (2026-11-20): macOS 27.2 runs the refit during a drag.
 */
typedef void (*frame_after_move_imp)(id, SEL, NSRect);
typedef void (*frame_refit_imp)(id, SEL, NSRect, NSScreen *, const CGPoint *, BOOL);
static frame_after_move_imp appkit_frame_after_move;
static frame_refit_imp appkit_frame_refit;
static int frame_after_move_depth;

static void hook_frame_after_move(id self, SEL cmd, NSRect frame)
{
    frame_after_move_depth++;
    @try {
        appkit_frame_after_move(self, cmd, frame);
    } @finally {
        frame_after_move_depth--;
    }
}

static void hook_frame_refit(id self, SEL cmd, NSRect frame, NSScreen *screen,
                             const CGPoint *anchor, BOOL animate)
{
    bool dragging = frame_after_move_depth > 0 && (NSEvent.pressedMouseButtons & 1);
    struct cocoa_surface *w = ((YaslWindow *)self).surface;
    if (darwin_ctx.base.trace_moves && w) {
        NSRect from = ((NSWindow *)self).frame;
        yasl_private_move_trace(&darwin_ctx.base, &w->base, dragging ? "refit-skipped" : "refit",
                "win=%ld screen=%u after-move=%d from=(%g,%g %gx%g) to=(%g,%g %gx%g)",
                (long)((NSWindow *)self).windowNumber, screen_id(screen),
                frame_after_move_depth > 0, NSMinX(from), NSMinY(from), NSWidth(from),
                NSHeight(from), NSMinX(frame), NSMinY(frame), NSWidth(frame), NSHeight(frame));
    }
    if (!dragging)
        appkit_frame_refit(self, cmd, frame, screen, anchor, animate);
}

/* Returns true when argument `index` of `method` has the type `type`,
 * ignoring a const qualifier. install_refit_hooks calls it to check the
 * AppKit signatures. */
static bool argument_is(Method method, unsigned index, const char *type)
{
    char *arg = method_copyArgumentType(method, index);
    const char *t = arg && arg[0] == 'r' ? arg + 1 : arg;
    bool same = t && !strcmp(t, type);
    free(arg);
    return same;
}

/* Installs the refit hooks on YaslWindow. Call it at startup, before the
 * first window opens. It installs nothing when AppKit lacks either method or
 * the argument types differ, so a later macOS keeps its own behavior. */
static void install_refit_hooks(void)
{
    static bool tried;
    if (tried)
        return;
    tried = true;
    SEL after_sel = NSSelectorFromString(@"_setFrameAfterMove:");
    SEL refit_sel = NSSelectorFromString(@"_setFrame:fromAdjustmentToScreen:anchorIfNeeded:animate:");
    Method after = class_getInstanceMethod([NSWindow class], after_sel);
    Method refit = class_getInstanceMethod([NSWindow class], refit_sel);
    char after_ret[4] = "", refit_ret[4] = "";
    if (after)
        method_getReturnType(after, after_ret, sizeof(after_ret));
    if (refit)
        method_getReturnType(refit, refit_ret, sizeof(refit_ret));
    if (!after || !refit || strcmp(after_ret, "v") || strcmp(refit_ret, "v") ||
        method_getNumberOfArguments(after) != 3 || method_getNumberOfArguments(refit) != 6 ||
        !argument_is(after, 2, @encode(NSRect)) || !argument_is(refit, 2, @encode(NSRect)) ||
        !argument_is(refit, 3, @encode(id)) || !argument_is(refit, 4, @encode(CGPoint *)) ||
        !argument_is(refit, 5, @encode(BOOL)))
        return;
    appkit_frame_after_move = (frame_after_move_imp)method_getImplementation(after);
    appkit_frame_refit = (frame_refit_imp)method_getImplementation(refit);
    class_addMethod([YaslWindow class], after_sel, (IMP)hook_frame_after_move,
                    method_getTypeEncoding(after));
    class_addMethod([YaslWindow class], refit_sel, (IMP)hook_frame_refit,
                    method_getTypeEncoding(refit));
}

static void app_init(const char *app_name)
{
    /* finishLaunching runs once per process, even when the display reopens. */
    static bool finished_launching;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow.allowsAutomaticWindowTabbing = NO;
    install_refit_hooks();
    darwin_ctx.delegate = [[YaslAppDelegate alloc] init];
    NSApp.delegate = darwin_ctx.delegate;

    NSString *ns_name = app_name ? [NSString stringWithUTF8String:app_name] : @"YASL";
    NSMenu *menu = [[NSMenu alloc] init];
    NSMenuItem *app_item = [[NSMenuItem alloc] init];
    NSMenu *app_menu = [[NSMenu alloc] initWithTitle:ns_name];
    [app_menu addItemWithTitle:[NSString stringWithFormat:@"About %@", ns_name]
                        action:@selector(orderFrontStandardAboutPanel:)
                 keyEquivalent:@""];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", ns_name]
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];
    app_item.submenu = app_menu;
    [menu addItem:app_item];
    [app_menu release];
    [app_item release];
    NSApp.mainMenu = menu;
    [menu release];

    darwin_ctx.event_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:
        NSEventMaskKeyUp handler:^NSEvent *(NSEvent *event) {
            if (!(event.modifierFlags & NSEventModifierFlagCommand) || !event.window)
                return event;
            [event.window sendEvent:event];
            return nil;
        }];
    if (!finished_launching) {
        [NSApp finishLaunching];
        finished_launching = true;
    }
}

static void place_window(NSWindow *window)
{
    if (!darwin_ctx.count) {
        [window center];
        NSRect frame = window.frame;
        darwin_ctx.cascade = NSMakePoint(frame.origin.x, frame.origin.y + frame.size.height);
        return;
    }
    darwin_ctx.cascade = [window cascadeTopLeftFromPoint:darwin_ctx.cascade];
}

struct cocoa_display *cocoa_display_open(struct yasl_loop *loop, const struct yasl_private_display_spec *spec)
{
    if (![NSThread isMainThread]) {
        errno = EPERM;
        return NULL;
    }
    if (darwin_ctx.open) return NULL;
    const char *name = spec && spec->app_id ? spec->app_id : "YASL";
    @autoreleasepool { app_init(name); }
    darwin_ctx.base.ops = &yasl_private_cocoa_backend;
    darwin_ctx.base.native_handle = (__bridge void *)[NSApplication sharedApplication];
    darwin_ctx.open = true;
    if (!loop) {
        darwin_ctx.loop = yasl_loop_create();
        darwin_ctx.owns_loop = true;
    } else {
        darwin_ctx.loop = loop;
        darwin_ctx.owns_loop = false;
    }
    darwin_ctx.base.loop = darwin_ctx.loop;
    if (spec) {
        if (spec->app_id) darwin_ctx.app_id = strdup(spec->app_id);
        darwin_ctx.base.app_id = darwin_ctx.app_id;
    }
    return &darwin_ctx;
}

void cocoa_display_close(struct cocoa_display *d)
{
    if (!d) return;
    drag_end(d, YASL_PRIVATE_DRAG_CANCELLED);
    assert(!d->count && "destroy every surface before its display");
    reap_dead(d);
    if (d->event_monitor) {
        [NSEvent removeMonitor:d->event_monitor];
        d->event_monitor = nil;
    }
    if (d->owns_loop && d->loop) {
        yasl_loop_destroy(d->loop);
        d->loop = NULL;
        d->base.loop = NULL;
    }
    NSApp.delegate = nil;
    [d->delegate release];
    d->delegate = nil;
    [d->hidden_cursor release];
    [d->diagonal_cursors[0] release];
    [d->diagonal_cursors[1] release];
    free(d->app_id);
    free(d->windows);
    *d = (struct cocoa_display){0};
}

/* A Vulkan surface needs a loader and its Metal surface extension. This build
 * can have the headers and the system can have no driver, so probe the loader
 * once. The application supplies the loader of the real instance, which is the
 * system loader that this probe opens. */
static bool cocoa_vulkan_available(void)
{
#if YASL_HAS_VULKAN
    static int available = -1;
    if (available >= 0)
        return available != 0;
    available = 0;
    void *lib = dlopen("libvulkan.1.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libvulkan.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libMoltenVK.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        return false;
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)dlsym(lib,
            "vkEnumerateInstanceExtensionProperties");
    uint32_t count = 0;
    VkExtensionProperties *extensions = NULL;
    if (enumerate && enumerate(NULL, &count, NULL) == VK_SUCCESS && count) {
        extensions = calloc(count, sizeof(*extensions));
        if (extensions && enumerate(NULL, &count, extensions) == VK_SUCCESS) {
            /* The surface needs both the core surface extension and the Metal one. */
            bool surface = false, metal = false;
            for (uint32_t i = 0; i < count; i++) {
                if (!strcmp(extensions[i].extensionName, "VK_KHR_surface"))
                    surface = true;
                else if (!strcmp(extensions[i].extensionName, "VK_EXT_metal_surface"))
                    metal = true;
            }
            available = surface && metal;
        }
    }
    free(extensions);
    dlclose(lib);
    return available != 0;
#else
    return false;
#endif
}

uint32_t cocoa_display_caps(const struct cocoa_display *d)
{
    if (!d)
        return 0;
    uint32_t caps = YASL_PRIVATE_CAP_POPUPS | YASL_PRIVATE_CAP_DRAG |
                    YASL_PRIVATE_CAP_DRAG_CARRY | YASL_PRIVATE_CAP_PLACE_WINDOW |
                    YASL_PRIVATE_CAP_CSD | YASL_PRIVATE_CAP_NATIVE_GRAPHICS |
                    YASL_PRIVATE_CAP_FRAME_PACING | YASL_PRIVATE_CAP_TABLET |
                    YASL_PRIVATE_CAP_RELATIVE_POINTER | YASL_PRIVATE_CAP_TEXT_INPUT |
                    YASL_PRIVATE_CAP_TITLEBAR_OVERLAY;
    if (cocoa_vulkan_available())
        caps |= YASL_PRIVATE_CAP_VULKAN;
    return caps;
}

struct cocoa_surface *cocoa_surface_create_toplevel(struct cocoa_display *d,
                                                  const struct yasl_private_window_spec *spec, void *graphics_device)
{
    if (![NSThread isMainThread]) {
        errno = EPERM;
        return NULL;
    }
    if (!d || !d->open || !spec)
        return NULL;
    @autoreleasepool {
        if (darwin_ctx.count == 0)
            trace_move_screens();
        if (darwin_ctx.count >= darwin_ctx.capacity) {
            int cap = darwin_ctx.capacity ? darwin_ctx.capacity * 2 : 16;
            struct cocoa_surface **wins = realloc(darwin_ctx.windows, (size_t)cap * sizeof(*wins));
            if (!wins) return NULL;
            darwin_ctx.windows = wins;
            darwin_ctx.capacity = cap;
        }

        struct cocoa_surface *w = calloc(1, sizeof(*w));
        if (!w) return NULL;
        w->base.display = &d->base;
        w->base.creation_flags = spec->flags;
        bool external_gpu = spec->external_gpu || graphics_device != NULL;
        /* Leave an external-GPU surface unclaimed until the public bridge
         * selects Metal or Vulkan. Both bridges request the layer while the
         * presenter is still NONE. */
        w->base.presenter = external_gpu ? YASL_PRIVATE_PRESENTER_NONE : YASL_PRIVATE_PRESENTER_CPU;
        w->use_iosurface = (spec->flags & YASL_PRIVATE_SURFACE_CPU) && !external_gpu;
        cocoa_surface_set_callbacks(w, NULL, NULL);
        w->clock.refresh_ns = YASL_PRIVATE_REFRESH_DEFAULT_NS;
        const char *t = spec->title ? spec->title : "YASL";

        int win_w = spec->size.x > 0 ? spec->size.x : 800;
        int win_h = spec->size.y > 0 ? spec->size.y : 480;
        /* Undecorated windows stay minimizable and edge-resizable; only FIXED_SIZE
         * removes resizing. */
        NSWindowStyleMask style = (spec->flags & YASL_PRIVATE_SURFACE_UNDECORATED) ?
            NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable :
            NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (!(spec->flags & YASL_PRIVATE_SURFACE_FIXED_SIZE))
            style |= NSWindowStyleMaskResizable;
        bool titled = (style & NSWindowStyleMaskTitled) != 0;
        bool overlay = titled && (spec->flags & YASL_PRIVATE_SURFACE_TITLEBAR_OVERLAY);
        w->base.state = titled ? 0 : YASL_PRIVATE_STATE_CSD;
        /* The content of an overlay window covers the title bar, so the
         * requested size includes it. */
        if (overlay)
            style |= NSWindowStyleMaskFullSizeContentView;
        w->window = [[YaslWindow alloc] initWithContentRect:NSMakeRect(0, 0, win_w, win_h)
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        w->window.releasedWhenClosed = NO;
        w->base.native_handle = (__bridge void *)w->window;
        w->base.size = w->size;
        w->base.scale_120 = w->scale_120;
        w->delegate = [[YaslWindowDelegate alloc] init];
        w->delegate.window = w->window;
        w->delegate.surface = w;
        w->window.delegate = w->delegate;
        w->window.acceptsMouseMovedEvents = YES;
        w->window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        /* The full title: a byte-limited copy could split a UTF-8 sequence. */
        w->window.title = [NSString stringWithUTF8String:t] ?: @"YASL";
        w->window.colorSpace = [NSColorSpace sRGBColorSpace];
        /* The application draws the title. The window menu, Mission Control,
         * and the Dock still show it. */
        if (overlay) {
            w->window.titlebarAppearsTransparent = YES;
            w->window.titleVisibility = NSWindowTitleHidden;
            w->window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        }

        w->view = [[YaslView alloc] initWithFrame:w->window.contentView.bounds cpuLayer:w->use_iosurface];
        w->window.contentView = w->view;
        w->layer = w->view.layer;
        w->metal_layer = w->use_iosurface ? nil : (CAMetalLayer *)w->view.layer;
        if (w->metal_layer) {
            if (graphics_device) {
                w->metal_layer.device = (id<MTLDevice>)graphics_device;
            } else if (!external_gpu) {
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                w->metal_layer.device = device;
                w->software_queue = [device newCommandQueue];
                [device release];
                if (!w->software_queue) {
                    w->window.delegate = nil;
                    w->delegate.surface = NULL;
                    w->delegate.window = nil;
                    [w->window close];
                    [w->view release];
                    [w->delegate release];
                    [w->window release];
                    free(w);
                    return NULL;
                }
                w->metal_layer.framebufferOnly = NO;
            }
        }

        w->link = [[w->view displayLinkWithTarget:w->view
                                         selector:@selector(displayLinkFired:)] retain];
        [w->link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

        place_window(w->window);
        w->window.surface = w;
        w->view.surface = w;
        darwin_ctx.windows[darwin_ctx.count++] = w;

        if (!(spec->flags & YASL_PRIVATE_SURFACE_HIDDEN)) {
            [w->window makeKeyAndOrderFront:nil];
            [w->window makeFirstResponder:w->view];
        }

        sync_size(w, true);
        sync_titlebar(w);
        if (spec->tearing) {
            if (w->metal_layer) w->metal_layer.displaySyncEnabled = NO;
            w->unpaced = true;
        }
        if (spec->fullscreen)
            [w->window toggleFullScreen:nil];
        if (spec->maximized)
            [w->window zoom:nil];
        return w;
    }
}

static void surface_free(struct cocoa_surface *w)
{
    cancel_tick_timer(w);
    cancel_trim_timer(w);
    cpu_release_all_slots(w);
    [w->mouse_down release];
    [w->link release];
    software_stages_free(w->software_stages);
    [w->software_queue release];
    [w->view release];
    [w->delegate release];
    [w->window release];
    free(w->preedit);
    free(w->pixels);
    free(w);
}

static void reap_dead(struct cocoa_display *d)
{
    assert(d->depth == 0);
    while (d->dead) {
        struct cocoa_surface *w = d->dead;
        d->dead = w->dead_next;
        surface_free(w);
    }
}

void cocoa_surface_destroy(struct cocoa_surface *w)
{
    assert([NSThread isMainThread]);
    if (!w || w->destroyed) return;
    if (darwin_ctx.drag.active) {
        if (darwin_ctx.drag.origin == w || darwin_ctx.drag.carried == w)
            drag_end(&darwin_ctx, YASL_PRIVATE_DRAG_CANCELLED);
        else if (darwin_ctx.drag.hover == w)
            darwin_ctx.drag.hover = NULL;
    }
    if (w->is_popup && w->parent && w->parent->window) {
        [w->parent->window removeChildWindow:w->window];
        w->parent = NULL;
    }
    for (int i = 0; i < darwin_ctx.count; i++) {
        if (darwin_ctx.windows[i]->parent == w) {
            if (w->window && darwin_ctx.windows[i]->window)
                [w->window removeChildWindow:darwin_ctx.windows[i]->window];
            darwin_ctx.windows[i]->parent = NULL;
        }
    }
    w->buttons = 0;
    w->tablet_down = false;
    /* The mouse association is process-wide, so a destroyed surface must release
     * the lock it holds. */
    if (w->pointer_locked) {
        w->pointer_locked = false;
        w->input.relative = false;
        CGAssociateMouseAndMouseCursorPosition(1);
    }
    darwin_ctx.depth++;
    bool native_closed = w->closed;
    static const struct yasl_private_window_cbs no_callbacks = {0};
    w->destroyed = true;
    w->closed = true;
    w->cbs = &no_callbacks;
    w->ud = NULL;
    [w->link invalidate];
    /* The delegate is removed before the window can end its live resize, so the
     * modal flag would never be cleared there. */
    if (w->window.inLiveResize)
        darwin_ctx.base.modal = false;
    w->window.delegate = nil;
    w->delegate.surface = NULL;
    w->delegate.window = nil;
    w->window.surface = NULL;
    w->view.surface = NULL;
    if (!native_closed)
        [w->window close];
    if (w->layer)
        w->layer.contents = nil;
    cpu_release_all_slots(w);
    input_queue_clear(&w->input);
    input_queue_clear(&w->text_events);
    w->dead_next = darwin_ctx.dead;
    darwin_ctx.dead = w;

    for (int i = 0; i < darwin_ctx.count; i++) {
        if (darwin_ctx.windows[i] == w) {
            darwin_ctx.windows[i] = darwin_ctx.windows[--darwin_ctx.count];
            break;
        }
    }
    if (!darwin_ctx.count) {
        free(darwin_ctx.windows);
        darwin_ctx.windows = NULL;
        darwin_ctx.capacity = 0;
        darwin_ctx.cascade = NSZeroPoint;
    }
    darwin_ctx.depth--;
    if (darwin_ctx.loop)
        yasl_private_loop_kick(darwin_ctx.loop);
}

void cocoa_surface_set_title(struct cocoa_surface *w, const char *title)
{
    if (!w || w->closed || !title) return;
    /* The full title: a byte-limited copy could split a UTF-8 sequence. */
    w->window.title = [NSString stringWithUTF8String:title] ?: @"YASL";
}

static bool impl_surface_set_size(struct yasl_surface *surface, struct yasl_private_vector2i size)
{
    struct cocoa_surface *w = (struct cocoa_surface *)surface;
    if (!w || w->closed || w->is_popup || size.x <= 0 || size.y <= 0) {
        errno = EINVAL;
        return false;
    }
    trace_window_move(w, "app-resize", NULL);
    [w->window setContentSize:NSMakeSize(size.x, size.y)];
    errno = 0;
    return true;
}

static bool impl_surface_set_fullscreen(struct yasl_surface *surface, bool enabled)
{
    struct cocoa_surface *w = (struct cocoa_surface *)surface;
    if (!w || w->closed || w->is_popup || !w->window) {
        errno = EINVAL;
        return false;
    }
    bool fullscreen = (w->window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (fullscreen != enabled)
        [w->window toggleFullScreen:nil];
    errno = 0;
    return true;
}

static bool impl_surface_set_maximized(struct yasl_surface *surface, bool enabled)
{
    struct cocoa_surface *w = (struct cocoa_surface *)surface;
    if (!w || w->closed || w->is_popup || !w->window) {
        errno = EINVAL;
        return false;
    }
    if (w->window.isZoomed != enabled)
        [w->window zoom:nil];
    errno = 0;
    return true;
}

static bool impl_surface_minimize(struct yasl_surface *surface)
{
    struct cocoa_surface *w = (struct cocoa_surface *)surface;
    if (!w || w->closed || w->is_popup || !w->window) {
        errno = EINVAL;
        return false;
    }
    [w->window miniaturize:nil];
    errno = 0;
    return true;
}

void cocoa_surface_set_background_color(struct cocoa_surface *w, uint32_t bg)
{
    if (!w || w->closed || w->is_popup) return;
    w->bg_color = bg;
    void (^apply)(void) = ^{
        if (w->closed || !w->window) return;
        double r = ((bg >> 16) & 0xff) / 255.0;
        double g = ((bg >> 8) & 0xff) / 255.0;
        double b = (bg & 0xff) / 255.0;
        w->window.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
        bool dark = 0.299 * r + 0.587 * g + 0.114 * b < 0.5;
        w->window.appearance = [NSAppearance appearanceNamed:
            dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    };
    if ([NSThread isMainThread])
        apply();
    else
        dispatch_async(dispatch_get_main_queue(), apply);
}

void cocoa_surface_set_cursor(struct cocoa_surface *w, enum yasl_private_cursor cursor)
{
    if (!w || w->closed || cursor == w->cursor || (unsigned)cursor > YASL_PRIVATE_CURSOR_HIDDEN) return;
    w->cursor = cursor;
    NSPoint p = [w->view convertPoint:w->window.mouseLocationOutsideOfEventStream fromView:nil];
    if (w->window.isKeyWindow && NSPointInRect(p, w->view.bounds))
        apply_cursor(w);
}

static bool place_under_pointer(struct cocoa_surface *w, NSPoint at)
{
    if (!w || w->closed || !w->window) return false;
    NSPoint mouse = [NSEvent mouseLocation];
    NSRect bounds = w->view.bounds;
    NSPoint top = [w->view convertPoint:NSMakePoint(NSMinX(bounds), NSMaxY(bounds))
                                 toView:nil];
    top = [w->window convertPointToScreen:top];
    NSPoint origin = w->window.frame.origin;
    origin.x += mouse.x - (top.x + at.x);
    origin.y += mouse.y - (top.y - at.y);
    [w->window setFrameOrigin:origin];
    if (darwin_ctx.base.trace_moves && move_trace_sample(&w->place_trace_ns))
        trace_window_move(w, "place", &(struct move_request){ .mouse = mouse,
                                                            .grab = at, .origin = origin });
    return true;
}

bool cocoa_surface_place_under_pointer(struct cocoa_surface *w, struct yasl_private_vector2i at)
{
    CGFloat scale = surface_scale(w);
    return place_under_pointer(w, NSMakePoint(at.x / scale, at.y / scale));
}

void cocoa_surface_set_ime_caret(struct cocoa_surface *w, struct yasl_private_rect2i caret)
{
    if (w) w->ime_caret = caret;
}

bool cocoa_surface_ime_preedit_cursor(const struct cocoa_surface *w, int *begin, int *end)
{
    if (!w || !w->preedit || w->preedit_cursor_begin < 0)
        return false;
    if (begin) *begin = w->preedit_cursor_begin;
    if (end) *end = w->preedit_cursor_end;
    return true;
}

static NSPasteboard *pasteboard_for_selection(enum yasl_private_selection sel)
{
    /* macOS has one system pasteboard, which is the clipboard. It has no primary
     * selection, so the backend reports no such capability and refuses the
     * selection instead of keeping a pasteboard of its own. */
    return sel == YASL_PRIVATE_CLIPBOARD ? [NSPasteboard generalPasteboard] : nil;
}

static bool cocoa_display_copy_text(struct cocoa_display *d, enum yasl_private_selection sel, const char *text);

static bool cocoa_display_copy_text(struct cocoa_display *d, enum yasl_private_selection sel, const char *text)
{
    assert([NSThread isMainThread]);
    if (!d || !text) { errno = EINVAL; return false; }
    NSPasteboard *pb = pasteboard_for_selection(sel);
    if (!pb) { errno = sel == YASL_PRIVATE_PRIMARY ? ENOTSUP : EINVAL; return false; }
    NSString *s = [NSString stringWithUTF8String:text];
    if (!s) { errno = EINVAL; return false; }
    [pb clearContents];
    if (![pb setString:s forType:NSPasteboardTypeString]) {
        errno = EIO;
        return false;
    }
    errno = 0;
    return true;
}

static bool cocoa_display_request_paste(struct cocoa_display *d, enum yasl_private_selection sel,
                                        struct cocoa_surface *target, uint64_t id)
{
    assert([NSThread isMainThread]);
    if (!d || !target || target->destroyed || target->closed ||
        target->base.display != &d->base) {
        errno = EINVAL;
        return false;
    }
    NSPasteboard *pb = pasteboard_for_selection(sel);
    if (!pb) { errno = sel == YASL_PRIVATE_PRIMARY ? ENOTSUP : EINVAL; return false; }
    NSString *s = [pb stringForType:NSPasteboardTypeString];
    const char *utf8 = s ? s.UTF8String : NULL;
    enum yasl_private_transfer_status status = !s ? YASL_PRIVATE_TRANSFER_UNAVAILABLE :
        utf8 ? YASL_PRIVATE_TRANSFER_OK : YASL_PRIVATE_TRANSFER_FAILED;
    yasl_private_surface_complete_paste(&target->base, id, status,
                                utf8 ? utf8 : NULL, utf8 ? strlen(utf8) : 0);
    errno = 0;
    return true;
}

int cocoa_display_modifiers(const struct cocoa_display *d)
{
    (void)d;
    return (int)cocoa_mods(NSEvent.modifierFlags);
}

struct yasl_private_geometry cocoa_surface_geometry(const struct cocoa_surface *w)
{
    return (struct yasl_private_geometry){
        .size = w ? w->size : (struct yasl_private_vector2i){0},
        .scale_120 = w ? w->scale_120 : YASL_PRIVATE_SCALE_UNIT,
    };
}

static bool popup_spec_valid(const struct cocoa_surface *parent,
                             const struct yasl_private_popup_spec *spec)
{
    if (!parent || !spec) return false;
    if (spec->anchor_edges > 8 || spec->gravity > 8 ||
            (spec->constraints & ~63u)) return false;
    int parent_cx = parent->is_popup ? parent->content_offset.x : 0;
    int parent_cy = parent->is_popup ? parent->content_offset.y : 0;
    int64_t x = (int64_t)spec->anchor.x - parent_cx;
    int64_t y = (int64_t)spec->anchor.y - parent_cy;
    return x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767 &&
           spec->anchor.width >= 0 && spec->anchor.height >= 0 &&
           spec->size.x > 0 && spec->size.y > 0 &&
           spec->size.x <= 32767 && spec->size.y <= 32767 &&
           spec->content_offset.x >= 0 && spec->content_offset.y >= 0 &&
           spec->content_size.x >= 0 && spec->content_size.y >= 0 &&
           (int64_t)spec->content_offset.x + spec->content_size.x <= spec->size.x &&
           (int64_t)spec->content_offset.y + spec->content_size.y <= spec->size.y;
}

enum {
    POPUP_EDGE_TOP = 1,
    POPUP_EDGE_BOTTOM = 2,
    POPUP_EDGE_LEFT = 3,
    POPUP_EDGE_RIGHT = 4,
    POPUP_EDGE_TOP_LEFT = 5,
    POPUP_EDGE_BOTTOM_LEFT = 6,
    POPUP_EDGE_TOP_RIGHT = 7,
    POPUP_EDGE_BOTTOM_RIGHT = 8,
    POPUP_CONSTRAINT_SLIDE_X = 1u << 0,
    POPUP_CONSTRAINT_SLIDE_Y = 1u << 1,
    POPUP_CONSTRAINT_FLIP_X = 1u << 2,
    POPUP_CONSTRAINT_FLIP_Y = 1u << 3,
    POPUP_CONSTRAINT_RESIZE_X = 1u << 4,
    POPUP_CONSTRAINT_RESIZE_Y = 1u << 5,
};

static bool popup_edge_has_left(unsigned edge)
{ return edge == POPUP_EDGE_LEFT || edge == POPUP_EDGE_TOP_LEFT || edge == POPUP_EDGE_BOTTOM_LEFT; }
static bool popup_edge_has_right(unsigned edge)
{ return edge == POPUP_EDGE_RIGHT || edge == POPUP_EDGE_TOP_RIGHT || edge == POPUP_EDGE_BOTTOM_RIGHT; }
static bool popup_edge_has_top(unsigned edge)
{ return edge == POPUP_EDGE_TOP || edge == POPUP_EDGE_TOP_LEFT || edge == POPUP_EDGE_TOP_RIGHT; }
static bool popup_edge_has_bottom(unsigned edge)
{ return edge == POPUP_EDGE_BOTTOM || edge == POPUP_EDGE_BOTTOM_LEFT || edge == POPUP_EDGE_BOTTOM_RIGHT; }

static unsigned popup_flip_x_edge(unsigned edge)
{
    switch (edge) {
    case POPUP_EDGE_LEFT: return POPUP_EDGE_RIGHT;
    case POPUP_EDGE_RIGHT: return POPUP_EDGE_LEFT;
    case POPUP_EDGE_TOP_LEFT: return POPUP_EDGE_TOP_RIGHT;
    case POPUP_EDGE_BOTTOM_LEFT: return POPUP_EDGE_BOTTOM_RIGHT;
    case POPUP_EDGE_TOP_RIGHT: return POPUP_EDGE_TOP_LEFT;
    case POPUP_EDGE_BOTTOM_RIGHT: return POPUP_EDGE_BOTTOM_LEFT;
    default: return edge;
    }
}

static unsigned popup_flip_y_edge(unsigned edge)
{
    switch (edge) {
    case POPUP_EDGE_TOP: return POPUP_EDGE_BOTTOM;
    case POPUP_EDGE_BOTTOM: return POPUP_EDGE_TOP;
    case POPUP_EDGE_TOP_LEFT: return POPUP_EDGE_BOTTOM_LEFT;
    case POPUP_EDGE_TOP_RIGHT: return POPUP_EDGE_BOTTOM_RIGHT;
    case POPUP_EDGE_BOTTOM_LEFT: return POPUP_EDGE_TOP_LEFT;
    case POPUP_EDGE_BOTTOM_RIGHT: return POPUP_EDGE_TOP_RIGHT;
    default: return edge;
    }
}

static CGFloat popup_axis_overflow(CGFloat min, CGFloat max, CGFloat visible_min, CGFloat visible_max)
{
    return MAX(0, visible_min - min) + MAX(0, max - visible_max);
}

static NSRect popup_frame_explicit(struct cocoa_surface *parent,
        const struct yasl_private_popup_spec *spec, unsigned anchor, unsigned gravity,
        NSPoint *screen_anchor)
{
    CGFloat scale = surface_scale(parent);
    CGFloat parent_x = (parent->is_popup ? parent->content_offset.x : 0) / scale;
    CGFloat parent_y = (parent->is_popup ? parent->content_offset.y : 0) / scale;
    CGFloat ax = spec->anchor.x / scale - parent_x;
    CGFloat ay = spec->anchor.y / scale - parent_y;
    CGFloat aw = spec->anchor.width / scale;
    CGFloat ah = spec->anchor.height / scale;
    CGFloat anchor_x = popup_edge_has_left(anchor) ? ax :
                       popup_edge_has_right(anchor) ? ax + aw : ax + aw * 0.5;
    CGFloat anchor_y = popup_edge_has_top(anchor) ? ay :
                       popup_edge_has_bottom(anchor) ? ay + ah : ay + ah * 0.5;
    CGFloat offset_x = spec->content_offset.x / scale;
    CGFloat offset_y = spec->content_offset.y / scale;
    CGFloat content_w = (spec->content_size.x ? spec->content_size.x : spec->size.x - spec->content_offset.x) / scale;
    CGFloat content_h = (spec->content_size.y ? spec->content_size.y : spec->size.y - spec->content_offset.y) / scale;
    CGFloat content_left = popup_edge_has_right(gravity) ? anchor_x :
                           popup_edge_has_left(gravity) ? anchor_x - content_w : anchor_x - content_w * 0.5;
    CGFloat content_top = popup_edge_has_bottom(gravity) ? anchor_y :
                          popup_edge_has_top(gravity) ? anchor_y - content_h : anchor_y - content_h * 0.5;
    NSPoint in_view = NSMakePoint(anchor_x, NSHeight(parent->view.bounds) - anchor_y);
    NSPoint in_window = [parent->view convertPoint:in_view toView:nil];
    NSPoint on_screen = [parent->window convertPointToScreen:in_window];
    if (screen_anchor)
        *screen_anchor = on_screen;
    CGFloat width = spec->size.x / scale;
    CGFloat height = spec->size.y / scale;
    return NSMakeRect(on_screen.x + (content_left - anchor_x) - offset_x,
                      on_screen.y - (content_top - anchor_y) + offset_y - height,
                      width, height);
}

static NSRect popup_calc_frame(struct cocoa_surface *parent, const struct yasl_private_popup_spec *spec)
{
    NSPoint anchor;
    NSRect frame = popup_frame_explicit(parent, spec, spec->anchor_edges, spec->gravity, &anchor);
    NSScreen *screen = parent->window.screen ?: [NSScreen mainScreen];
    /* The parent can span screens. Keep the popup on the anchor's screen. */
    for (NSScreen *candidate in NSScreen.screens) {
        if (NSPointInRect(anchor, candidate.frame)) {
            screen = candidate;
            break;
        }
    }
    if (!screen) return frame;
    NSRect visible = screen.visibleFrame;
    unsigned constraints = spec->constraints;

    if (constraints & POPUP_CONSTRAINT_FLIP_X) {
        NSRect flipped = popup_frame_explicit(parent, spec,
            popup_flip_x_edge(spec->anchor_edges), popup_flip_x_edge(spec->gravity), NULL);
        if (popup_axis_overflow(flipped.origin.x, NSMaxX(flipped), NSMinX(visible), NSMaxX(visible)) <
            popup_axis_overflow(frame.origin.x, NSMaxX(frame), NSMinX(visible), NSMaxX(visible)))
            frame = NSMakeRect(flipped.origin.x, frame.origin.y, flipped.size.width, frame.size.height);
    }
    if (constraints & POPUP_CONSTRAINT_FLIP_Y) {
        NSRect flipped = popup_frame_explicit(parent, spec,
            popup_flip_y_edge(spec->anchor_edges), popup_flip_y_edge(spec->gravity), NULL);
        if (popup_axis_overflow(flipped.origin.y, NSMaxY(flipped), NSMinY(visible), NSMaxY(visible)) <
            popup_axis_overflow(frame.origin.y, NSMaxY(frame), NSMinY(visible), NSMaxY(visible)))
            frame = NSMakeRect(frame.origin.x, flipped.origin.y, frame.size.width, flipped.size.height);
    }

    if (constraints & POPUP_CONSTRAINT_SLIDE_X) {
        if (NSMinX(frame) < NSMinX(visible)) frame.origin.x = NSMinX(visible);
        if (NSMaxX(frame) > NSMaxX(visible)) frame.origin.x = NSMaxX(visible) - frame.size.width;
    }
    if (constraints & POPUP_CONSTRAINT_SLIDE_Y) {
        if (NSMinY(frame) < NSMinY(visible)) frame.origin.y = NSMinY(visible);
        if (NSMaxY(frame) > NSMaxY(visible)) frame.origin.y = NSMaxY(visible) - frame.size.height;
    }
    if (constraints & POPUP_CONSTRAINT_RESIZE_X) {
        if (NSMinX(frame) < NSMinX(visible)) {
            frame.size.width = MAX(0, frame.size.width - (NSMinX(visible) - NSMinX(frame)));
            frame.origin.x = NSMinX(visible);
        }
        if (NSMaxX(frame) > NSMaxX(visible))
            frame.size.width = MAX(0, NSMaxX(visible) - frame.origin.x);
    }
    if (constraints & POPUP_CONSTRAINT_RESIZE_Y) {
        if (NSMinY(frame) < NSMinY(visible)) {
            frame.size.height = MAX(0, frame.size.height - (NSMinY(visible) - NSMinY(frame)));
            frame.origin.y = NSMinY(visible);
        }
        if (NSMaxY(frame) > NSMaxY(visible))
            frame.size.height = MAX(0, NSMaxY(visible) - frame.origin.y);
    }
    return frame;
}

struct cocoa_surface *cocoa_surface_create_popup(struct cocoa_surface *parent,
        const struct yasl_private_popup_spec *spec)
{
    if (![NSThread isMainThread]) {
        errno = EPERM;
        return NULL;
    }
    if (!parent || parent->closed || !parent->window || !parent->view ||
        !popup_spec_valid(parent, spec))
        return NULL;

    @autoreleasepool {
        if (darwin_ctx.count >= darwin_ctx.capacity) {
            int cap = darwin_ctx.capacity ? darwin_ctx.capacity * 2 : 16;
            struct cocoa_surface **wins = realloc(darwin_ctx.windows, (size_t)cap * sizeof(*wins));
            if (!wins) return NULL;
            darwin_ctx.windows = wins;
            darwin_ctx.capacity = cap;
        }

        struct cocoa_surface *w = calloc(1, sizeof(*w));
        if (!w) return NULL;
        /* Popups are always CPU surfaces, whatever the parent draws with. */
        w->base.display = parent->base.display;
        w->base.presenter = YASL_PRIVATE_PRESENTER_CPU;
        w->base.creation_flags = parent->base.creation_flags | YASL_PRIVATE_SURFACE_CPU;
        w->use_iosurface = true;
        cocoa_surface_set_callbacks(w, NULL, NULL);
        w->clock.refresh_ns = parent->clock.refresh_ns ? parent->clock.refresh_ns : YASL_PRIVATE_REFRESH_DEFAULT_NS;
        w->parent = parent;
        w->is_popup = true;
        w->popup_order = ++darwin_ctx.popup_serial;
        /* A popup has no system decorations, so the application draws its frame. */
        w->base.state = YASL_PRIVATE_STATE_CSD;
        w->content_offset = spec->content_offset;

        NSRect frame = popup_calc_frame(parent, spec);
        NSWindowStyleMask style = NSWindowStyleMaskBorderless;
        w->window = [[YaslWindow alloc] initWithContentRect:frame
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        w->window.releasedWhenClosed = NO;
        w->base.native_handle = (__bridge void *)w->window;
        w->base.size = w->size;
        w->base.scale_120 = w->scale_120;
        w->window.opaque = NO;
        w->window.backgroundColor = [NSColor clearColor];
        w->window.hasShadow = NO;
        w->delegate = [[YaslWindowDelegate alloc] init];
        w->delegate.window = w->window;
        w->delegate.surface = w;
        w->window.delegate = w->delegate;
        w->window.acceptsMouseMovedEvents = YES;
        w->window.colorSpace = [NSColorSpace sRGBColorSpace];

        w->view = [[YaslView alloc] initWithFrame:w->window.contentView.bounds cpuLayer:YES];
        w->window.contentView = w->view;
        w->layer = w->view.layer;
        w->layer.opaque = NO;

        w->link = [[w->view displayLinkWithTarget:w->view
                                         selector:@selector(displayLinkFired:)] retain];
        [w->link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

        w->window.surface = w;
        w->view.surface = w;
        darwin_ctx.windows[darwin_ctx.count++] = w;

        [parent->window addChildWindow:w->window ordered:NSWindowAbove];
        [w->window orderFront:nil];

        sync_size(w, true);
        return w;
    }
}

bool cocoa_surface_popup_reposition(struct cocoa_surface *popup,
        const struct yasl_private_popup_spec *spec)
{
    if (!popup || popup->closed || !popup->is_popup || !popup->parent ||
        !popup->parent->window || !popup->parent->view ||
        !popup_spec_valid(popup->parent, spec))
        return false;

    @autoreleasepool {
        popup->content_offset = spec->content_offset;
        NSRect frame = popup_calc_frame(popup->parent, spec);
        [popup->window setFrame:frame display:YES];
        sync_size(popup, false);
        return true;
    }
}

bool cocoa_surface_start_interactive_move(struct cocoa_surface *s)
{
    if (!s || !s->window || s->closed) {
        errno = EINVAL;
        return false;
    }
    /* The drag must start from a press still in progress. NSApp's current event
     * need not be that press, so use the mouse-down recorded for this window. */
    if (!s->mouse_down || [NSEvent pressedMouseButtons] == 0) {
        errno = ESTALE;
        return false;
    }
    trace_window_move(s, "interactive-move", NULL);
    [s->window performWindowDragWithEvent:s->mouse_down];
    return true;
}

static NSImage *image_from_pixels(const uint32_t *pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0)
        return nil;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!cs) return nil;
    size_t bytes = (size_t)width * (size_t)height * 4;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)pixels, (CFIndex)bytes);
    CGDataProviderRef provider = data ? CGDataProviderCreateWithCFData(data) : NULL;
    if (data)
        CFRelease(data);
    if (!provider) {
        CGColorSpaceRelease(cs);
        return nil;
    }
    CGImageRef cgImage = CGImageCreate((size_t)width, (size_t)height,
                                       8, 32, (size_t)width * 4, cs,
                                       kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
                                       provider, NULL, NO, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    if (!cgImage)
        return nil;
    NSImage *image = [[NSImage alloc] initWithCGImage:cgImage size:NSMakeSize(width, height)];
    CGImageRelease(cgImage);
    return image;
}

bool cocoa_display_drag_start(struct cocoa_display *d, struct cocoa_surface *origin,
                             const struct yasl_private_drag_image *image,
                             const struct yasl_private_drag_listener *l, void *ud)
{
    if (!d || !origin || !l) {
        errno = EINVAL;
        return false;
    }
    if (d->drag.active) {
        errno = EBUSY;
        return false;
    }
    if ([NSEvent pressedMouseButtons] == 0) {
        errno = ESTALE;
        return false;
    }
    trace_window_move(origin, "drag-start", NULL);
    d->drag.active = true;
    d->drag.origin = origin;
    d->drag.carried = NULL;
    d->drag.hover = NULL;
    d->drag.hover_pos = (struct yasl_private_vector2i){0};
    d->drag.listener = *l;
    d->drag.ud = ud;
    [d->drag.image release];
    d->drag.image = nil;
    if (d->drag.image_window) {
        [d->drag.image_window close];
        [d->drag.image_window release];
        d->drag.image_window = nil;
    }
    if (image && image->pixels && image->size.x > 0 && image->size.y > 0) {
        d->drag.image = image_from_pixels(image->pixels, image->size.x, image->size.y);
        if (d->drag.image) {
            CGFloat scale = surface_scale(origin);
            d->drag.image_hotspot = NSMakePoint(image->hotspot.x / scale,
                                               image->hotspot.y / scale);
            CGFloat pt_w = image->size.x / scale;
            CGFloat pt_h = image->size.y / scale;
            NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, pt_w, pt_h)
                                                        styleMask:NSWindowStyleMaskBorderless
                                                          backing:NSBackingStoreBuffered
                                                            defer:NO];
            win.releasedWhenClosed = NO;
            win.opaque = NO;
            win.backgroundColor = [NSColor clearColor];
            win.ignoresMouseEvents = YES;
            win.level = NSFloatingWindowLevel;
            win.hasShadow = YES;
            NSImageView *iv = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, pt_w, pt_h)];
            iv.image = d->drag.image;
            iv.imageScaling = NSImageScaleAxesIndependently;
            win.contentView = iv;
            [iv release];
            d->drag.image_window = win;
            NSPoint mouse = [NSEvent mouseLocation];
            [win setFrameOrigin:NSMakePoint(mouse.x - d->drag.image_hotspot.x,
                                           mouse.y - pt_h + d->drag.image_hotspot.y)];
            [win orderFront:nil];
        }
    }
    update_drag_hover(d, [NSEvent mouseLocation]);
    return true;
}

bool cocoa_surface_drag_carry(struct cocoa_surface *w, struct yasl_private_vector2i offset)
{
    if (!w || w->closed || !darwin_ctx.drag.active)
        return false;
    if (darwin_ctx.drag.image_window) {
        [darwin_ctx.drag.image_window close];
        [darwin_ctx.drag.image_window release];
        darwin_ctx.drag.image_window = nil;
    }
    darwin_ctx.drag.carried = w;
    /* Keep the grab point stable when the window changes backing scale. */
    CGFloat scale = surface_scale(w);
    darwin_ctx.drag.carry_offset = NSMakePoint(offset.x / scale, offset.y / scale);
    trace_window_move(w, "carry-start", NULL);
    return place_under_pointer(w, darwin_ctx.drag.carry_offset);
}

bool cocoa_surface_start_interactive_resize(struct cocoa_surface *s, enum yasl_private_resize_edge edge)
{
    (void)s; (void)edge;
    errno = ENOTSUP;
    return false;
}

bool cocoa_surface_show_window_menu(struct cocoa_surface *s, struct yasl_private_vector2i pos)
{
    (void)s; (void)pos;
    errno = ENOTSUP;
    return false;
}

void cocoa_surface_set_callbacks(struct cocoa_surface *w,
        const struct yasl_private_window_cbs *cbs, void *ud)
{
    if (!w || w->closed) return;
    w->base.callbacks = cbs ? *cbs : (struct yasl_private_window_cbs){0};
    w->base.ud = ud;
    w->cbs = &w->base.callbacks;
    w->ud = ud;
}

struct yasl_private_timing cocoa_surface_timing(const struct cocoa_surface *w)
{
    struct yasl_private_timing timing = {0};
    if (w) {
        timing.refresh_ns = w->clock.refresh_ns;
        timing.last_present_ns = w->clock.last_present_ns;
        timing.next_present_ns = yasl_private_clock_next_present_ns(&w->clock);
        timing.frames_skipped = w->frames_skipped;
        timing.source = w->clock.source;
        yasl_private_clock_resize_stats(&w->clock, &timing.resize);
    }
    return timing;
}

static struct yasl_private_display *impl_display_open(struct yasl_loop *loop, const struct yasl_private_display_spec *spec)
{
    struct cocoa_display *result = cocoa_display_open(loop, spec);
    return result ? &result->base : NULL;
}

static void impl_display_close(struct yasl_private_display *d)
{
    cocoa_display_close((struct cocoa_display *)d);
}

static uint32_t impl_display_caps(const struct yasl_private_display *d)
{
    return cocoa_display_caps((const struct cocoa_display *)d);
}

static bool impl_display_prepare(struct yasl_private_display *d)
{
    return cocoa_display_prepare((struct cocoa_display *)d);
}

static int impl_display_wait(struct yasl_private_display *d, int timeout_ms)
{
    return cocoa_display_wait((struct cocoa_display *)d, timeout_ms);
}

static void impl_display_check(struct yasl_private_display *d)
{
    (void)d;
}

static void impl_display_flush(struct yasl_private_display *d)
{
    cocoa_display_flush((struct cocoa_display *)d);
}

static int impl_display_double_click_ms(const struct yasl_private_display *d)
{
    (void)d;
    return (int)lround([NSEvent doubleClickInterval] * 1000.0);
}

static int impl_display_modifiers(const struct yasl_private_display *d)
{
    return cocoa_display_modifiers((const struct cocoa_display *)d);
}

static bool impl_display_copy_text(struct yasl_private_display *d, enum yasl_private_selection sel, const char *text)
{
    return cocoa_display_copy_text((struct cocoa_display *)d, sel, text);
}

static bool impl_display_request_paste(struct yasl_private_display *d, enum yasl_private_selection sel,
                                       struct yasl_surface *target, uint64_t request_id)
{
    return cocoa_display_request_paste((struct cocoa_display *)d, sel,
                                       (struct cocoa_surface *)target, request_id);
}

static bool impl_display_drag_start(struct yasl_private_display *d, struct yasl_surface *origin, const struct yasl_private_drag_image *image, const struct yasl_private_drag_listener *l, void *ud)
{
    return cocoa_display_drag_start((struct cocoa_display *)d, (struct cocoa_surface *)origin, image, l, ud);
}

static void impl_display_drag_cancel(struct yasl_private_display *d)
{
    drag_end((struct cocoa_display *)d, YASL_PRIVATE_DRAG_CANCELLED);
}

static struct yasl_surface *impl_surface_create_toplevel(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec, void *graphics_device)
{
    struct cocoa_surface *result = cocoa_surface_create_toplevel((struct cocoa_display *)d, spec, graphics_device);
    return result ? &result->base : NULL;
}

static struct yasl_surface *impl_surface_create_popup(struct yasl_surface *parent, const struct yasl_private_popup_spec *spec)
{
    struct cocoa_surface *result = cocoa_surface_create_popup((struct cocoa_surface *)parent, spec);
    return result ? &result->base : NULL;
}

static bool impl_surface_popup_reposition(struct yasl_surface *popup, const struct yasl_private_popup_spec *spec)
{
    return cocoa_surface_popup_reposition((struct cocoa_surface *)popup, spec);
}

static void impl_surface_destroy(struct yasl_surface *s)
{
    cocoa_surface_destroy((struct cocoa_surface *)s);
}

static struct input_queue *impl_surface_input_queue(struct yasl_surface *s)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    return w ? &w->input : NULL;
}

static struct yasl_private_display *impl_surface_display(const struct yasl_surface *s)
{
    return s ? &darwin_ctx.base : NULL;
}

static void impl_surface_set_callbacks(struct yasl_surface *s, const struct yasl_private_window_cbs *cbs, void *ud)
{
    cocoa_surface_set_callbacks((struct cocoa_surface *)s, cbs, ud);
}

static void impl_surface_set_title(struct yasl_surface *s, const char *title)
{
    cocoa_surface_set_title((struct cocoa_surface *)s, title);
}

static void impl_surface_schedule_redraw(struct yasl_surface *s)
{
    cocoa_surface_schedule_redraw((struct cocoa_surface *)s);
}

/* A frame request wakes the tick gate of the window, and the next display tick
 * answers it. For the CPU presenter the gate is the display flush. No frame
 * here draws at once, so a request waits for the display at every rate. */
static void impl_surface_wake_frame_request(struct yasl_surface *s)
{
    cocoa_surface_schedule_redraw((struct cocoa_surface *)s);
}

/* An application-drawn surface waits for the display tick that answers its
 * frame intent. The fallback timer answers it when no tick comes. The CPU
 * presenter presents in the display flush, so it waits for no frame. */
static bool impl_surface_frame_in_flight(const struct yasl_surface *s)
{
    const struct cocoa_surface *w = (const struct cocoa_surface *)s;
    return w && w->base.presenter != YASL_PRIVATE_PRESENTER_CPU && w->want_draw;
}

static bool impl_surface_present(struct yasl_surface *s, const struct yasl_private_framebuffer *fb, const struct yasl_private_damage_list *damage)
{
    return cocoa_surface_present((struct cocoa_surface *)s, fb, damage);
}

static struct yasl_private_geometry impl_surface_geometry(const struct yasl_surface *s)
{
    return cocoa_surface_geometry((const struct cocoa_surface *)s);
}

static struct yasl_private_timing impl_surface_timing(const struct yasl_surface *s)
{
    return cocoa_surface_timing((const struct cocoa_surface *)s);
}

static uint64_t impl_surface_frame_time_ns(struct yasl_surface *s, uint64_t now)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    return w ? yasl_private_clock_frame_timestamp_ns(&w->clock, now) : now;
}

static void impl_surface_set_cursor(struct yasl_surface *s, enum yasl_private_cursor cursor)
{
    cocoa_surface_set_cursor((struct cocoa_surface *)s, cursor);
}

static bool impl_surface_set_pointer_mode(struct yasl_surface *s, enum yasl_private_pointer_mode mode)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    if (!w || w->closed || w->destroyed) {
        errno = EINVAL;
        return false;
    }
    w->pointer_locked = mode == YASL_PRIVATE_POINTER_RELATIVE;
    w->input.relative = w->pointer_locked;
    apply_pointer_lock(w);
    errno = 0;
    return true;
}

static void impl_surface_set_background_color(struct yasl_surface *s, uint32_t bg)
{
    cocoa_surface_set_background_color((struct cocoa_surface *)s, bg);
}

static const char *impl_surface_ime_preedit(const struct yasl_surface *s)
{
    const struct cocoa_surface *w = (const struct cocoa_surface *)s;
    return w ? w->preedit : NULL;
}

static bool impl_surface_ime_preedit_cursor(const struct yasl_surface *s, int *begin, int *end)
{
    return cocoa_surface_ime_preedit_cursor((const struct cocoa_surface *)s, begin, end);
}

static void impl_surface_set_ime_caret(struct yasl_surface *s, struct yasl_private_rect2i caret)
{
    cocoa_surface_set_ime_caret((struct cocoa_surface *)s, caret);
}

static bool impl_surface_start_interactive_move(struct yasl_surface *s)
{
    return cocoa_surface_start_interactive_move((struct cocoa_surface *)s);
}

static bool impl_surface_start_interactive_resize(struct yasl_surface *s, enum yasl_private_resize_edge edge)
{
    return cocoa_surface_start_interactive_resize((struct cocoa_surface *)s, edge);
}

static bool impl_surface_show_window_menu(struct yasl_surface *s, struct yasl_private_vector2i pos)
{
    return cocoa_surface_show_window_menu((struct cocoa_surface *)s, pos);
}

static bool impl_surface_drag_carry(struct yasl_surface *s, struct yasl_private_vector2i offset)
{
    return cocoa_surface_drag_carry((struct cocoa_surface *)s, offset);
}

static bool impl_surface_place_under_pointer(struct yasl_surface *s, struct yasl_private_vector2i at)
{
    return cocoa_surface_place_under_pointer((struct cocoa_surface *)s, at);
}

static void *impl_surface_get_metal_layer(struct yasl_surface *surface)
{
    struct cocoa_surface *w = (struct cocoa_surface *)surface;
    return w && w->metal_layer && (w->base.presenter == YASL_PRIVATE_PRESENTER_NONE ||
                 w->base.presenter == YASL_PRIVATE_PRESENTER_METAL) ? (void *)w->metal_layer : NULL;
}

static bool impl_surface_cpu_acquire(struct yasl_surface *s, struct yasl_private_framebuffer *out)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    if (!w || !out || w->destroyed || w->closed || !w->use_iosurface ||
        w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        errno = (!w || !out || w->destroyed || w->closed ||
                 w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) ? EINVAL : ENOTSUP;
        return false;
    }
    if (w->rendering || w->direct_acquired) {
        errno = EBUSY;
        return false;
    }
    if (w->size.x <= 0 || w->size.y <= 0) {
        errno = EAGAIN;
        return false;
    }
    @autoreleasepool {
        uint32_t *px = NULL;
        int stride = 0;
        int idx = cpu_acquire_slot(w, &px, &stride);
        if (idx < 0)
            return false;
        w->direct_slot = idx;
        w->direct_acquired = true;
        w->want_draw = false;
        *out = (struct yasl_private_framebuffer){
            .generation = yasl_private_frame_begin(&w->base),
            .pixels = px,
            .size = w->size,
            .stride_bytes = stride,
        };
        return true;
    }
}

static bool impl_surface_cpu_commit(struct yasl_surface *s, const struct yasl_private_framebuffer *fb,
                                    const struct yasl_private_damage_list *damage)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    if (!w) {
        errno = EINVAL;
        return false;
    }
    bool was_acquired = w->direct_acquired;
    int idx = w->direct_slot;
    w->direct_acquired = false;
    if (!was_acquired || w->destroyed || w->closed || !w->use_iosurface ||
        w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        if (was_acquired)
            cpu_unlock_slot(&w->cpu_slots[idx]);
        errno = EINVAL;
        return false;
    }
    struct cocoa_cpu_slot *slot = &w->cpu_slots[idx];
    void *addr = slot->surface ? IOSurfaceGetBaseAddress(slot->surface) : NULL;
    if (!fb || !damage || fb->pixels != addr ||
        fb->size.x != w->size.x || fb->size.y != w->size.y ||
        slot->size.x != w->size.x || slot->size.y != w->size.y) {
        cpu_unlock_slot(slot);
        errno = EINVAL;
        return false;
    }
    @autoreleasepool {
        cpu_commit_slot(w, idx);
    }
    return true;
}

static void impl_surface_cpu_discard(struct yasl_surface *s)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    if (!w || !w->direct_acquired)
        return;
    cpu_unlock_slot(&w->cpu_slots[w->direct_slot]);
    w->direct_acquired = false;
}

static bool impl_surface_update_text_input(struct yasl_surface *surface)
{
    struct cocoa_surface *w = (struct cocoa_surface *)surface;
    if (!w || w->closed || w->destroyed || !w->view) return false;
    if (w->base.text_input_active) {
        [w->window makeFirstResponder:w->view];
    } else {
        [w->view.inputContext discardMarkedText];
        if (w->preedit) [w->view unmarkText];
    }
    return true;
}

static bool impl_surface_read_pixels(struct yasl_surface *s, uint32_t *out, size_t cap,
                                     struct yasl_private_vector2i *size)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    if (w->use_iosurface) {
        if (w->closed || w->destroyed || w->rendering || w->direct_acquired || !w->has_shown)
            return false;
        struct cocoa_cpu_slot *slot = &w->cpu_slots[w->shown_slot];
        if (!slot->surface ||
            slot->size.x != w->shown_size.x || slot->size.y != w->shown_size.y)
            return false;
        if (IOSurfaceLock(slot->surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess)
            return false;
        const uint32_t *px = (const uint32_t *)IOSurfaceGetBaseAddress(slot->surface);
        bool ok = px &&
                  IOSurfaceGetBytesPerRow(slot->surface) == (size_t)w->shown_size.x * sizeof(uint32_t) &&
                  yasl_private_copy_frame(px, w->shown_size, out, cap, size);
        IOSurfaceUnlock(slot->surface, kIOSurfaceLockReadOnly, NULL);
        return ok;
    }
    /* did_present stays set from the last draw until the next one starts, and
     * pixels are reallocated only when a draw starts. */
    if (w->closed || w->rendering || !w->did_present || !w->software_queue || !w->pixels)
        return false;
    return yasl_private_copy_frame(w->pixels, w->shown_size, out, cap, size);
}

static bool impl_surface_bell(struct yasl_surface *s)
{
    assert([NSThread isMainThread]);
    (void)s;
    NSBeep();
    return true;
}

static void *impl_display_get_native_handle(struct yasl_private_display *d)
{
    return d ? (void *)NSApp : NULL;
}

static void *impl_surface_get_native_handle(struct yasl_surface *s)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    return w ? (void *)w->window : NULL;
}

static bool impl_surface_get_native_frame(struct yasl_surface *s,
        const struct yasl_private_framebuffer *target, struct yasl_private_native_frame *out)
{
    struct cocoa_surface *w = (struct cocoa_surface *)s;
    if (w->closed || !w->rendering || w->did_present || !w->drawable ||
        target->pixels != w->frame.target.pixels ||
        target->size.x != w->size.x || target->size.y != w->size.y)
        return false;
    *out = (struct yasl_private_native_frame){(void *)w->drawable, w->transaction};
    return true;
}

/* Cocoa keeps YASL_SURFACE_GL unsupported. The table has no surface_create_gl,
 * so yasl_surface_create returns -ENOTSUP for that type. OpenGL would need a
 * second context path through ANGLE over the Metal layer, and a third-party
 * dependency. Metal and Vulkan cover application-managed drawing on macOS. */
const struct yasl_private_backend_ops yasl_private_cocoa_backend = {
    .type = YASL_PRIVATE_BACKEND_COCOA,
    .surface_get_metal_layer = impl_surface_get_metal_layer,
    .surface_update_text_input = impl_surface_update_text_input,
    .display_open = impl_display_open,
    .display_close = impl_display_close,
    .display_caps = impl_display_caps,
    .display_prepare = impl_display_prepare,
    .display_wait = impl_display_wait,
    .display_check = impl_display_check,
    .display_flush = impl_display_flush,
    .display_modifiers = impl_display_modifiers,
    .display_double_click_ms = impl_display_double_click_ms,
    .display_copy_text = impl_display_copy_text,
    .display_request_paste = impl_display_request_paste,
    .display_drag_start = impl_display_drag_start,
    .display_drag_cancel = impl_display_drag_cancel,
    .surface_create_toplevel = impl_surface_create_toplevel,
    .surface_create_popup = impl_surface_create_popup,
    .surface_popup_reposition = impl_surface_popup_reposition,
    .surface_destroy = impl_surface_destroy,
    .surface_input_queue = impl_surface_input_queue,
    .surface_set_size = impl_surface_set_size,
    .surface_set_fullscreen = impl_surface_set_fullscreen,
    .surface_set_maximized = impl_surface_set_maximized,
    .surface_minimize = impl_surface_minimize,
    .surface_display = impl_surface_display,
    .surface_set_callbacks = impl_surface_set_callbacks,
    .surface_set_title = impl_surface_set_title,
    .surface_schedule_redraw = impl_surface_schedule_redraw,
    .surface_wake_frame_request = impl_surface_wake_frame_request,
    .surface_frame_in_flight = impl_surface_frame_in_flight,
    .surface_present = impl_surface_present,
    .surface_cpu_acquire = impl_surface_cpu_acquire,
    .surface_cpu_commit = impl_surface_cpu_commit,
    .surface_cpu_discard = impl_surface_cpu_discard,
    .surface_geometry = impl_surface_geometry,
    .surface_timing = impl_surface_timing,
    .surface_frame_time_ns = impl_surface_frame_time_ns,
    .surface_set_cursor = impl_surface_set_cursor,
    .surface_set_pointer_mode = impl_surface_set_pointer_mode,
    .surface_set_background_color = impl_surface_set_background_color,
    .surface_ime_preedit = impl_surface_ime_preedit,
    .surface_ime_preedit_cursor = impl_surface_ime_preedit_cursor,
    .surface_set_ime_caret = impl_surface_set_ime_caret,
    .surface_start_interactive_move = impl_surface_start_interactive_move,
    .surface_start_interactive_resize = impl_surface_start_interactive_resize,
    .surface_show_window_menu = impl_surface_show_window_menu,
    .surface_drag_carry = impl_surface_drag_carry,
    .surface_place_under_pointer = impl_surface_place_under_pointer,
    .surface_bell = impl_surface_bell,
    .surface_read_pixels = impl_surface_read_pixels,
    .display_get_native_handle = impl_display_get_native_handle,
    .surface_get_native_handle = impl_surface_get_native_handle,
    .surface_get_native_frame = impl_surface_get_native_frame,
};

NSApplication *yasl_private_cocoa_display_get_application(struct yasl_private_display *d)
{
    return (d && yasl_private_display_backend(d) == YASL_PRIVATE_BACKEND_COCOA) ?
        (NSApplication *)yasl_private_display_get_native_handle(d) : nil;
}

NSWindow *yasl_private_cocoa_surface_get_window(struct yasl_surface *s)
{
    return (s && s->display && yasl_private_display_backend(s->display) == YASL_PRIVATE_BACKEND_COCOA) ?
        (NSWindow *)yasl_private_surface_get_native_handle(s) : nil;
}

NSView *yasl_private_cocoa_surface_get_view(struct yasl_surface *s)
{
    YaslWindow *w = (YaslWindow *)yasl_private_cocoa_surface_get_window(s);
    return w.surface ? w.surface->view : nil;
}
