#ifndef WSAP_DESKTOP_WINDOW_H
#define WSAP_DESKTOP_WINDOW_H

#include "util/geom.h"

struct ui_image;
struct ui_widget;

/* Options for a standalone window. */
struct wsap_window_spec {
    /* An initialized, detached widget. The caller retains ownership. */
    struct ui_widget *root;
    /* Window title, or NULL to use the application identifier if set. */
    const char *title;
    /* Initial size in logical pixels. Zero selects the default on each axis. */
    struct vector2i size;
};

/* Opens a window and renders its widget tree until it closes or the platform
 * requests quit. Returns EXIT_SUCCESS on normal exit or EXIT_FAILURE if
 * setup or the event loop fails. `spec` and `spec->root` must be non-NULL,
 * and the size must be nonnegative. Keep the title and widget tree alive
 * until this call returns. The runner detaches the tree without destroying it
 * and releases its renderer and font-system reference before returning.
 * Call on the main thread without an active Yasl display. */
int wsap_window_run(const struct wsap_window_spec *spec);

/* Displays an already decoded image at one device pixel per image pixel.
 * Requests the image dimensions as the initial window size in logical pixels.
 * Clips pixels that do not fit. Borrows `title` and the image pixels until it
 * returns. A NULL image, missing pixels, or a nonpositive dimension fails.
 * Uses the title, thread, display, and exit status rules of wsap_window_run(). */
int wsap_window_show_image(const char *title, const struct ui_image *image);

#endif
