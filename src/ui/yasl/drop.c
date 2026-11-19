#define _GNU_SOURCE

#include "ui/yasl/drop.h"

#include <stdio.h>
#include <strings.h>

bool drop_mime_is_text(const char *mime)
{
    if (!mime)
        return false;
    return !strcasecmp(mime, "text/plain;charset=utf-8") ||
           !strcasecmp(mime, "UTF8_STRING") || !strcasecmp(mime, "text/plain");
}

bool drop_mime_is_uri_list(const char *mime)
{
    return mime && (!mime[0] || !strcasecmp(mime, "text/uri-list"));
}

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
