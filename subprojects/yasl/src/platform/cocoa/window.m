#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

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
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>

#define LINK_IDLE_FRAMES 8
/* Application-defined event subtype that only wakes a waiting dispatch. */
#define LOOP_WAKE_SUBTYPE 0x5953
/* Wait bound while some loop descriptor cannot be watched with kqueue. */
#define LOOP_FALLBACK_MS 10
#define SCROLL_POINTS_PER_LINE 10.0

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
@end

@interface YaslAppDelegate : NSObject <NSApplicationDelegate>
@end

@interface YaslWindow : NSWindow
@property (nonatomic, assign) struct cocoa_surface *surface;
@end

@interface YaslWindowController : NSWindowController <NSWindowDelegate>
@property (nonatomic, assign) struct cocoa_surface *surface;
@property (nonatomic, assign) BOOL fullscreenTransition;
@end

#include "common/input.h"

struct cocoa_surface {
    struct yasl_surface base;
    YaslWindow *window;
    YaslWindowController *controller;
    YaslView *view;
    CAMetalLayer *layer;
    id<MTLCommandQueue> software_queue;
    id<MTLBuffer> software_buffer;
    NSUInteger software_buffer_capacity;
    CADisplayLink *link;

    struct cocoa_surface *parent;
    bool is_popup;
    struct yasl_private_vector2i content_offset;

    const struct yasl_private_window_cbs *cbs;
    void *ud;
    uint64_t frames_skipped;

    char *preedit;
    size_t preedit_begin, preedit_end;
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
    bool want_draw;
    bool frame_due;
    bool unpaced;
    bool rendering;
    bool closed;
    bool destroyed;
    struct cocoa_surface *dead_next;

    struct input_queue input;
    struct yasl_private_vector2i pointer;
    double scroll_x;
    uint32_t mods;
    uint32_t buttons;
    enum yasl_private_cursor cursor;
    enum yasl_private_pointer_device tablet_device;
    bool tablet_down;
    uint64_t pen_device_id, pen_tool_id;
    uint32_t pen_buttons;

    /* Retained mouse-down of the press in progress, for interactive moves. */
    NSEvent *mouse_down;
    struct yasl_private_frame_clock clock;
};

struct cocoa_display {
    struct yasl_private_display base;
    struct cocoa_surface **windows;
    int count;
    int capacity;
    char *app_id;
    struct yasl_loop *loop;
    id key_up_monitor;
    NSCursor *hidden_cursor;
    YaslAppDelegate *delegate;
    NSPoint cascade;
    struct cocoa_surface *dead;
    struct {
        bool active;
        struct cocoa_surface *origin;
        struct cocoa_surface *carried;
        struct yasl_private_vector2i carry_offset;
        struct cocoa_surface *hover;
        struct yasl_private_vector2i hover_pos;
        struct yasl_private_drag_listener listener;
        void *ud;
        NSImage *image;
        NSWindow *image_window;
        struct yasl_private_vector2i image_size;
        struct yasl_private_vector2i image_hotspot;
    } drag;
    int depth;
    bool open;
    bool pumping;
    bool owns_loop;
};

static struct cocoa_display darwin_ctx;

static void drag_end(struct cocoa_display *d, enum yasl_private_drag_end how)
{
    if (!d || !d->drag.active) return;
    struct yasl_private_drag_listener l = d->drag.listener;
    void *ud = d->drag.ud;
    struct cocoa_surface *hover = d->drag.hover;
    struct yasl_private_vector2i pos = d->drag.hover_pos;

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
static void reap_dead(struct cocoa_display *d);
static void push_pen(struct cocoa_surface *w, NSEvent *event, unsigned action, bool contact);

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
    if (!focused)
        check_pointer_reset(w);
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_FOCUS,
        .surface = &w->base,
        .focus = { .focused = focused },
    };
    input_queue_push(&w->input, &ev);
    darwin_ctx.depth++;
    set_state(w, YASL_PRIVATE_STATE_ACTIVATED, focused);
    if (!w->destroyed && !w->closed)
        cocoa_surface_schedule_redraw(w);
    darwin_ctx.depth--;
}

static void sync_size(struct cocoa_surface *w, bool force)
{
    if (!w || w->destroyed || w->closed || !w->view || !w->window)
        return;
    uint64_t start = yasl_private_loop_now_ns();
    NSRect bounds = w->view.bounds;
    CGFloat scale = w->window.backingScaleFactor;
    if (scale < 1.0)
        scale = 1.0;
    struct yasl_private_vector2i size = {
        .x = (int)lround(bounds.size.width * scale),
        .y = (int)lround(bounds.size.height * scale),
    };
    uint32_t scale_120 = (uint32_t)lround(scale * YASL_PRIVATE_SCALE_UNIT);
    bool rescaled = force || scale_120 != w->scale_120;
    bool resized = force || (size.x != w->size.x || size.y != w->size.y);
    if (!rescaled && !resized)
        return;
    darwin_ctx.depth++;
    w->size = size;
    w->scale_120 = scale_120;
    if (w->layer) {
        w->layer.contentsScale = scale;
        w->layer.drawableSize = CGSizeMake(size.x, size.y);
    }
    /* Finish local bookkeeping before publishing geometry and requesting a frame. */
    yasl_private_clock_note_resize(&w->clock, yasl_private_loop_now_ns() - start);
    yasl_private_surface_notify_configure(&w->base, (struct yasl_private_geometry){
            .size = size, .scale_120 = scale_120,
        });
    /* During a live resize the app draws the new size here, before the frame is
     * rendered and committed below. */
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU || w->view.inLiveResize)
        yasl_private_surface_notify_refresh(&w->base);
    if (!w->destroyed && !w->closed && w->view.inLiveResize)
        render_and_commit(w);
    darwin_ctx.depth--;
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
    };
    /* characters properties are invalid on flagsChanged events. */
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
    CGFloat scale = w->window.backingScaleFactor;
    if (scale < 1.0)
        scale = 1.0;
    return (struct yasl_private_vector2i){
        .x = (int)floor(p.x * scale),
        .y = (int)floor((w->view.bounds.size.height - p.y) * scale),
    };
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
                    CGFloat scale = candidate->window.backingScaleFactor;
                    if (scale < 1.0) scale = 1.0;
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
                        CGFloat scale = s->window.backingScaleFactor;
                        if (scale < 1.0) scale = 1.0;
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
            CGFloat scale = w->window.backingScaleFactor;
            if (scale < 1.0) scale = 1.0;
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
    if (type != YASL_PRIVATE_EVENT_POINTER_LEAVE)
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
            cocoa_surface_place_under_pointer(darwin_ctx.drag.carried, darwin_ctx.drag.carry_offset);
        } else if (darwin_ctx.drag.image_window) {
            CGFloat scale = darwin_ctx.drag.origin && darwin_ctx.drag.origin->window
                ? darwin_ctx.drag.origin->window.backingScaleFactor : 1.0;
            if (scale < 1.0) scale = 1.0;
            NSPoint mouse = [NSEvent mouseLocation];
            CGFloat pt_h = darwin_ctx.drag.image_size.y / scale;
            [darwin_ctx.drag.image_window setFrameOrigin:NSMakePoint(
                mouse.x - darwin_ctx.drag.image_hotspot.x / scale,
                mouse.y - pt_h + darwin_ctx.drag.image_hotspot.y / scale)];
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

static NSCursor *cocoa_cursor(enum yasl_private_cursor cursor)
{
    switch (cursor) {
    case YASL_PRIVATE_CURSOR_TEXT: return [NSCursor IBeamCursor];
    case YASL_PRIVATE_CURSOR_LINK: return [NSCursor pointingHandCursor];
    case YASL_PRIVATE_CURSOR_CROSSHAIR: return [NSCursor crosshairCursor];
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
@end

static struct yasl_private_vector2i dragging_pos(YaslView *view, id<NSDraggingInfo> sender)
{
    NSPoint p = [view convertPoint:[sender draggingLocation] fromView:nil];
    NSWindow *win = view.window;
    CGFloat scale = win ? win.backingScaleFactor : 1.0;
    if (scale < 1.0) scale = 1.0;
    return (struct yasl_private_vector2i){
        .x = (int)floor(p.x * scale),
        .y = (int)floor((view.bounds.size.height - p.y) * scale),
    };
}

@implementation YaslView

- (instancetype)initWithFrame:(NSRect)frameRect
{
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        self.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    }
    return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([pb canReadItemWithDataConformingToTypes:@[NSPasteboardTypeFileURL]]) {
        struct cocoa_surface *w = self.surface;
        if (w && !w->closed) {
            struct yasl_private_vector2i pos = dragging_pos(self, sender);
            darwin_ctx.depth++;
            yasl_private_surface_notify_drop(&w->base, YASL_PRIVATE_DROP_OVER, pos);
            darwin_ctx.depth--;
        }
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    return [self draggingEntered:sender];
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([pb canReadItemWithDataConformingToTypes:@[NSPasteboardTypeFileURL]]) {
        struct cocoa_surface *w = self.surface;
        if (w && !w->closed) {
            struct yasl_private_vector2i pos = dragging_pos(self, sender);
            darwin_ctx.depth++;
            yasl_private_surface_notify_drop(&w->base, YASL_PRIVATE_DROP_LEAVE, pos);
            darwin_ctx.depth--;
        }
    }
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    struct cocoa_surface *w = self.surface;
    if (!w) return NO;
    struct yasl_private_vector2i pos = dragging_pos(self, sender);
    BOOL ok = NO;

    NSPasteboard *pb = [sender draggingPasteboard];
    NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count) {
        NSMutableString *result = [NSMutableString string];
        for (NSURL *url in urls) {
            [result appendString:url.absoluteString];
            [result appendString:@"\r\n"];
        }
        const char *utf8 = result.UTF8String;
        if (utf8 && strlen(utf8)) {
            struct yasl_private_event ev = {
                .type = YASL_PRIVATE_EVENT_DROP,
                .surface = &w->base,
                .text = { .data = utf8, .len = strlen(utf8) },
            };
            input_queue_push(&w->input, &ev);
            cocoa_surface_schedule_redraw(w);
            ok = YES;
        }
    }
    if (!w->destroyed) {
        darwin_ctx.depth++;
        yasl_private_surface_notify_drop(&w->base, YASL_PRIVATE_DROP_LEAVE, pos);
        darwin_ctx.depth--;
    }
    return ok;
}

- (CALayer *)makeBackingLayer
{
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
    sync_size(self.surface, false);
}

- (void)viewWillStartLiveResize
{
    [super viewWillStartLiveResize];
    struct cocoa_surface *w = self.surface;
    /* Layers that the app owns through Metal or Vulkan keep their own setting. */
    if (w && w->base.presenter == YASL_PRIVATE_PRESENTER_CPU)
        w->layer.presentsWithTransaction = YES;
}

- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    struct cocoa_surface *w = self.surface;
    if (!w)
        return;
    if (w->base.presenter == YASL_PRIVATE_PRESENTER_CPU)
        w->layer.presentsWithTransaction = NO;
    cocoa_surface_schedule_redraw(w);
}

- (void)displayLinkFired:(CADisplayLink *)link
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->closed)
        return;
    CFTimeInterval period = link.targetTimestamp - link.timestamp;
    if (period > 0.0 && period < 0.25)
        w->clock.refresh_ns = period * 1e9;
    w->frame_due = true;
    if (!w->want_draw || (w->base.state & YASL_PRIVATE_STATE_SUSPENDED)) {
        if (++w->idle_frames >= LINK_IDLE_FRAMES)
            link.paused = YES;
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
    [cocoa_cursor(w ? w->cursor : YASL_PRIVATE_CURSOR_ARROW) set];
}

- (void)keyDown:(NSEvent *)event
{
    struct cocoa_surface *w = self.surface;
    if (!w) return;
    on_key_event(w, event, event.isARepeat ? YASL_KEY_REPEAT : YASL_KEY_PRESS);
    [self interpretKeyEvents:@[event]];
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

- (void)mouseDown:(NSEvent *)event
{
    [self.window makeFirstResponder:self];
    /* Right and other buttons also arrive here. Keep the press for interactive moves. */
    struct cocoa_surface *w = self.surface;
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
    push_pointer(self.surface, event, YASL_PRIVATE_EVENT_POINTER_MOTION, 0);
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
    if (event.hasPreciseScrollingDeltas) {
        dx /= SCROLL_POINTS_PER_LINE;
        dy /= SCROLL_POINTS_PER_LINE;
    }
    if (!isfinite(dx) || !isfinite(dy) || (dx == 0 && dy == 0)) return;
    w->mods = cocoa_mods(event.modifierFlags);
    w->pointer = event_pos(w, event);
    struct yasl_private_event input_ev = {
        .type = YASL_PRIVATE_EVENT_POINTER_SCROLL,
        .surface = &w->base,
        .pointer = {
            .pos = w->pointer,
            .mods = w->mods,
            .scroll = {dx, dy},
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
                           size_t begin, size_t end)
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
        },
    };
    input_queue_push(&w->input, &ev);
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->destroyed || w->closed || !w->base.text_input_active) return;
    const char *utf8 = plain_string(string).UTF8String;
    size_t len = utf8 ? strlen(utf8) : 0;
    size_t begin, end;
    if (!text_edit_range(w, replacementRange, true, &begin, &end)) return;
    bool had_preedit = w->preedit != NULL;
    darwin_ctx.depth++;
    push_text_edit(w, utf8, len, begin, end);
    free(w->preedit);
    w->preedit = NULL;
    w->preedit_begin = w->preedit_end = 0;
    if (had_preedit)
        yasl_private_surface_notify_composition(&w->base, NULL, -1, -1);
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
        push_text_edit(w, "", 0, byte_begin, byte_end);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange
{
    struct cocoa_surface *w = self.surface;
    if (!w || w->destroyed || w->closed || !w->base.text_input_active) return;
    NSString *str = plain_string(string);
    size_t range_begin, range_end;
    if (!text_edit_range(w, replacementRange, true, &range_begin, &range_end)) return;
    w->preedit_begin = range_begin;
    w->preedit_end = range_end;
    w->preedit_revision = w->base.text_revision;
    w->preedit_generation = w->base.text_generation;
    if (!text_utf8_to_utf16(w->base.text_surrounding ?: "", range_begin,
                            &w->marked_utf16_location))
        w->marked_utf16_location = 0;
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
    yasl_private_surface_notify_composition(&w->base, w->preedit, begin, end);
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
    darwin_ctx.depth++;
    yasl_private_surface_notify_composition(&w->base, NULL, -1, -1);
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
    CGFloat scale = w->window.backingScaleFactor;
    if (scale < 1.0) scale = 1.0;
    NSRect r = NSMakeRect(w->ime_caret.x / scale,
        self.bounds.size.height - (w->ime_caret.y + w->ime_caret.height) / scale,
        w->ime_caret.width / scale, w->ime_caret.height / scale);
    return [w->window convertRectToScreen:[self convertRect:r toView:nil]];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return 0; }

@end

@implementation YaslWindow

- (void)sendEvent:(NSEvent *)event
{
    darwin_ctx.depth++;
    @try {
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

@implementation YaslWindowController

- (void)windowDidBecomeKey:(NSNotification *)notification
{
    (void)notification;
    if (self.surface) push_focus(self.surface, true);
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
    darwin_ctx.depth++;
    set_state(w, YASL_PRIVATE_STATE_SUSPENDED, !visible);
    if (!w->destroyed && !w->closed) {
        if (visible)
            cocoa_surface_schedule_redraw(w);
        else
            w->link.paused = YES;
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
    sync_size(w, false);
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

- (void)windowDidChangeScreen:(NSNotification *)notification
{
    (void)notification;
    sync_size(self.surface, false);
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
        fb->pixels != w->frame.target.pixels || fb->size.x != w->size.x ||
        fb->size.y != w->size.y || !w->drawable)
        return false;
    if (w->software_queue) {
        if (fb->pixels != w->pixels || fb->size.x != w->size.x || fb->size.y != w->size.y)
            return false;
        NSUInteger row_bytes = ((NSUInteger)w->size.x * 4 + 255) & ~(NSUInteger)255;
        NSUInteger length = row_bytes * (NSUInteger)w->size.y;
        if (!w->software_buffer || w->software_buffer_capacity < length) {
            [w->software_buffer release];
            w->software_buffer = [w->layer.device newBufferWithLength:length
                                                    options:MTLResourceStorageModeShared];
            w->software_buffer_capacity = w->software_buffer ? length : 0;
        }
        id<MTLBuffer> buffer = w->software_buffer;
        if (!buffer) return false;
        for (int y = 0; y < w->size.y; y++)
            memcpy((char *)buffer.contents + (NSUInteger)y * row_bytes,
                   w->pixels + (size_t)y * w->size.x, (size_t)w->size.x * 4);
        id<MTLCommandBuffer> command = [w->software_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!blit) return false;
        id<CAMetalDrawable> drawable = w->drawable;
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:row_bytes
           sourceBytesPerImage:length sourceSize:MTLSizeMake(w->size.x, w->size.y, 1)
                    toTexture:drawable.texture destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [command presentDrawable:drawable];
        [command commit];
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

    size_t count = (size_t)w->size.x * (size_t)w->size.y;
    if (w->software_queue && count > w->pixel_capacity) {
        uint32_t *pixels = realloc(w->pixels, count * sizeof(*pixels));
        if (!pixels) return;
        w->pixels = pixels;
        w->pixel_capacity = count;
    }
    darwin_ctx.depth++;
    w->rendering = true;
    w->want_draw = false;
    w->frame_due = false;
    w->did_present = false;
    id<CAMetalDrawable> drawable = [w->layer nextDrawable];
    w->drawable = drawable;
    w->transaction = w->layer.presentsWithTransaction;
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
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        yasl_private_surface_notify_refresh(&w->base);
        return;
    }
    w->want_draw = true;
    w->idle_frames = 0;
    if (w->link.paused) {
        w->frame_due = true;
        w->link.paused = NO;
    }
    yasl_private_loop_wake(darwin_ctx.loop);
}

static void app_init(const char *app_name)
{
    /* finishLaunching runs once per process, even when the display reopens. */
    static bool finished_launching;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow.allowsAutomaticWindowTabbing = NO;
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

    darwin_ctx.key_up_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp
        handler:^NSEvent *(NSEvent *event) {
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
    if (d->key_up_monitor) {
        [NSEvent removeMonitor:d->key_up_monitor];
        d->key_up_monitor = nil;
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
    free(d->app_id);
    free(d->windows);
    *d = (struct cocoa_display){0};
}

uint32_t cocoa_display_caps(const struct cocoa_display *d)
{
    return d ? (YASL_PRIVATE_CAP_POPUPS | YASL_PRIVATE_CAP_DRAG | YASL_PRIVATE_CAP_DRAG_CARRY |
                YASL_PRIVATE_CAP_PLACE_WINDOW | YASL_PRIVATE_CAP_PRIMARY_SELECTION |
                YASL_PRIVATE_CAP_CSD | YASL_PRIVATE_CAP_NATIVE_GRAPHICS |
                YASL_PRIVATE_CAP_FRAME_PACING | YASL_PRIVATE_CAP_TABLET |
                YASL_PRIVATE_CAP_TEXT_INPUT) : 0;
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
        w->window = [[YaslWindow alloc] initWithContentRect:NSMakeRect(0, 0, win_w, win_h)
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        w->window.releasedWhenClosed = NO;
        w->base.native_handle = (__bridge void *)w->window;
        w->base.size = w->size;
        w->base.scale_120 = w->scale_120;
        w->controller = [[YaslWindowController alloc] initWithWindow:w->window];
        w->controller.surface = w;
        w->window.delegate = w->controller;
        w->window.acceptsMouseMovedEvents = YES;
        w->window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        w->window.titlebarAppearsTransparent = YES;
        if ([w->window respondsToSelector:@selector(setTitlebarSeparatorStyle:)])
            w->window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        /* The full title: a byte-limited copy could split a UTF-8 sequence. */
        w->window.title = [NSString stringWithUTF8String:t] ?: @"YASL";
        w->window.colorSpace = [NSColorSpace sRGBColorSpace];

        w->view = [[YaslView alloc] initWithFrame:w->window.contentView.bounds];
        w->window.contentView = w->view;
        w->layer = (CAMetalLayer *)w->view.layer;
        if (graphics_device) {
            w->layer.device = (id<MTLDevice>)graphics_device;
        } else if (!external_gpu) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            w->layer.device = device;
            w->software_queue = [device newCommandQueue];
            [device release];
            if (!w->software_queue) {
                w->window.delegate = nil;
                w->controller.surface = NULL;
                [w->window close];
                [w->view release];
                [w->controller release];
                [w->window release];
                free(w);
                return NULL;
            }
            w->layer.framebufferOnly = NO;
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
        if (spec->tearing) {
            if (w->layer) w->layer.displaySyncEnabled = NO;
            w->unpaced = true;
        }
        if (spec->fullscreen)
            [w->window toggleFullScreen:nil];
        return w;
    }
}

static void surface_free(struct cocoa_surface *w)
{
    [w->mouse_down release];
    [w->link release];
    [w->software_buffer release];
    [w->software_queue release];
    [w->view release];
    [w->controller release];
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
    w->controller.surface = NULL;
    w->window.surface = NULL;
    w->view.surface = NULL;
    if (!native_closed)
        [w->window close];
    input_queue_clear(&w->input);
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
        [cocoa_cursor(cursor) set];
}

bool cocoa_surface_place_under_pointer(struct cocoa_surface *w, struct yasl_private_vector2i at)
{
    if (!w || w->closed || !w->window) return false;
    CGFloat scale = w->window.backingScaleFactor;
    if (scale < 1.0) scale = 1.0;
    NSPoint mouse = [NSEvent mouseLocation];
    NSRect content = [w->window contentRectForFrameRect:w->window.frame];
    NSPoint origin = NSMakePoint(mouse.x - at.x / scale,
                                 mouse.y - content.size.height + at.y / scale);
    [w->window setFrameOrigin:[w->window frameRectForContentRect:
        (NSRect){ .origin = origin, .size = content.size }].origin];
    return true;
}

void cocoa_surface_set_ime_caret(struct cocoa_surface *w, struct yasl_private_rect2i caret)
{
    if (w) w->ime_caret = caret;
}

bool cocoa_surface_ime_preedit_cursor(const struct cocoa_surface *w, int *begin, int *end)
{
    (void)w; (void)begin; (void)end;
    return false;
}

static NSPasteboard *pasteboard_for_selection(enum yasl_private_selection sel)
{
    switch (sel) {
    case YASL_PRIVATE_CLIPBOARD:
        return [NSPasteboard generalPasteboard];
    case YASL_PRIVATE_PRIMARY:
        return [NSPasteboard pasteboardWithName:@"dev.wsap.yasl.primary"];
    default:
        return nil;
    }
}

static bool cocoa_display_copy_text(struct cocoa_display *d, enum yasl_private_selection sel, const char *text);

static bool cocoa_display_copy_text(struct cocoa_display *d, enum yasl_private_selection sel, const char *text)
{
    assert([NSThread isMainThread]);
    if (!d || !text) { errno = EINVAL; return false; }
    NSPasteboard *pb = pasteboard_for_selection(sel);
    if (!pb) { errno = EINVAL; return false; }
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
    if (!pb) { errno = EINVAL; return false; }
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
        const struct yasl_private_popup_spec *spec, unsigned anchor, unsigned gravity)
{
    CGFloat scale = parent->window.backingScaleFactor;
    if (scale < 1.0) scale = 1.0;
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
    CGFloat top_left_x = content_left - offset_x;
    CGFloat top_left_y = content_top - offset_y;

    CGFloat view_h = parent->view.bounds.size.height;
    NSPoint in_view = NSMakePoint(top_left_x, view_h - top_left_y);
    NSPoint in_window = [parent->view convertPoint:in_view toView:nil];
    NSPoint screen_top_left = [parent->window convertPointToScreen:in_window];
    CGFloat width = spec->size.x / scale;
    CGFloat height = spec->size.y / scale;
    return NSMakeRect(screen_top_left.x, screen_top_left.y - height, width, height);
}

static NSRect popup_calc_frame(struct cocoa_surface *parent, const struct yasl_private_popup_spec *spec)
{

    NSRect frame = popup_frame_explicit(parent, spec, spec->anchor_edges, spec->gravity);
    NSScreen *screen = parent->window.screen ?: [NSScreen mainScreen];
    if (!screen) return frame;
    NSRect visible = screen.visibleFrame;
    unsigned constraints = spec->constraints;

    if (constraints & POPUP_CONSTRAINT_FLIP_X) {
        NSRect flipped = popup_frame_explicit(parent, spec,
            popup_flip_x_edge(spec->anchor_edges), popup_flip_x_edge(spec->gravity));
        if (popup_axis_overflow(flipped.origin.x, NSMaxX(flipped), NSMinX(visible), NSMaxX(visible)) <
            popup_axis_overflow(frame.origin.x, NSMaxX(frame), NSMinX(visible), NSMaxX(visible)))
            frame = NSMakeRect(flipped.origin.x, frame.origin.y, flipped.size.width, frame.size.height);
    }
    if (constraints & POPUP_CONSTRAINT_FLIP_Y) {
        NSRect flipped = popup_frame_explicit(parent, spec,
            popup_flip_y_edge(spec->anchor_edges), popup_flip_y_edge(spec->gravity));
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
        cocoa_surface_set_callbacks(w, NULL, NULL);
        w->clock.refresh_ns = parent->clock.refresh_ns ? parent->clock.refresh_ns : YASL_PRIVATE_REFRESH_DEFAULT_NS;
        w->parent = parent;
        w->is_popup = true;
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
        w->controller = [[YaslWindowController alloc] initWithWindow:w->window];
        w->controller.surface = w;
        w->window.delegate = w->controller;
        w->window.acceptsMouseMovedEvents = YES;
        w->window.colorSpace = [NSColorSpace sRGBColorSpace];

        w->view = [[YaslView alloc] initWithFrame:w->window.contentView.bounds];
        w->window.contentView = w->view;
        w->layer = (CAMetalLayer *)w->view.layer;
        w->layer.opaque = NO;
        w->layer.device = parent->layer.device;
        /* A CPU popup needs the same software queue as a CPU toplevel, even when
         * the parent presents through Metal. */
        w->software_queue = [w->layer.device newCommandQueue];
        if (!w->software_queue) {
            w->window.delegate = nil;
            w->controller.surface = NULL;
            [w->view release];
            [w->controller release];
            [w->window release];
            free(w);
            return NULL;
        }
        w->layer.framebufferOnly = NO;

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
        d->drag.image_size = image->size;
        d->drag.image_hotspot = image->hotspot;
        if (d->drag.image) {
            CGFloat scale = origin->window ? origin->window.backingScaleFactor : 1.0;
            if (scale < 1.0) scale = 1.0;
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
            [win setFrameOrigin:NSMakePoint(mouse.x - image->hotspot.x / scale,
                                            mouse.y - pt_h + image->hotspot.y / scale)];
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
    darwin_ctx.drag.carry_offset = offset;
    return cocoa_surface_place_under_pointer(w, offset);
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
    return w && (w->base.presenter == YASL_PRIVATE_PRESENTER_NONE ||
                 w->base.presenter == YASL_PRIVATE_PRESENTER_METAL) ? (void *)w->layer : NULL;
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
    .surface_present = impl_surface_present,
    .surface_geometry = impl_surface_geometry,
    .surface_timing = impl_surface_timing,
    .surface_frame_time_ns = impl_surface_frame_time_ns,
    .surface_set_cursor = impl_surface_set_cursor,
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
    NSWindow *w = yasl_private_cocoa_surface_get_window(s);
    return w ? w.contentView : nil;
}
