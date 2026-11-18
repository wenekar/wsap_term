#define _GNU_SOURCE

#include "ui/yasl/drop.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Returns the length of the major type of `mime`, up to its slash. A type
 * with no slash has a major type that is the whole string. */
static size_t major_length(const char *mime)
{
    const char *slash = strchr(mime, '/');
    return slash ? (size_t)(slash - mime) : strlen(mime);
}

/* Returns true when `offer` gives type `mime`. An offer with a star gives
 * every type with its major type. */
static bool offer_matches(const char *offer, const char *mime)
{
    if (!strcmp(offer, mime))
        return true;
    size_t n = major_length(offer);
    if (offer[n] != '/' || offer[n + 1] != '*' || offer[n + 2])
        return false;
    return major_length(mime) == n && !strncmp(offer, mime, n);
}

const char *drop_pick_mime(const char *const *offered, size_t offered_count,
                           const char *const *accepted, size_t accepted_count)
{
    if (!offered || !accepted)
        return NULL;
    /* The first acceptable type wins, because both lists are best first. */
    for (size_t i = 0; i < accepted_count; i++) {
        const char *want = accepted[i];
        if (!want || !want[0])
            continue;
        for (size_t j = 0; j < offered_count; j++) {
            const char *offer = offered[j];
            if (offer && offer_matches(offer, want))
                return want;
        }
    }
    return NULL;
}

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

bool drop_hover_retarget(struct drop_hover *hover, const void *target)
{
    if (!hover)
        return false;
    bool left = hover->active && hover->target != target;
    hover->target = target;
    hover->active = target != NULL;
    return left;
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
