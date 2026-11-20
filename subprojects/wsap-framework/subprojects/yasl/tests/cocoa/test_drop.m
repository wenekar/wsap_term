#import <AppKit/AppKit.h>
#include <common/private_native.h>
#include <common/private_native_cocoa.h>
#include <common/backend.h>
#include <assert.h>
#include <math.h>
#include <string.h>

/* A drag that carries one pasteboard. The view reads only these methods. */
@interface DragSample : NSObject
@property(nonatomic, retain) NSPasteboard *pasteboard;
@property NSPoint location;
@end

@implementation DragSample
- (NSPasteboard *)draggingPasteboard { return self.pasteboard; }
- (NSPoint)draggingLocation { return self.location; }
@end

/* The view takes the drag of a real session. The test sends an untyped sender
 * with the two methods that the view reads. */
@interface NSView (DragDouble)
- (NSDragOperation)draggingEntered:(id)sender;
- (void)draggingExited:(id)sender;
- (BOOL)performDragOperation:(id)sender;
@end

static struct yasl_private_event next_at(struct yasl_surface *surface,
                                         enum yasl_private_event_type type, int line)
{
    struct yasl_private_event event;
    assert(yasl_private_display_poll_event(yasl_private_surface_display(surface), &event));
    if (event.type != type)
        fprintf(stderr, "Line %d: expected event %d, received %d\n", line, type, event.type);
    assert(event.type == type && event.surface == surface);
    return event;
}

#define next(surface, type) next_at(surface, type, __LINE__)

static void empty(struct yasl_surface *surface)
{
    struct yasl_private_event event;
    assert(!yasl_private_display_poll_event(yasl_private_surface_display(surface), &event));
}

static void set_types(struct yasl_surface *surface, const char *const *types, size_t count)
{
    /* The test drives the view of a display that yasl_private_display_open
     * made, so it uses the private call. The public call needs a display from
     * yasl_init, and it reports ENODEV without one. */
    assert(yasl_private_surface_set_drop_types(surface, types, count));
}

int main(void)
{
    @autoreleasepool {
        struct yasl_private_display *display = yasl_private_display_open(NULL, NULL);
        assert(display);
        struct yasl_private_window_spec spec = {.title = "Drop test", .size = {320, 240}};
        struct yasl_surface *surface = yasl_private_surface_create_toplevel(display, &spec);
        assert(surface);
        NSWindow *window = (NSWindow *)yasl_private_surface_get_native_handle(surface);
        NSView *view = yasl_private_cocoa_surface_get_view(surface);
        struct yasl_private_event event;
        while (yasl_private_display_poll_event(display, &event)) {}

        NSPasteboard *text = [NSPasteboard pasteboardWithUniqueName];
        assert(text);
        [text declareTypes:@[NSPasteboardTypeString] owner:nil];
        assert([text setString:@"dropped" forType:NSPasteboardTypeString]);
        DragSample *sample = [DragSample new];
        sample.pasteboard = text;
        sample.location = NSMakePoint(10, 10);

        /* Without a call the surface takes file drops only. */
        assert([view draggingEntered:sample] == NSDragOperationNone);
        empty(surface);

        /* A drag that offers no configured type is refused. */
        const char *image[] = {"image/png"};
        set_types(surface, image, 1);
        assert([view draggingEntered:sample] == NSDragOperationNone);
        empty(surface);

        /* A configured type accepts the drag and names the type it would take. */
        const char *plain[] = {"text/plain"};
        set_types(surface, plain, 1);
        assert([view draggingEntered:sample] == NSDragOperationCopy);
        event = next(surface, YASL_PRIVATE_EVENT_DROP_HOVER);
        assert(event.drop.action == YASL_PRIVATE_DROP_OVER);
        assert(!strcmp(event.drop.mime, "text/plain"));
        empty(surface);

        /* The drop delivers the bytes of the picked type, with the release
         * position in surface-relative device pixels. */
        assert([view performDragOperation:sample]);
        event = next(surface, YASL_PRIVATE_EVENT_DROP);
        assert(event.text.len == strlen("dropped"));
        assert(!strcmp(event.text.data, "dropped"));
        assert(!strcmp(event.text.mime, "text/plain"));
        CGFloat scale = window.backingScaleFactor < 1.0 ? 1.0 : window.backingScaleFactor;
        assert(event.text.pos.x == (int)floor(10 * scale));
        assert(event.text.pos.y == (int)floor((view.bounds.size.height - 10) * scale));
        event = next(surface, YASL_PRIVATE_EVENT_DROP_HOVER);
        assert(event.drop.action == YASL_PRIVATE_DROP_LEAVE);
        empty(surface);

        /* A major type with a star takes every subtype. */
        const char *any_text[] = {"text/*"};
        set_types(surface, any_text, 1);
        assert([view draggingEntered:sample] == NSDragOperationCopy);
        event = next(surface, YASL_PRIVATE_EVENT_DROP_HOVER);
        assert(!strcmp(event.drop.mime, "text/plain"));
        [view draggingExited:sample];
        event = next(surface, YASL_PRIVATE_EVENT_DROP_HOVER);
        assert(event.drop.action == YASL_PRIVATE_DROP_LEAVE);
        empty(surface);

        set_types(surface, NULL, 0);
        [sample release];
        yasl_private_surface_destroy(surface);
        yasl_private_display_close(display);
    }
    return 0;
}
