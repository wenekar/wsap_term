#ifndef APP_THUMBNAIL_H
#define APP_THUMBNAIL_H

#include <stdbool.h>

#include "paint/scene.h"

struct app;
struct font_instance;
struct wsap_vtcore;

bool app_thumbnail_capture(void *ud, void **state, struct font_instance *font,
                           struct wsap_vtcore *term, int width, struct ui_image *image);
void app_thumbnail_release(void *state);

#endif
