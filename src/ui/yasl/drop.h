#ifndef UI_YASL_DROP_H
#define UI_YASL_DROP_H

#include <stddef.h>

/* Writes an image to a new private file and returns the file path. The
 * `mime` argument picks the file extension. The module owns the path. It
 * stays valid until drop_cleanup runs, so the caller never frees it. Returns
 * NULL when the payload is empty or over 64 MiB, or when the save fails. Call
 * it on the loop thread. */
const char *drop_save_image(const char *mime, const char *data, size_t len);

/* Removes every file that drop_save_image wrote. Call it once on the normal
 * exit path, after the last window closes. */
void drop_cleanup(void);

#endif
