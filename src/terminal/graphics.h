#ifndef TERMINAL_GRAPHICS_H
#define TERMINAL_GRAPHICS_H

#include "paint/scene.h"
#include "terminal/terminal.h"
#include <wsap_vtcore/wsap_vtcore.h>

struct terminal;
struct terminal_graphics;

enum wsap_vtcore_graphics_result terminal_graphics_receive(
    void *ud, const struct wsap_vtcore_graphics *cmd, struct wsap_vtcore_vector2i *extent);
void terminal_graphics_destroy(struct terminal_graphics *g);
/* Captures the images for the current text view while the core is locked. */
struct terminal_graphics_layers terminal_graphics_capture(struct terminal *t, struct rect2i bounds,
                                                            int rows, bool live_view);

#endif
