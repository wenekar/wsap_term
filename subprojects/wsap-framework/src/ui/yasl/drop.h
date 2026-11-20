#ifndef UI_YASL_DROP_H
#define UI_YASL_DROP_H

#include <stdbool.h>
#include <stddef.h>

#include "ui/surface.h"

/* The drag and drop helpers of the window layer. A drop carries its type and
 * its data. It never becomes a file. */

/* Returns NULL for every payload: a drop writes no file. Call it on the loop
 * thread. */
const char *drop_save_image(const char *mime, const char *data, size_t len);

/* Does nothing: a drop writes no file, so no temporary file is left to
 * remove. The app calls it on the normal exit path. */
void drop_cleanup(void);

#endif
