#import <AppKit/AppKit.h>
#include <common/private_native.h>
#include <assert.h>
#include <math.h>

/* A motion event with device deltas, which a synthetic NSEvent cannot carry. */
@interface MotionSample : NSObject
@property NSEventType type;
@property NSEventModifierFlags modifierFlags;
@property NSEventSubtype subtype;
@property NSPoint locationInWindow;
@property CGFloat deltaX;
@property CGFloat deltaY;
@end

@implementation MotionSample
@end

static struct yasl_private_event next_motion(struct yasl_surface *surface)
{
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(yasl_private_surface_display(surface), &event))
        if (event.type == YASL_PRIVATE_EVENT_POINTER_MOTION)
            return event;
    assert(0 && "no pointer motion event");
    return event;
}

static void assert_transparent(NSCursor *cursor)
{
    assert(cursor);
    NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:cursor.image.TIFFRepresentation];
    assert(bitmap && bitmap.hasAlpha);
    for (NSInteger y = 0; y < bitmap.pixelsHigh; y++)
        for (NSInteger x = 0; x < bitmap.pixelsWide; x++)
            assert([bitmap colorAtX:x y:y].alphaComponent == 0);
}

int main(void)
{
    @autoreleasepool {
        struct yasl_private_display *display = yasl_private_display_open(NULL, NULL);
        assert(display);
        struct yasl_private_window_spec spec = {.title = "Cursor test", .size = {320, 240}};
        struct yasl_surface *surface = yasl_private_surface_create_toplevel(display, &spec);
        assert(surface);
        NSWindow *window = (NSWindow *)yasl_private_surface_get_native_handle(surface);
        NSView *view = window.contentView;
        NSEvent *cursor = [NSEvent enterExitEventWithType:NSEventTypeCursorUpdate
            location:NSMakePoint(100, 120) modifierFlags:0 timestamp:0
            windowNumber:window.windowNumber context:nil eventNumber:0 trackingNumber:0 userData:NULL];
        assert(cursor);
        yasl_private_surface_set_cursor(surface, YASL_PRIVATE_CURSOR_HIDDEN);
        [view cursorUpdate:cursor];
        NSCursor *hidden = [NSCursor currentCursor];
        assert_transparent(hidden);

        NSEvent *motion = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
            location:NSMakePoint(100, 120) modifierFlags:0 timestamp:0
            windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:0 pressure:0];
        [view mouseMoved:motion];
        [view cursorUpdate:cursor];
        assert([NSCursor currentCursor] == hidden);
        struct yasl_private_event event;
        bool moved = false;
        struct yasl_private_vector2i start = {0};
        while (yasl_private_display_poll_event(yasl_private_surface_display(surface), &event))
            if (event.type == YASL_PRIVATE_EVENT_POINTER_MOTION) {
                moved = true;
                start = event.pointer.pos;
            }
        assert(moved);

        [[NSCursor arrowCursor] set]; /* Simulate leaving and re-entering. */
        [view cursorUpdate:cursor];
        assert([NSCursor currentCursor] == hidden);
        yasl_private_surface_set_cursor(surface, YASL_PRIVATE_CURSOR_ARROW);
        [view cursorUpdate:cursor];
        assert([NSCursor currentCursor] == [NSCursor arrowCursor]);

        /* A locked pointer hides the cursor, keeps its position, and reports
         * device movement. The test drives the view of a display that
         * yasl_private_display_open made, so it uses the private call. The
         * public call needs a display from yasl_init, and it reports ENODEV
         * without one. */
        assert(yasl_private_surface_set_pointer_mode(surface, YASL_PRIVATE_POINTER_RELATIVE));
        assert(yasl_private_display_caps(display) & YASL_PRIVATE_CAP_RELATIVE_POINTER);
        [view cursorUpdate:cursor];
        assert([NSCursor currentCursor] == hidden);

        CGFloat scale = window.backingScaleFactor < 1.0 ? 1.0 : window.backingScaleFactor;
        MotionSample *sample = [MotionSample new];
        sample.type = NSEventTypeMouseMoved;
        sample.subtype = NSEventSubtypeMouseEvent;
        sample.locationInWindow = NSMakePoint(100, 120);
        sample.deltaX = 3.5;
        sample.deltaY = 2.0;
        [view mouseMoved:(NSEvent *)sample];
        event = next_motion(surface);
        assert(event.pointer.pos.x == start.x && event.pointer.pos.y == start.y);
        assert(fabs(event.pointer.delta.x - 3.5 * scale) < 1e-9);
        assert(fabs(event.pointer.delta.y + 2.0 * scale) < 1e-9);

        /* Unlocked motion reports the change of position again. */
        assert(yasl_private_surface_set_pointer_mode(surface, YASL_PRIVATE_POINTER_NORMAL));
        yasl_private_surface_set_cursor(surface, YASL_PRIVATE_CURSOR_ARROW);
        sample.deltaX = 0;
        sample.deltaY = 0;
        sample.locationInWindow = NSMakePoint(40, 60);
        [view mouseMoved:(NSEvent *)sample];
        event = next_motion(surface);
        assert(fabs(event.pointer.delta.x - (event.pointer.pos.x - start.x)) < 1e-9);
        [sample release];

        /* Every shape selects its system cursor. */
        struct {
            enum yasl_private_cursor shape;
            NSCursor *want;
        } system[] = {
            {YASL_PRIVATE_CURSOR_MOVE, [NSCursor openHandCursor]},
            {YASL_PRIVATE_CURSOR_RESIZE_EW, [NSCursor resizeLeftRightCursor]},
            {YASL_PRIVATE_CURSOR_RESIZE_NS, [NSCursor resizeUpDownCursor]},
        };
        for (size_t i = 0; i < sizeof(system) / sizeof(system[0]); i++) {
            yasl_private_surface_set_cursor(surface, system[i].shape);
            [view cursorUpdate:cursor];
            assert([NSCursor currentCursor] == system[i].want);
        }
        /* macOS has no system diagonal cursor. The two diagonal shapes still
         * select a cursor of their own with an image. */
        yasl_private_surface_set_cursor(surface, YASL_PRIVATE_CURSOR_RESIZE_NWSE);
        [view cursorUpdate:cursor];
        NSCursor *nwse = [NSCursor currentCursor];
        assert(nwse && nwse != [NSCursor arrowCursor] && nwse.image.size.width > 0);
        yasl_private_surface_set_cursor(surface, YASL_PRIVATE_CURSOR_RESIZE_NESW);
        [view cursorUpdate:cursor];
        assert([NSCursor currentCursor] && [NSCursor currentCursor] != nwse);

        yasl_private_surface_destroy(surface);
        yasl_private_display_close(display);
    }
    return 0;
}
