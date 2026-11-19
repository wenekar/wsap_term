#ifndef UI_YASL_DROP_H
#define UI_YASL_DROP_H

#include <stdbool.h>
#include <stddef.h>

#include "ui/surface.h"

/* The drag and drop helpers of the window layer. A drop carries its type and
 * its data. It never becomes a file. */

/* Returns true when `mime` names a text type. */
bool drop_mime_is_text(const char *mime);

/* Returns true when `mime` names a URI list. An empty type counts as one,
 * because yasl reports it that way. */
bool drop_mime_is_uri_list(const char *mime);

/* The old file path of a dropped image. A drop writes no file now, so it
 * returns NULL for every payload. Drop it when the native drop path sends a
 * MIME drop. Call it on the loop thread. */
const char *drop_save_image(const char *mime, const char *data, size_t len);

/* Does nothing: a drop writes no file. Call it on the normal exit path for
 * the same reason as before. */
void drop_cleanup(void);

#endif
