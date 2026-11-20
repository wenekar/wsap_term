#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <yasl/yasl.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>

static NSPoint mouse;

static NSPoint test_mouse_location(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return mouse;
}

static NSUInteger test_pressed_buttons(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return 1;
}

static struct yasl_native native_of(yasl_surface *s)
{
    struct yasl_native native = {0};
    assert(yasl_surface_get_native(s, &native) == 0);
    assert(native.ns_window && native.ns_view);
    return native;
}

static NSPoint center(NSScreen *screen)
{
    return NSMakePoint(NSMidX(screen.visibleFrame), NSMidY(screen.visibleFrame));
}

static NSPoint content_point(struct yasl_native native, CGFloat x, CGFloat y)
{
    NSWindow *window = (NSWindow *)native.ns_window;
    NSView *view = (NSView *)native.ns_view;
    NSPoint point = [view convertPoint:NSMakePoint(x, NSHeight(view.bounds) - y) toView:nil];
    return [window convertPointToScreen:point];
}

static void assert_point(NSPoint actual, NSPoint expected)
{
    if (fabs(actual.x - expected.x) > 1 || fabs(actual.y - expected.y) > 1) {
        fprintf(stderr, "Expected screen point (%g, %g), got (%g, %g)\n",
                expected.x, expected.y, actual.x, actual.y);
        abort();
    }
}

static void test_drag(yasl_surface *origin, yasl_surface *carried,
                      NSScreen *first, NSScreen *second)
{
    struct yasl_native source = native_of(origin);
    struct yasl_native target = native_of(carried);
    NSWindow *window = (NSWindow *)target.ns_window;
    mouse = center(first);
    [window setFrameOrigin:mouse];
    assert(yasl_surface_get_info(carried).scale == first.backingScaleFactor);

    /* Send drag events to the test window. Do not move the system pointer. */
    Method location = class_getClassMethod([NSEvent class], @selector(mouseLocation));
    Method buttons = class_getClassMethod([NSEvent class], @selector(pressedMouseButtons));
    IMP saved_location = method_setImplementation(location, (IMP)test_mouse_location);
    IMP saved_buttons = method_setImplementation(buttons, (IMP)test_pressed_buttons);
    assert(yasl_drag_start(origin, NULL) == 0);
    assert(yasl_drag_carry(carried, 64, 24) == 0);
    assert_point(content_point(target, 64, 24), mouse);

    NSEvent *motion = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
        location:NSMakePoint(40, 40) modifierFlags:0 timestamp:0
        windowNumber:[(NSWindow *)source.ns_window windowNumber] context:nil
        eventNumber:0 clickCount:1 pressure:1];
    mouse = center(second);
    [(NSView *)source.ns_view mouseDragged:motion];
    assert(yasl_surface_get_info(carried).scale == second.backingScaleFactor);
    mouse.x += 8;
    [(NSView *)source.ns_view mouseDragged:motion];
    assert_point(content_point(target, 64, 24), mouse);
    assert(yasl_drag_cancel() == 0);
    method_setImplementation(buttons, saved_buttons);
    method_setImplementation(location, saved_location);
}

/* Find a point across the seam while most of the parent stays on its screen. */
static bool popup_position(NSScreen *major, NSScreen *minor, NSPoint *seam,
                           NSPoint *direction)
{
    NSRect a = major.frame, b = minor.frame;
    NSRect va = major.visibleFrame, vb = minor.visibleFrame;
    CGFloat y0 = MAX(NSMinY(va), NSMinY(vb)), y1 = MIN(NSMaxY(va), NSMaxY(vb));
    CGFloat x0 = MAX(NSMinX(va), NSMinX(vb)), x1 = MIN(NSMaxX(va), NSMaxX(vb));
    if (y1 - y0 >= 300 && (NSMaxX(a) == NSMinX(b) || NSMinX(a) == NSMaxX(b))) {
        bool right = NSMaxX(a) == NSMinX(b);
        *seam = NSMakePoint(right ? NSMaxX(a) : NSMinX(a), (y0 + y1) / 2);
        *direction = NSMakePoint(right ? 1 : -1, 0);
        return true;
    }
    if (x1 - x0 >= 300 && (NSMaxY(a) == NSMinY(b) || NSMinY(a) == NSMaxY(b))) {
        bool above = NSMaxY(a) == NSMinY(b);
        *seam = NSMakePoint((x0 + x1) / 2, above ? NSMaxY(a) : NSMinY(a));
        *direction = NSMakePoint(0, above ? 1 : -1);
        return true;
    }
    return false;
}

static void test_popup(yasl_surface *parent, NSScreen *major, NSScreen *minor)
{
    NSPoint seam, direction;
    if (!popup_position(major, minor, &seam, &direction)) {
        puts("SKIP: need adjacent screens for popup placement");
        return;
    }
    struct yasl_native native = native_of(parent);
    NSWindow *window = (NSWindow *)native.ns_window;
    NSView *view = (NSView *)native.ns_view;
    NSRect content = NSMakeRect(seam.x - 180 - 130 * direction.x,
                               seam.y - 140 - 100 * direction.y, 360, 280);
    [window setFrameOrigin:center(major)];
    NSWindowStyleMask style = window.styleMask & ~NSWindowStyleMaskFullSizeContentView;
    [window setFrame:[NSWindow frameRectForContentRect:content styleMask:style] display:NO];
    if (!NSEqualRects(window.screen.frame, major.frame)) {
        fprintf(stderr, "Parent %s on %s, expected %s\n",
                NSStringFromRect(window.frame).UTF8String,
                window.screen.localizedName.UTF8String, major.localizedName.UTF8String);
        abort();
    }
    NSPoint expected = NSMakePoint(seam.x + 35 * direction.x, seam.y + 30 * direction.y);
    NSPoint anchor = [view convertPoint:[window convertPointFromScreen:expected] fromView:nil];
    assert(NSPointInRect(anchor, view.bounds));
    assert(NSPointInRect(expected, minor.visibleFrame));
    yasl_surface *popup = yasl_popup_create(parent, &(struct yasl_popup_options){
        .width = 24, .height = 20, .x = anchor.x, .y = NSHeight(view.bounds) - anchor.y,
        .anchor = YASL_EDGE_TOP | YASL_EDGE_LEFT,
        .gravity = YASL_EDGE_BOTTOM | YASL_EDGE_RIGHT,
    });
    assert(popup);
    struct yasl_native menu = native_of(popup);
    assert_point(content_point(menu, 0, 0), expected);
    struct yasl_surface_info info = yasl_surface_get_info(popup);
    assert(info.scale == major.backingScaleFactor);
    assert(info.pixel_width == 24 * info.scale);
    assert(info.pixel_height == 20 * info.scale);

    /* Popup input and its framebuffer must use the same scale. */
    NSWindow *popup_window = (NSWindow *)menu.ns_window;
    NSPoint click = [popup_window convertPointFromScreen:content_point(menu, 8, 6)];
    NSEvent *press = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
        location:click modifierFlags:0 timestamp:0 windowNumber:popup_window.windowNumber
        context:nil eventNumber:0 clickCount:1 pressure:1];
    [(NSView *)menu.ns_view mouseDown:press];
    yasl_event event;
    bool found = false;
    for (int i = 0; i < 100 && yasl_wait_event(&event, 20) > 0; i++) {
        if (event.surface != popup || event.type != YASL_EVENT_POINTER ||
            event.action != YASL_POINTER_DOWN)
            continue;
        assert(event.x == 8 && event.y == 6);
        found = true;
        break;
    }
    assert(found);

    [window setFrameOrigin:center(minor)];
    info = yasl_surface_get_info(popup);
    assert(info.scale == minor.backingScaleFactor);
    assert(info.pixel_width == 24 * info.scale);
    assert(info.pixel_height == 20 * info.scale);
    assert(yasl_surface_destroy(popup) == 0);
}

int main(void)
{
    @autoreleasepool {
        NSScreen *normal = nil, *retina = nil;
        for (NSScreen *screen in NSScreen.screens) {
            if (screen.backingScaleFactor == 1.0) normal = screen;
            if (screen.backingScaleFactor == 2.0) retina = screen;
        }
        if (!normal || !retina) {
            puts("SKIP: need a 1x and a 2x display");
            return 77;
        }
        struct yasl_surface_options spec = {.title = "YASL scale test", .width = 320, .height = 200};
        yasl_surface *origin = yasl_surface_create(&spec);
        yasl_surface *carried = yasl_surface_create(&spec);
        assert(origin && carried);
        test_drag(origin, carried, normal, retina);
        test_drag(origin, carried, retina, normal);
        test_popup(origin, normal, retina);
        test_popup(origin, retina, normal);
        assert(yasl_shutdown() == 0);
    }
    return 0;
}
