#include "common/private_native.h"
#include "common/private_native_cocoa.h"
#include "common/backend.h"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "common/types.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static bool has_event(struct yasl_private_display *d, enum yasl_private_event_type type,
                      struct yasl_surface *surface)
{
    struct yasl_private_event event;
    bool found = false;
    while (yasl_private_display_poll_event(d, &event))
        if (event.type == type && event.surface == surface)
            found = true;
    return found;
}

int main(void)
{
    struct yasl_private_display *d = yasl_private_display_open(NULL, NULL);
    assert(d);
    assert(yasl_private_display_caps(d) & YASL_PRIVATE_CAP_TEXT_INPUT);
    assert(yasl_private_display_caps(d) & YASL_PRIVATE_CAP_TITLEBAR_OVERLAY);
    /* macOS has one system pasteboard and no primary selection. */
    assert(!(yasl_private_display_caps(d) & YASL_PRIVATE_CAP_PRIMARY_SELECTION));
    struct yasl_private_window_spec spec = {.title = "Ownership test", .size = {160, 100}};
    struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &spec);
    assert(s);
    assert(yasl_private_display_get_native_handle(d) == (void *)NSApp);
    assert(yasl_private_cocoa_display_get_application(d) == NSApp);
    NSWindow *window = (NSWindow *)yasl_private_surface_get_native_handle(s);
    assert(yasl_private_cocoa_surface_get_window(s) == window);
    NSView *view = yasl_private_cocoa_surface_get_view(s);
    assert([window isKindOfClass:[NSWindow class]] && view.window == window);
    assert(window.styleMask & NSWindowStyleMaskTitled);
    assert(!(window.styleMask & NSWindowStyleMaskFullSizeContentView));
    assert(!window.titlebarAppearsTransparent);
    assert(NSEqualSizes(view.bounds.size, NSMakeSize(160, 100)));
    assert(NSEqualRects([view convertRect:view.bounds toView:nil], window.contentLayoutRect));
    /* AppKit refits the frame of a window that a window controller owns when
     * a drag takes it to another display, so no controller owns a YASL window. */
    assert(!window.windowController && ![window.delegate isKindOfClass:[NSWindowController class]]);
    assert(!s->titlebar.height && !s->titlebar.controls.width);
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(d, &event)) {}

    /* A window without the overlay has no title bar style to change. */
    assert(!d->ops->surface_set_titlebar_style(s, YASL_PRIVATE_TITLEBAR_TRANSPARENT));
    assert(!window.titlebarAppearsTransparent);

    /* An overlay window: the content covers the title bar, and the requested
     * size includes it. The system draws its title bar until the application
     * makes it transparent. */
    struct yasl_private_window_spec overlay_spec = {
        .title = "Overlay test", .size = {320, 200}, .flags = YASL_PRIVATE_SURFACE_TITLEBAR_OVERLAY,
    };
    struct yasl_surface *overlay = yasl_private_surface_create_toplevel(d, &overlay_spec);
    assert(overlay);
    NSWindow *overlay_window = (NSWindow *)yasl_private_surface_get_native_handle(overlay);
    NSView *overlay_view = yasl_private_cocoa_surface_get_view(overlay);
    assert(overlay_window.styleMask & NSWindowStyleMaskFullSizeContentView);
    assert(!overlay_window.titlebarAppearsTransparent);
    assert(overlay_window.titleVisibility == NSWindowTitleVisible);
    assert(NSEqualSizes(overlay_view.bounds.size, NSMakeSize(320, 200)));
    struct yasl_private_titlebar tb = overlay->titlebar;
    assert(tb.height > 0 && tb.height < 100);
    assert(tb.controls.width > 0 && tb.controls.height > 0 && tb.controls.x >= 0);
    assert(tb.controls.y >= 0 && tb.controls.y + tb.controls.height <= tb.height);
    assert(!(overlay->state & YASL_PRIVATE_STATE_CSD));
    while (yasl_private_display_poll_event(d, &event)) {}

    /* NSWindow sends a press to the frame view's hit test result. The opaque
     * system title bar takes the presses in its band. A transparent title bar
     * leaves the band right of its title text to the view, and a title bar
     * with only buttons leaves the view all of the band but the buttons. The
     * title bar keeps its height with every style. */
    NSView *frame_view = overlay_view.superview;
    CGFloat band_y = 200 - tb.height / 2.0;
    NSPoint band = [frame_view convertPoint:NSMakePoint(150, band_y) fromView:nil];
    NSPoint band_end = [frame_view convertPoint:NSMakePoint(300, band_y) fromView:nil];
    NSPoint button = [frame_view convertPoint:NSMakePoint(tb.controls.x + 6, band_y) fromView:nil];
    assert([frame_view hitTest:band_end] != overlay_view);
    assert(d->ops->surface_set_titlebar_style(overlay, YASL_PRIVATE_TITLEBAR_TRANSPARENT));
    assert(overlay_window.titlebarAppearsTransparent);
    assert(overlay_window.titleVisibility == NSWindowTitleVisible);
    assert([frame_view hitTest:band_end] == overlay_view);
    assert([frame_view hitTest:button] != overlay_view);
    assert(d->ops->surface_set_titlebar_style(overlay, YASL_PRIVATE_TITLEBAR_BUTTONS));
    assert(overlay_window.titlebarAppearsTransparent);
    assert(overlay_window.titleVisibility == NSWindowTitleHidden);
    assert([frame_view hitTest:band] == overlay_view);
    assert([frame_view hitTest:button] != overlay_view);
    assert(overlay->titlebar.height == tb.height);

    /* A press in a title bar area reaches the application as no event; a
     * double click does what the system setting asks, here nothing. */
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults setObject:@"None" forKey:@"AppleActionOnDoubleClick"];
    struct yasl_private_area area = {.x = 200, .y = 0, .width = 120, .height = tb.height};
    overlay->titlebar_areas = &area;
    overlay->titlebar_area_count = 1;
    @autoreleasepool {
        NSEvent *outside = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
            location:NSMakePoint(150, band_y) modifierFlags:0 timestamp:0
            windowNumber:overlay_window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
        NSEvent *inside = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
            location:NSMakePoint(260, band_y) modifierFlags:0 timestamp:0
            windowNumber:overlay_window.windowNumber context:nil eventNumber:0 clickCount:2 pressure:1];
        NSEvent *release = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
            location:NSMakePoint(260, band_y) modifierFlags:0 timestamp:0
            windowNumber:overlay_window.windowNumber context:nil eventNumber:0 clickCount:2 pressure:0];
        assert(![overlay_view acceptsFirstMouse:outside]);
        assert([overlay_view acceptsFirstMouse:inside]);
        [overlay_view mouseDown:outside];
        assert(has_event(d, YASL_PRIVATE_EVENT_POINTER_DOWN, overlay));
        [overlay_view mouseDown:inside];
        [overlay_view mouseUp:release];
        while (yasl_private_display_poll_event(d, &event))
            assert(event.surface != overlay || (event.type != YASL_PRIVATE_EVENT_POINTER_DOWN &&
                                                event.type != YASL_PRIVATE_EVENT_POINTER_UP));
    }
    overlay->titlebar_areas = NULL;
    overlay->titlebar_area_count = 0;
    [defaults removeObjectForKey:@"AppleActionOnDoubleClick"];
    yasl_private_surface_destroy(overlay);
    while (yasl_private_display_poll_event(d, &event)) {}

    struct yasl_private_window_spec gpu_spec = {
        .flags = YASL_PRIVATE_SURFACE_HIDDEN | YASL_PRIVATE_SURFACE_UNDECORATED |
                 YASL_PRIVATE_SURFACE_FIXED_SIZE,
        .external_gpu = true,
        .title = "External GPU test",
        .size = {160, 100},
    };
    struct yasl_surface *gpu = yasl_private_surface_create_toplevel(d, &gpu_spec);
    assert(gpu && gpu->presenter == YASL_PRIVATE_PRESENTER_NONE);
    CAMetalLayer *metal = (CAMetalLayer *)d->ops->surface_get_metal_layer(gpu);
    assert(metal && [metal isKindOfClass:[CAMetalLayer class]] && !metal.device);
    NSWindow *hidden = (NSWindow *)yasl_private_surface_get_native_handle(gpu);
    assert(!hidden.isVisible && !(hidden.styleMask & NSWindowStyleMaskTitled));
    /* A titled window keeps its system title bar. An undecorated window has
     * none, so the application draws the frame. */
    assert(!(s->state & YASL_PRIVATE_STATE_CSD));
    assert(gpu->state & YASL_PRIVATE_STATE_CSD);
    yasl_private_surface_destroy(gpu);

    assert(!d->ops->surface_create_gl && !d->ops->gl_make_current &&
           !d->ops->gl_get_proc_address && !d->ops->gl_swap_interval && !d->ops->gl_swap_buffers);

    assert(yasl_private_surface_set_size(s, (struct yasl_private_vector2i){180, 110}));
    assert(NSEqualSizes(view.bounds.size, NSMakeSize(180, 110)));
    assert(yasl_private_surface_set_fullscreen(s, false));
    assert(yasl_private_surface_set_maximized(s, false));
    assert(!yasl_private_surface_set_size(s, (struct yasl_private_vector2i){0, 100}));
    assert(errno == EINVAL);
    struct yasl_private_popup_spec popup_spec = {

        .anchor_edges = 4, .gravity = 3, .constraints = 63,
        .size = {80, 40}, .anchor = {10, 10, 2, 2},
    };
    struct yasl_surface *popup = yasl_private_surface_create_popup(s, &popup_spec);
    assert(popup);
    assert(popup->state & YASL_PRIVATE_STATE_CSD);
    popup_spec.anchor_edges = 8;
    popup_spec.gravity = 5;
    assert(yasl_private_surface_popup_reposition(popup, &popup_spec));
    assert(!yasl_private_surface_set_size(popup, (struct yasl_private_vector2i){80, 40}));
    assert(errno == EINVAL);
    assert(!yasl_private_surface_set_fullscreen(popup, true) && errno == EINVAL);
    assert(!yasl_private_surface_set_maximized(popup, true) && errno == EINVAL);
    assert(!yasl_private_surface_minimize(popup) && errno == EINVAL);

    /* A press in another window of the application dismisses the popup, and a
     * press in the popup keeps it. */
    NSWindow *popup_window = (NSWindow *)yasl_private_surface_get_native_handle(popup);
    @autoreleasepool {
        NSEvent *in_popup = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
            location:NSMakePoint(4, 4) modifierFlags:0 timestamp:0
            windowNumber:popup_window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
        NSEvent *in_parent = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
            location:NSMakePoint(20, 20) modifierFlags:0 timestamp:0
            windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
        [popup_window sendEvent:in_popup];
        assert(!has_event(d, YASL_PRIVATE_EVENT_WINDOW_CLOSE, popup));
        [window sendEvent:in_parent];
        assert(has_event(d, YASL_PRIVATE_EVENT_WINDOW_CLOSE, popup));
    }
    yasl_private_surface_destroy(popup);
    while (yasl_private_display_poll_event(d, &event)) {}

    s->text_input_active = true;
    s->text_surrounding = strdup("é漢");
    s->text_length = strlen(s->text_surrounding);
    s->text_begin = 0;
    s->text_end = s->text_length;
    assert(d->ops->surface_update_text_input(s));
    id<NSTextInputClient> text_input = (id<NSTextInputClient>)yasl_private_cocoa_surface_get_view(s);
    [text_input setMarkedText:@"é漢" selectedRange:NSMakeRange(1, 1)
              replacementRange:NSMakeRange(NSNotFound, 0)];
    assert(strcmp(yasl_private_surface_ime_preedit(s), "é漢") == 0);
    int cursor_begin = 0, cursor_end = 0;
    assert(yasl_private_surface_ime_preedit_cursor(s, &cursor_begin, &cursor_end));
    assert(cursor_begin == 2 && cursor_end == 5);
    [text_input unmarkText];
    assert(yasl_private_surface_ime_preedit(s) == NULL);
    assert(!yasl_private_surface_ime_preedit_cursor(s, &cursor_begin, &cursor_end));
    assert(yasl_private_display_poll_event(d, &event));
    assert(event.type == YASL_PRIVATE_EVENT_COMPOSITION && event.composition.active);
    assert(strcmp(event.composition.data, "é漢") == 0);
    assert(event.composition.begin == 2 && event.composition.end == 5);
    assert(yasl_private_display_poll_event(d, &event));
    assert(event.type == YASL_PRIVATE_EVENT_COMPOSITION && !event.composition.active);
    assert(event.composition.begin == -1 && event.composition.end == -1);
    while (yasl_private_display_poll_event(d, &event)) {}

    uint64_t request_ids[600];
    @autoreleasepool {
        for (int i = 0; i < 600; i++) {
            char text[32];
            snprintf(text, sizeof(text), "paste-%d", i);
            assert(yasl_private_clipboard_set(d, YASL_PRIVATE_CLIPBOARD, text));
            request_ids[i] = yasl_private_clipboard_request(s, YASL_PRIVATE_CLIPBOARD);
            assert(request_ids[i] != 0);
            memset(text, 'x', sizeof(text));
        }
    }
    /* All NSString/UTF8String temporaries have now expired. */
    for (int i = 0; i < 600; i++) {
        char text[32];
        snprintf(text, sizeof(text), "paste-%d", i);
        assert(yasl_private_display_poll_event(d, &event));
        assert(event.type == YASL_PRIVATE_EVENT_PASTE && event.paste.request_id == request_ids[i]);
        assert(event.paste.status == YASL_PRIVATE_TRANSFER_OK && strcmp(event.paste.data, text) == 0);
    }
    const char *last = event.paste.data;
    assert(!yasl_private_display_poll_event(d, &event));
    @autoreleasepool { yasl_private_clipboard_set(d, YASL_PRIVATE_CLIPBOARD, "new selection"); }
    assert(strcmp(last, "paste-599") == 0);
    assert(!yasl_private_clipboard_set(d, YASL_PRIVATE_CLIPBOARD, "\xc3"));
    assert(errno == EILSEQ);
    assert(yasl_private_clipboard_set(d, YASL_PRIVATE_CLIPBOARD, ""));
    uint64_t empty_id = yasl_private_clipboard_request(s, YASL_PRIVATE_CLIPBOARD);
    assert(empty_id && yasl_private_display_poll_event(d, &event));
    assert(event.type == YASL_PRIVATE_EVENT_PASTE && event.paste.request_id == empty_id);
    assert(event.paste.status == YASL_PRIVATE_TRANSFER_OK && !event.paste.len && !event.paste.data[0]);
    [NSPasteboard.generalPasteboard clearContents];
    uint64_t unavailable_id = yasl_private_clipboard_request(s, YASL_PRIVATE_CLIPBOARD);
    assert(unavailable_id && yasl_private_display_poll_event(d, &event));
    assert(event.type == YASL_PRIVATE_EVENT_PASTE && event.paste.request_id == unavailable_id);
    assert(event.paste.status == YASL_PRIVATE_TRANSFER_UNAVAILABLE && !event.paste.len);
    yasl_private_surface_destroy(s);
    yasl_private_display_close(d);
    return 0;
}
