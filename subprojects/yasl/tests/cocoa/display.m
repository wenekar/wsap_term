#import <AppKit/AppKit.h>
#include "common/private_native.h"

@interface NSView (YaslTestDisplayLink)
- (void)displayLinkFired:(id)link;
@end

/* Exercise the native CPU presenter without depending on the host's cadence. */
void yasl_test_frame_tick(struct yasl_surface *surface)
{
    NSWindow *window = (NSWindow *)yasl_private_surface_get_native_handle(surface);
    [window.contentView displayLinkFired:nil];
}
