#define _GNU_SOURCE

#include "ui/yasl/drop.h"

#include <stdio.h>

/* Images and other binary payloads stay in memory. A dropped image reaches
 * the widget as a DROP event of its own type, and the widget keeps or shows
 * the bytes. This function is the old file path. It refuses every payload,
 * because a drop no longer writes a file. It stays until the native drop path
 * sends a MIME drop. */
const char *drop_save_image(const char *mime, const char *data, size_t len)
{
    (void)data;
    fprintf(stderr, "drop: type %s of %zu bytes stays in memory, not in a file\n",
            mime && mime[0] ? mime : "(none)", len);
    return NULL;
}

void drop_cleanup(void)
{
    /* A drop writes no file, so there is nothing to remove. */
}
