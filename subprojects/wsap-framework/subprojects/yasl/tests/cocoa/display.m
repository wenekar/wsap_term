#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <yasl/yasl.h>
#include <assert.h>

static BOOL in_live_resize(id view, SEL selector)
{
    (void)view;
    (void)selector;
    return YES;
}

/* Reproduce AppKit’s live-resize notifications and view state without moving
 * the system pointer. */
void yasl_test_live_resize(yasl_surface *surface, int width, int height)
{
    struct yasl_native native;
    assert(yasl_surface_get_native(surface, &native) == 0);
    NSWindow *window = (NSWindow *)native.ns_window;
    NSView *view = (NSView *)native.ns_view;
    Method method = class_getInstanceMethod([view class], @selector(inLiveResize));
    IMP saved = method_setImplementation(method, (IMP)in_live_resize);
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center postNotificationName:NSWindowWillStartLiveResizeNotification object:window];
    [window setContentSize:NSMakeSize(width, height)];
    [center postNotificationName:NSWindowDidEndLiveResizeNotification object:window];
    method_setImplementation(method, saved);
}
