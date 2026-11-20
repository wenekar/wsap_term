# Run a desktop window

This guide shows how to display decoded pixels or a widget tree with `wsap-desktop`.

## Display decoded pixels

Create a `struct ui_image` from your decoder’s output and call `wsap_window_show_image()`:

```c
#include "desktop/window.h"
#include "paint/scene.h"

int show_photo(const char *title, const uint32_t *pixels, int width, int height)
{
    return wsap_window_show_image(title, &(struct ui_image){
        .pixels = pixels,
        .size = { .x = width, .y = height },
    });
}
```

The pixels are a tightly packed array of premultiplied `0xAARRGGBB` words.
The runner borrows them until the window closes. It draws the image at the
top left at one device pixel per image pixel and clips it to the window.
The initial size request uses logical pixels, so a high-DPI window can leave
space around the image. Decoding and pixel conversion are the caller’s
responsibility.

Build the [checkerboard example](../examples/show_pixels.c) with the installed SDK:

```sh
cc -std=c23 examples/show_pixels.c -o show-pixels \
    $(pkg-config --static --cflags --libs wsap-desktop)
./show-pixels
```

For a Meson subproject build, use `dependency('wsap-desktop')` after loading
`subproject('wsap-framework')`.

## Run a widget tree

Initialize a root widget or mount a declarative view, then pass its detached
root to `wsap_window_run()`. Keep the tree alive until the call returns. The
runner mounts and detaches the root; the caller destroys it.

The runner starts the renderer, creates the Yasl loop, opens the window, and
composes the widget scene. It returns when the window closes or the platform
requests quit. Apps that need multiple windows, a shared event loop, or custom
composition can use [`wsap-ui-yasl` directly](ui-screen-guide.md). Custom
composers can call `renderer_draw()` with a widget scene and, optionally, a
cell grid supplied by the app. See [`render/render.h`](../src/render/render.h)
and [`render/grid.h`](../src/render/grid.h).
