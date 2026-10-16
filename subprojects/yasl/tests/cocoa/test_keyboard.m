#import <AppKit/AppKit.h>
#include <common/private_native.h>
#include <common/backend.h>
#include <assert.h>
#include <math.h>
#include <string.h>

/* Reject character queries on modifier-only events, as AppKit does. */
@interface InputSample : NSObject
@property NSEventType type;
@property unsigned short keyCode;
@property NSEventModifierFlags modifierFlags;
@property(copy) NSString *label;
@property(copy) NSString *shiftedLabel;
@property NSPoint locationInWindow;
@property CGFloat scrollingDeltaX;
@property CGFloat scrollingDeltaY;
@property BOOL hasPreciseScrollingDeltas;
@end

@implementation InputSample
- (NSString *)charactersByApplyingModifiers:(NSEventModifierFlags)modifiers
{
    assert(self.type == NSEventTypeKeyDown || self.type == NSEventTypeKeyUp);
    return modifiers & NSEventModifierFlagShift ? self.shiftedLabel : self.label;
}
- (void)dealloc
{
    [_label release];
    [_shiftedLabel release];
    [super dealloc];
}
@end

static struct yasl_private_event next(struct yasl_surface *surface, enum yasl_private_event_type type)
{
    struct yasl_private_event event;
    assert(yasl_private_display_poll_event(yasl_private_surface_display(surface), &event));
    assert(event.type == type && event.surface == surface);
    return event;
}

static void empty(struct yasl_surface *surface)
{
    struct yasl_private_event event;
    assert(!yasl_private_display_poll_event(yasl_private_surface_display(surface), &event));
}

int main(void)
{
    @autoreleasepool {
        struct yasl_private_display *display = yasl_private_display_open(NULL, NULL);
        assert(display);
        struct yasl_private_window_spec spec = {.title = "Keyboard test", .size = {320, 240}};
        struct yasl_surface *surface = yasl_private_surface_create_toplevel(display, &spec);
        assert(surface);
        surface->text_input_active = true;
        surface->text_surrounding = strdup("A😀B");
        surface->text_length = strlen(surface->text_surrounding);
        surface->text_begin = 1;
        surface->text_end = 5;
        surface->text_revision = 7;
        surface->text_generation = 3;
        assert(display->ops->surface_update_text_input(surface));
        NSWindow *window = (NSWindow *)yasl_private_surface_get_native_handle(surface);
        NSView<NSTextInputClient> *view = (id)window.contentView;
        struct yasl_private_event event;
        while (yasl_private_display_poll_event(yasl_private_surface_display(surface), &event)) {}

        InputSample *sample = [InputSample new];
        NSEvent *native = (NSEvent *)sample;
        sample.type = NSEventTypeKeyUp;
        sample.keyCode = 0; /* US A, even with a different active layout. */
        sample.label = @"q";
        sample.shiftedLabel = @"Q";
        [view keyUp:native];
        event = next(surface, YASL_PRIVATE_EVENT_KEY);
        assert(event.key.key == YASL_KEY_A && event.key.native_code == 0);
        assert(event.key.action == YASL_KEY_RELEASE);
        assert(event.key.codepoint == 'q' && event.key.shifted == 'Q');
        empty(surface);
        sample.label = @"😀"; /* One scalar, two UTF-16 units. */
        sample.shiftedLabel = @"ab";
        [view keyUp:native];
        event = next(surface, YASL_PRIVATE_EVENT_KEY);
        assert(event.key.codepoint == 0x1f600 && event.key.shifted == 0);
        sample.type = NSEventTypeFlagsChanged;
        sample.keyCode = 56; /* Left Shift. */
        sample.modifierFlags = NSEventModifierFlagShift | 0x0002;
        [view flagsChanged:native];
        event = next(surface, YASL_PRIVATE_EVENT_KEY);
        assert(event.key.key == YASL_KEY_LEFT_SHIFT && event.key.action == YASL_KEY_PRESS);
        assert(event.key.mods == YASL_PRIVATE_MOD_SHIFT && !event.key.codepoint && !event.key.shifted);
        sample.modifierFlags = 0;
        [view flagsChanged:native];
        assert(next(surface, YASL_PRIVATE_EVENT_KEY).key.action == YASL_KEY_RELEASE);

        /* Native text services own preedit and commits. Marked text is not an
         * insertion, and commits retain all UTF-8 bytes, including non-BMP. */
        [view setMarkedText:@"é" selectedRange:NSMakeRange(1, 0)
            replacementRange:NSMakeRange(1, 2)];
        assert(strcmp(yasl_private_surface_ime_preedit(surface), "é") == 0);
        event = next(surface, YASL_PRIVATE_EVENT_COMPOSITION);
        assert(event.composition.active && !strcmp(event.composition.data, "é"));
        empty(surface);
        [view insertText:@"😀é" replacementRange:NSMakeRange(NSNotFound, 0)];
        event = next(surface, YASL_PRIVATE_EVENT_TEXT_EDIT);
        assert(event.text_edit.len == strlen("😀é") && strcmp(event.text_edit.data, "😀é") == 0);
        assert(event.text_edit.begin == 1 && event.text_edit.end == 5);
        assert(event.text_edit.revision == 7 && event.text_edit.generation == 3);
        assert(!yasl_private_surface_ime_preedit(surface));
        assert(!next(surface, YASL_PRIVATE_EVENT_COMPOSITION).composition.active);
        empty(surface);

        /* Exercise the keyDown -> interpretKeyEvents -> insertText route with
         * an actual AppKit key event rather than the strict event double. */
        NSEvent *press = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
            modifierFlags:0 timestamp:0 windowNumber:window.windowNumber context:nil
            characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
        [view keyDown:press];
        assert(next(surface, YASL_PRIVATE_EVENT_KEY).key.key == YASL_KEY_A);
        event = next(surface, YASL_PRIVATE_EVENT_TEXT_EDIT);
        assert(event.text_edit.len == 1 && event.text_edit.data[0] == 'a');
        assert(event.text_edit.begin == 1 && event.text_edit.end == 5);
        empty(surface);

        surface->text_end = 1;
        [view doCommandBySelector:@selector(deleteBackward)];
        event = next(surface, YASL_PRIVATE_EVENT_TEXT_EDIT);
        assert(event.text_edit.len == 0 && event.text_edit.begin == 0 && event.text_edit.end == 1);
        empty(surface);

        sample.type = NSEventTypeScrollWheel;
        sample.hasPreciseScrollingDeltas = YES;
        sample.scrollingDeltaX = -2.5;
        sample.scrollingDeltaY = 1.25;
        [view scrollWheel:native];
        event = next(surface, YASL_PRIVATE_EVENT_POINTER_SCROLL);
        assert(event.pointer.scroll.x == 0.25 && event.pointer.scroll.y == -0.125);
        sample.hasPreciseScrollingDeltas = NO;
        sample.scrollingDeltaX = 1;
        sample.scrollingDeltaY = -1;
        [view scrollWheel:native];
        event = next(surface, YASL_PRIVATE_EVENT_POINTER_SCROLL);
        assert(event.pointer.scroll.x == -1 && event.pointer.scroll.y == 1);
        sample.scrollingDeltaX = NAN;
        [view scrollWheel:native];
        empty(surface);
        [sample release];
        yasl_private_surface_destroy(surface);
        yasl_private_display_close(display);
    }
    return 0;
}
