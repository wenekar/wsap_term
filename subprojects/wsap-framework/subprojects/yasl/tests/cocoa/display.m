#import <AppKit/AppKit.h>
#include "common/private_native_cocoa.h"

@interface NSView (YaslTestDisplayLink)
- (void)displayLinkFired:(id)link;
@end

/* Exercise the native CPU presenter without depending on the host's cadence. */
void yasl_test_frame_tick(struct yasl_surface *surface)
{
    [yasl_private_cocoa_surface_get_view(surface) displayLinkFired:nil];
}
