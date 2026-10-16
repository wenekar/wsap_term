#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include "ui/yasl/metal.h"

#include "yasl/yasl.h"

void *window_metal_attach(struct yasl_surface *s, void *device)
{
    if (!device)
        return NULL;
    CAMetalLayer *layer = (CAMetalLayer *)yasl_metal_get_layer(s);
    if (!layer)
        return NULL;
    /* yasl sizes the layer and picks BGRA8; the renderer draws with this
     * device into that format. */
    layer.device = (id<MTLDevice>)device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    return layer;
}

/* A live resize presents in the Core Animation transaction, so the frame
 * shows together with the new window size. */
static bool in_live_resize(struct yasl_surface *s)
{
    struct yasl_native native = {0};
    if (yasl_surface_get_native(s, &native) != 0 || !native.ns_view)
        return false;
    return ((NSView *)native.ns_view).inLiveResize;
}

bool window_metal_draw(void *layer, struct yasl_surface *s,
                       window_metal_draw_fn draw, void *ud)
{
    CAMetalLayer *metal = (CAMetalLayer *)layer;
    bool drew = false;
    bool transaction = in_live_resize(s);
    @autoreleasepool {
        metal.presentsWithTransaction = transaction;
        id<CAMetalDrawable> drawable = [metal nextDrawable];
        if (drawable) {
            id<MTLTexture> texture = drawable.texture;
            struct window_metal_frame frame = {
                .drawable = drawable,
                .size = {
                    .x = (int)texture.width,
                    .y = (int)texture.height,
                },
                .transaction = metal.presentsWithTransaction,
            };
            draw(&frame, ud);
            drew = true;
        }
    }
    /* Send the resize frame before another callback asks for a drawable. */
    if (drew && transaction)
        [CATransaction flush];
    return drew;
}
