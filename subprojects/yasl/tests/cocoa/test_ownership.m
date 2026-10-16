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

int main(void)
{
    struct yasl_private_display *d = yasl_private_display_open(NULL, NULL);
    assert(d);
    assert(yasl_private_display_caps(d) & YASL_PRIVATE_CAP_TEXT_INPUT);
    struct yasl_private_window_spec spec = {.title = "Ownership test", .size = {160, 100}};
    struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &spec);
    assert(s);
    assert(yasl_private_display_get_native_handle(d) == (void *)NSApp);
    assert(yasl_private_cocoa_display_get_application(d) == NSApp);
    NSWindow *window = (NSWindow *)yasl_private_surface_get_native_handle(s);
    assert(yasl_private_cocoa_surface_get_window(s) == window);
    assert(yasl_private_cocoa_surface_get_view(s) == window.contentView);
    assert([window isKindOfClass:[NSWindow class]] && window.contentView);
    struct yasl_private_event event;
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
    assert(!hidden.isVisible && hidden.styleMask == NSWindowStyleMaskBorderless);
    yasl_private_surface_destroy(gpu);

    assert(!d->ops->surface_create_gl && !d->ops->gl_make_current &&
           !d->ops->gl_get_proc_address && !d->ops->gl_swap_interval && !d->ops->gl_swap_buffers);

    assert(yasl_private_surface_set_size(s, (struct yasl_private_vector2i){180, 110}));
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
    popup_spec.anchor_edges = 8;
    popup_spec.gravity = 5;
    assert(yasl_private_surface_popup_reposition(popup, &popup_spec));
    assert(!yasl_private_surface_set_size(popup, (struct yasl_private_vector2i){80, 40}));
    assert(errno == EINVAL);
    assert(!yasl_private_surface_set_fullscreen(popup, true) && errno == EINVAL);
    assert(!yasl_private_surface_set_maximized(popup, true) && errno == EINVAL);
    assert(!yasl_private_surface_minimize(popup) && errno == EINVAL);
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
    [text_input unmarkText];
    assert(yasl_private_surface_ime_preedit(s) == NULL);
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
