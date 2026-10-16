#import <AppKit/AppKit.h>
#include <common/private_native.h>
#include <assert.h>

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
        while (yasl_private_display_poll_event(yasl_private_surface_display(surface), &event))
            if (event.type == YASL_PRIVATE_EVENT_POINTER_MOTION)
                moved = true;
        assert(moved);

        [[NSCursor arrowCursor] set]; /* Simulate leaving and re-entering. */
        [view cursorUpdate:cursor];
        assert([NSCursor currentCursor] == hidden);
        yasl_private_surface_set_cursor(surface, YASL_PRIVATE_CURSOR_ARROW);
        [view cursorUpdate:cursor];
        assert([NSCursor currentCursor] == [NSCursor arrowCursor]);
        yasl_private_surface_destroy(surface);
        yasl_private_display_close(display);
    }
    return 0;
}
